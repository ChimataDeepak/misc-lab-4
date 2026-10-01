// SPDX-License-Identifier: GPL-2.0
/*
 * fake_mhi_modem.ko -- a QMI control channel exposed as a character device.
 *
 * WHAT IT MODELS
 *   On a real PCIe (M.2) Qualcomm modem the stack is:
 *     mhi_pci_generic (PCI driver: BARs, MSI, ring buffers in host RAM)
 *       -> MHI bus (drivers/bus/mhi): channels such as "QMI", "IP_HW0"
 *         -> mhi_wwan_ctrl (drivers/net/wwan): binds the "QMI" channel and
 *            registers a WWAN port -> /dev/wwan0qmi0
 *   User space (ModemManager / libqmi / our modem_manager) then just does
 *   write() of a QMUX frame and read() of the reply on that char device.
 *
 *   This module replaces everything below the char device with an in-kernel
 *   "modem" that answers a few QMI requests, so the *char-device contract* is
 *   real kernel code: file_operations, copy_{from,to}_user, a wait queue for
 *   blocking read(), poll(), and an ioctl() ABI with _IOR/_IOW numbers. There
 *   is no PCIe hardware and no MHI rings here -- see ../edu_pci for the real
 *   PCI-driver half (probe, BAR MMIO, MSI) against an emulated PCI device.
 *
 * QMUX framing (same bytes as lib/qmi in the main lab):
 *   0x01 | len(le16, excludes the 0x01) | ctrl flags | service | client id | SDU
 *   CTL SDU:   flags(1) txn(1)  msg(le16) tlv_len(le16) TLVs
 *   other SDU: flags(1) txn(le16) msg(le16) tlv_len(le16) TLVs
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/unaligned.h>

#include "../include/fake_qmi_uapi.h"

#define FQMI_MAX_FRAME 512
#define FQMI_QUEUE_LEN 16

#define QMI_SVC_CTL 0x00
#define QMI_SVC_DMS 0x02
#define QMI_CTL_ALLOCATE_CID 0x0022
#define QMI_DMS_GET_REVISION 0x0023
#define QMI_ERR_INVALID_CLIENT_ID 0x0022
#define QMI_ERR_INVALID_QMI_CMD 0x0047

static const char fqmi_revision[] = "FAKE_MHI_MODEM_REV1";

struct fqmi_frame {
	u16 len;
	u8 data[FQMI_MAX_FRAME];
};

struct fqmi_dev {
	struct mutex lock;           /* protects everything below          */
	wait_queue_head_t readq;     /* readers sleep here until a reply    */
	u32 state;                   /* enum fqmi_state                     */
	struct fqmi_frame q[FQMI_QUEUE_LEN];
	unsigned int head, count;    /* ring of pending modem->host frames  */
	u8 next_cid[256];            /* per-service client-id allocator     */
	struct fqmi_stats stats;
};

static struct fqmi_dev fdev;

/* ---- tiny QMI response builder ------------------------------------------ */

struct fqmi_builder {
	u8 *buf;
	size_t len;
	size_t tlv_len_off;  /* offset of the SDU's tlv_len field */
	size_t tlv_start;
};

static void put_tlv(struct fqmi_builder *b, u8 type, const void *val, u16 vlen)
{
	b->buf[b->len++] = type;
	put_unaligned_le16(vlen, b->buf + b->len);
	b->len += 2;
	memcpy(b->buf + b->len, val, vlen);
	b->len += vlen;
}

static void put_result(struct fqmi_builder *b, u16 err)
{
	u8 r[4];

	put_unaligned_le16(err ? 1 : 0, r);  /* 0 = success, 1 = failure */
	put_unaligned_le16(err, r + 2);
	put_tlv(b, 0x02, r, sizeof(r));
}

static void begin_resp(struct fqmi_builder *b, u8 *out, u8 svc, u8 cid,
		       u16 txn, u16 msg)
{
	b->buf = out;
	out[0] = 0x01;          /* QMUX marker; len patched at the end   */
	out[3] = 0x80;          /* ctrl flags: sender is the service     */
	out[4] = svc;
	out[5] = cid;
	b->len = 6;
	if (svc == QMI_SVC_CTL) {
		out[b->len++] = 0x01;   /* CTL: response */
		out[b->len++] = (u8)txn;
	} else {
		out[b->len++] = 0x02;   /* service: response */
		put_unaligned_le16(txn, out + b->len);
		b->len += 2;
	}
	put_unaligned_le16(msg, out + b->len);
	b->len += 2;
	b->tlv_len_off = b->len;
	b->len += 2;
	b->tlv_start = b->len;
}

static u16 end_resp(struct fqmi_builder *b)
{
	put_unaligned_le16(b->len - b->tlv_start, b->buf + b->tlv_len_off);
	put_unaligned_le16(b->len - 1, b->buf + 1);
	return b->len;
}

/* ---- the "modem firmware" ------------------------------------------------ */

/* Called with fdev.lock held. Returns 0 or -EINVAL for a malformed frame. */
static int fqmi_handle(const u8 *f, size_t n, struct fqmi_frame *resp)
{
	struct fqmi_builder b;
	u8 svc, cid;
	u16 txn, msg, tlv_len;
	size_t hdr;

	if (n < 6 + 6 || f[0] != 0x01)
		return -EINVAL;
	if (get_unaligned_le16(f + 1) != n - 1)
		return -EINVAL;
	if (f[3] & 0x80)                 /* host must not send "from service" */
		return -EINVAL;
	svc = f[4];
	cid = f[5];
	if (svc == QMI_SVC_CTL) {
		txn = f[7];
		hdr = 6 + 6;
	} else {
		if (n < 6 + 7)
			return -EINVAL;
		txn = get_unaligned_le16(f + 7);
		hdr = 6 + 7;
	}
	msg = get_unaligned_le16(f + hdr - 4);
	tlv_len = get_unaligned_le16(f + hdr - 2);
	if (hdr + tlv_len != n)
		return -EINVAL;

	begin_resp(&b, resp->data, svc, cid, txn, msg);

	if (svc == QMI_SVC_CTL && msg == QMI_CTL_ALLOCATE_CID) {
		/* request TLV 0x01 = service to allocate a client for */
		const u8 *t = f + hdr;
		u8 want, pair[2];

		if (tlv_len < 4 || t[0] != 0x01 || get_unaligned_le16(t + 1) < 1)
			return -EINVAL;
		want = t[3];
		pair[0] = want;
		pair[1] = ++fdev.next_cid[want];  /* first client id is 1 */
		put_result(&b, 0);
		put_tlv(&b, 0x01, pair, sizeof(pair));
	} else if (svc == QMI_SVC_DMS && msg == QMI_DMS_GET_REVISION) {
		if (cid == 0 || cid > fdev.next_cid[QMI_SVC_DMS]) {
			put_result(&b, QMI_ERR_INVALID_CLIENT_ID);
		} else {
			put_result(&b, 0);
			put_tlv(&b, 0x01, fqmi_revision, sizeof(fqmi_revision) - 1);
		}
	} else {
		put_result(&b, QMI_ERR_INVALID_QMI_CMD);
	}
	resp->len = end_resp(&b);
	return 0;
}

static void fqmi_flush_locked(void)
{
	fdev.head = 0;
	fdev.count = 0;
	memset(fdev.next_cid, 0, sizeof(fdev.next_cid));
}

/* ---- file_operations ----------------------------------------------------- */

static ssize_t fqmi_write(struct file *file, const char __user *ubuf,
			  size_t n, loff_t *ppos)
{
	u8 *req;
	struct fqmi_frame *slot;
	ssize_t ret;

	if (n == 0 || n > FQMI_MAX_FRAME)
		return -EINVAL;
	req = memdup_user(ubuf, n);        /* copy_from_user into a kmalloc */
	if (IS_ERR(req))
		return PTR_ERR(req);

	mutex_lock(&fdev.lock);
	if (fdev.state != FQMI_STATE_M0) {
		ret = -ENETDOWN;            /* channel not started, like MHI */
		goto out;
	}
	if (fdev.count == FQMI_QUEUE_LEN) {
		ret = -EAGAIN;              /* host is not reading replies   */
		goto out;
	}
	slot = &fdev.q[(fdev.head + fdev.count) % FQMI_QUEUE_LEN];
	ret = fqmi_handle(req, n, slot);
	if (ret) {
		fdev.stats.bad_frames++;
		goto out;
	}
	fdev.count++;
	fdev.stats.tx_frames++;
	ret = n;
	wake_up_interruptible(&fdev.readq);
out:
	mutex_unlock(&fdev.lock);
	kfree(req);
	return ret;
}

static ssize_t fqmi_read(struct file *file, char __user *ubuf,
			 size_t n, loff_t *ppos)
{
	struct fqmi_frame *f;
	ssize_t ret;

	mutex_lock(&fdev.lock);
	while (fdev.count == 0) {
		mutex_unlock(&fdev.lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/* sleep without the lock; woken by write() or reset */
		if (wait_event_interruptible(fdev.readq, READ_ONCE(fdev.count) > 0))
			return -ERESTARTSYS;
		mutex_lock(&fdev.lock);
	}
	f = &fdev.q[fdev.head];
	if (n < f->len) {           /* whole frames only, like a cdc-wdm read */
		ret = -EMSGSIZE;
		goto out;
	}
	if (copy_to_user(ubuf, f->data, f->len)) {
		ret = -EFAULT;
		goto out;
	}
	ret = f->len;
	fdev.head = (fdev.head + 1) % FQMI_QUEUE_LEN;
	fdev.count--;
	fdev.stats.rx_frames++;
out:
	mutex_unlock(&fdev.lock);
	return ret;
}

static __poll_t fqmi_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &fdev.readq, wait);
	mutex_lock(&fdev.lock);
	if (fdev.count)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (fdev.state == FQMI_STATE_M0 && fdev.count < FQMI_QUEUE_LEN)
		mask |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&fdev.lock);
	return mask;
}

static long fqmi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct fqmi_stats st;
	u32 v;

	switch (cmd) {
	case FQMI_IOC_GET_STATE:
		mutex_lock(&fdev.lock);
		v = fdev.state;
		mutex_unlock(&fdev.lock);
		return put_user(v, (u32 __user *)uarg);
	case FQMI_IOC_SET_STATE:
		if (get_user(v, (u32 __user *)uarg))
			return -EFAULT;
		if (v > FQMI_STATE_SYS_ERR)
			return -EINVAL;
		mutex_lock(&fdev.lock);
		fdev.state = v;
		mutex_unlock(&fdev.lock);
		return 0;
	case FQMI_IOC_RESET:
		/* RESET -> READY -> M0, dropping queued replies and client ids,
		 * which is what a host sees after a modem warm reset. */
		mutex_lock(&fdev.lock);
		fdev.state = FQMI_STATE_RESET;
		fqmi_flush_locked();
		fdev.state = FQMI_STATE_READY;
		fdev.state = FQMI_STATE_M0;
		fdev.stats.resets++;
		mutex_unlock(&fdev.lock);
		return 0;
	case FQMI_IOC_GET_STATS:
		mutex_lock(&fdev.lock);
		st = fdev.stats;
		st.state = fdev.state;
		mutex_unlock(&fdev.lock);
		return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
	default:
		return -ENOTTY;   /* the conventional "not my ioctl" answer */
	}
}

static const struct file_operations fqmi_fops = {
	.owner          = THIS_MODULE,
	.read           = fqmi_read,
	.write          = fqmi_write,
	.poll           = fqmi_poll,
	.unlocked_ioctl = fqmi_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice fqmi_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "fake_qmi0",       /* -> /dev/fake_qmi0 via devtmpfs */
	.fops  = &fqmi_fops,
	.mode  = 0666,
};

static int __init fqmi_init(void)
{
	int ret;

	mutex_init(&fdev.lock);
	init_waitqueue_head(&fdev.readq);
	fdev.state = FQMI_STATE_RESET;
	ret = misc_register(&fqmi_misc);
	if (ret)
		return ret;
	/* "power up": a real MHI controller walks RESET -> READY -> M0 */
	fdev.state = FQMI_STATE_READY;
	fdev.state = FQMI_STATE_M0;
	pr_info("fake_mhi_modem: /dev/%s ready, state M0\n", fqmi_misc.name);
	return 0;
}

static void __exit fqmi_exit(void)
{
	misc_deregister(&fqmi_misc);
	pr_info("fake_mhi_modem: unloaded\n");
}

module_init(fqmi_init);
module_exit(fqmi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fake MHI modem: QMI control channel as a char device");

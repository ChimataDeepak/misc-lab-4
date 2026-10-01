// SPDX-License-Identifier: GPL-2.0
/*
 * edu_pci.ko -- a real PCI driver for QEMU's educational PCI device
 * ("-device edu", vendor 0x1234, device 0x11e8; docs/specs/edu.rst in QEMU).
 *
 * This is the same skeleton a PCIe modem driver such as mhi_pci_generic uses:
 *   pci_driver + id_table      -> the PCI core calls probe() on a match
 *   pcim_enable_device()       -> wake the function, managed cleanup
 *   pcim_iomap_regions()       -> claim + ioremap BAR0 (MMIO registers)
 *   pci_alloc_irq_vectors()    -> MSI if the platform supports it, else INTx
 *   request_irq() + completion -> device signals "work done" by interrupt
 * The difference: MHI's BAR holds doorbells and the context base addresses of
 * DMA rings; edu's BAR holds a factorial engine. The driver mechanics are equal.
 *
 * BAR0 register map (edu.rst):
 *   0x00 RO ident      0x010000ed
 *   0x04 RW liveness   reads back ~written value
 *   0x08 RW factorial  write n, read n! when status.computing clears
 *   0x20 RW status     bit0 computing (RO), bit7 raise IRQ after factorial
 *   0x24 RO irq status
 *   0x60 WO irq raise  (OR value into irq status, raise IRQ)
 *   0x64 WO irq ack    (clear bits, lower IRQ)
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/io.h>

#include "../include/edu_uapi.h"

#define EDU_REG_IDENT     0x00
#define EDU_REG_LIVENESS  0x04
#define EDU_REG_FACT      0x08
#define EDU_REG_STATUS    0x20
#define EDU_REG_IRQ_STAT  0x24
#define EDU_REG_IRQ_RAISE 0x60
#define EDU_REG_IRQ_ACK   0x64
#define EDU_STATUS_COMPUTING 0x01
#define EDU_STATUS_IRQFACT   0x80
#define EDU_IRQ_FACT_DONE    0x01

struct edu_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	struct mutex lock;          /* one ioctl operation on the device at a time */
	struct completion done;
	u32 last_irq_status;
	atomic_t irq_count;
	int irq;
	struct miscdevice misc;
};

static struct edu_dev *edu_single;  /* demo: one device -> /dev/edu0 */

static irqreturn_t edu_isr(int irq, void *data)
{
	struct edu_dev *e = data;
	u32 st = ioread32(e->bar0 + EDU_REG_IRQ_STAT);

	if (!st)
		return IRQ_NONE;   /* shared INTx line, not ours */
	iowrite32(st, e->bar0 + EDU_REG_IRQ_ACK);  /* ack = lower the line */
	e->last_irq_status = st;
	atomic_inc(&e->irq_count);
	complete(&e->done);
	return IRQ_HANDLED;
}

static long edu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct edu_dev *e = edu_single;
	void __user *uarg = (void __user *)arg;
	struct edu_info info;
	struct edu_xfer x;
	long ret = 0;

	if (!e)
		return -ENODEV;
	/* Validate the command BEFORE touching the user pointer: an unknown
	 * ioctl must fail with ENOTTY, not EFAULT from a premature copy. */
	if (_IOC_TYPE(cmd) != EDU_IOC_MAGIC || _IOC_NR(cmd) < 1 || _IOC_NR(cmd) > 4)
		return -ENOTTY;
	if (cmd != EDU_IOC_INFO && copy_from_user(&x, uarg, sizeof(x)))
		return -EFAULT;

	mutex_lock(&e->lock);
	switch (cmd) {
	case EDU_IOC_INFO:
		info.ident = ioread32(e->bar0 + EDU_REG_IDENT);
		info.irq_kind = e->pdev->msi_enabled ? 1 : 0;
		info.irq_count = atomic_read(&e->irq_count);
		info.bar0_len = pci_resource_len(e->pdev, 0);
		mutex_unlock(&e->lock);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	case EDU_IOC_LIVENESS:
		iowrite32(x.in, e->bar0 + EDU_REG_LIVENESS);
		x.out = ioread32(e->bar0 + EDU_REG_LIVENESS);
		break;
	case EDU_IOC_FACTORIAL:
		reinit_completion(&e->done);
		iowrite32(EDU_STATUS_IRQFACT, e->bar0 + EDU_REG_STATUS);
		iowrite32(x.in, e->bar0 + EDU_REG_FACT);  /* starts the engine */
		if (!wait_for_completion_timeout(&e->done, msecs_to_jiffies(2000))) {
			ret = -ETIMEDOUT;   /* no interrupt arrived */
			break;
		}
		if (!(e->last_irq_status & EDU_IRQ_FACT_DONE)) {
			ret = -EIO;
			break;
		}
		x.out = ioread32(e->bar0 + EDU_REG_FACT);
		break;
	case EDU_IOC_RAISE_IRQ:
		reinit_completion(&e->done);
		iowrite32(x.in, e->bar0 + EDU_REG_IRQ_RAISE);
		if (!wait_for_completion_timeout(&e->done, msecs_to_jiffies(2000))) {
			ret = -ETIMEDOUT;
			break;
		}
		x.out = e->last_irq_status;
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&e->lock);
	if (!ret && copy_to_user(uarg, &x, sizeof(x)))
		ret = -EFAULT;
	return ret;
}

static const struct file_operations edu_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = edu_ioctl,
	.llseek         = noop_llseek,
};

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_dev *e;
	u32 ident;
	int ret;

	if (edu_single)
		return -EBUSY;
	e = devm_kzalloc(&pdev->dev, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->pdev = pdev;
	mutex_init(&e->lock);
	init_completion(&e->done);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	ret = pcim_iomap_regions(pdev, BIT(0), "edu_pci");
	if (ret)
		return ret;
	e->bar0 = pcim_iomap_table(pdev)[0];
	pci_set_master(pdev);  /* edu can DMA; MSI writes are bus-master writes */

	ident = ioread32(e->bar0 + EDU_REG_IDENT);
	if ((ident & 0xff) != 0xed) {
		dev_err(&pdev->dev, "bad ident 0x%08x\n", ident);
		return -ENODEV;
	}

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_LEGACY);
	if (ret < 0)
		return ret;
	e->irq = pci_irq_vector(pdev, 0);
	ret = request_irq(e->irq, edu_isr, IRQF_SHARED, "edu_pci", e);
	if (ret) {
		pci_free_irq_vectors(pdev);
		return ret;
	}

	e->misc.minor = MISC_DYNAMIC_MINOR;
	e->misc.name = "edu0";
	e->misc.fops = &edu_fops;
	e->misc.mode = 0666;
	ret = misc_register(&e->misc);
	if (ret) {
		free_irq(e->irq, e);
		pci_free_irq_vectors(pdev);
		return ret;
	}
	pci_set_drvdata(pdev, e);
	edu_single = e;
	dev_info(&pdev->dev, "edu ident 0x%08x, BAR0 %pR, irq %d (%s)\n",
		 ident, &pdev->resource[0], e->irq,
		 pdev->msi_enabled ? "MSI" : "INTx");
	return 0;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *e = pci_get_drvdata(pdev);

	misc_deregister(&e->misc);
	edu_single = NULL;
	free_irq(e->irq, e);
	pci_free_irq_vectors(pdev);
	/* BAR mapping and enable are undone by the pcim_* managed helpers */
}

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(0x1234, 0x11e8) },
	{ }
};
MODULE_DEVICE_TABLE(pci, edu_ids);  /* lets udev/modprobe autoload by alias */

static struct pci_driver edu_driver = {
	.name     = "edu_pci",
	.id_table = edu_ids,
	.probe    = edu_probe,
	.remove   = edu_remove,
};
module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PCI driver for QEMU edu device: BAR MMIO + MSI/INTx");

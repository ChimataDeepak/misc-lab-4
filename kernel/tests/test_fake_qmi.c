/* User-space test: talk QMI to fake_mhi_modem.ko through /dev/fake_qmi0. */
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../include/fake_qmi_uapi.h"
#include "check.h"

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* Build a QMUX frame. ctl=1 -> 1-byte txn id, else 2-byte. */
static size_t build(uint8_t *f, uint8_t svc, uint8_t cid, uint16_t txn, uint16_t msg,
		    const uint8_t *tlvs, uint16_t tlen)
{
	size_t n = 6;
	f[0] = 0x01; f[3] = 0x00; f[4] = svc; f[5] = cid;
	f[n++] = 0x00;                              /* request */
	f[n++] = txn & 0xff;
	if (svc != 0) f[n++] = txn >> 8;
	f[n++] = msg & 0xff; f[n++] = msg >> 8;
	f[n++] = tlen & 0xff; f[n++] = tlen >> 8;
	memcpy(f + n, tlvs, tlen); n += tlen;
	f[1] = (n - 1) & 0xff; f[2] = (n - 1) >> 8;
	return n;
}

/* Find TLV `type` in a response; returns pointer to value or NULL. */
static const uint8_t *tlv(const uint8_t *f, size_t n, uint8_t type, uint16_t *vlen)
{
	size_t off = (f[4] == 0) ? 12 : 13;
	while (off + 3 <= n) {
		uint16_t l = le16(f + off + 1);
		if (f[off] == type) { *vlen = l; return f + off + 3; }
		off += 3 + l;
	}
	return NULL;
}

int main(void)
{
	uint8_t req[256], rsp[512];
	uint16_t vl;
	uint32_t st = 99;
	struct fqmi_stats stats;
	ssize_t n;
	const uint8_t *v;

	printf("== test_fake_qmi: /dev/fake_qmi0 ==\n");
	int fd = open("/dev/fake_qmi0", O_RDWR);
	CHECK(fd >= 0, "open /dev/fake_qmi0");
	if (fd < 0) return 1;

	CHECK(ioctl(fd, FQMI_IOC_GET_STATE, &st) == 0 && st == FQMI_STATE_M0,
	      "ioctl GET_STATE (_IOR) -> M0 after module init (got %u)", st);

	struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };
	CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT) && !(p.revents & POLLIN),
	      "poll: writable, nothing to read yet");

	/* CTL Allocate CID for DMS (svc 2) */
	uint8_t want_dms[] = { 0x01, 0x01, 0x00, 0x02 };
	size_t rl = build(req, 0, 0, 7, 0x0022, want_dms, sizeof(want_dms));
	CHECK(write(fd, req, rl) == (ssize_t)rl, "write CTL AllocateCID(DMS) frame (%zu bytes)", rl);
	CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN), "poll: reply readable");
	n = read(fd, rsp, sizeof(rsp));
	CHECK(n > 12 && rsp[0] == 0x01 && le16(rsp + 1) == n - 1 && rsp[3] == 0x80,
	      "read reply: QMUX marker, length %zd, from-service flag", n);
	CHECK(rsp[6] == 0x01 && rsp[7] == 7 && le16(rsp + 8) == 0x0022, "CTL response flag, txn 7 echoed, msg 0x0022");
	v = tlv(rsp, n, 0x02, &vl);
	CHECK(v && vl == 4 && le16(v) == 0, "result TLV = success");
	v = tlv(rsp, n, 0x01, &vl);
	uint8_t cid = v ? v[1] : 0;
	CHECK(v && vl == 2 && v[0] == 2 && cid == 1, "allocated DMS client id %u", cid);

	/* DMS Get Revision on that client */
	rl = build(req, 2, cid, 0x1234, 0x0023, NULL, 0);
	CHECK(write(fd, req, rl) == (ssize_t)rl, "write DMS GetRevision (2-byte txn 0x1234)");
	n = read(fd, rsp, sizeof(rsp));
	CHECK(n > 13 && rsp[6] == 0x02 && le16(rsp + 7) == 0x1234, "DMS response, txn 0x1234 echoed");
	v = tlv(rsp, n, 0x01, &vl);
	CHECK(v && vl == 19 && memcmp(v, "FAKE_MHI_MODEM_REV1", 19) == 0, "revision = %.*s", v ? vl : 0, v ? (const char *)v : "");

	/* invalid client id -> QMI error, not a kernel error */
	rl = build(req, 2, 9, 2, 0x0023, NULL, 0);
	write(fd, req, rl); n = read(fd, rsp, sizeof(rsp));
	v = tlv(rsp, n, 0x02, &vl);
	CHECK(v && le16(v) == 1 && le16(v + 2) == 0x0022, "unallocated client id -> QMI_ERR_INVALID_CLIENT_ID");

	/* unknown message -> QMI_ERR_INVALID_QMI_CMD */
	rl = build(req, 2, cid, 3, 0x7777, NULL, 0);
	write(fd, req, rl); n = read(fd, rsp, sizeof(rsp));
	v = tlv(rsp, n, 0x02, &vl);
	CHECK(v && le16(v + 2) == 0x0047, "unknown msg id -> QMI_ERR_INVALID_QMI_CMD");

	/* malformed frames -> -EINVAL from write() */
	uint8_t junk[] = { 0x02, 0x05, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	errno = 0;
	CHECK(write(fd, junk, sizeof(junk)) < 0 && errno == EINVAL, "bad QMUX marker -> EINVAL");
	rl = build(req, 2, cid, 4, 0x0023, NULL, 0);
	req[1]++;
	errno = 0;
	CHECK(write(fd, req, rl) < 0 && errno == EINVAL, "QMUX length mismatch -> EINVAL");

	/* empty queue + O_NONBLOCK -> EAGAIN */
	int nb = open("/dev/fake_qmi0", O_RDWR | O_NONBLOCK);
	errno = 0;
	CHECK(read(nb, rsp, sizeof(rsp)) < 0 && errno == EAGAIN, "non-blocking read on empty queue -> EAGAIN");
	close(nb);

	/* modem crash: SYS_ERR -> writes refused */
	st = FQMI_STATE_SYS_ERR;
	CHECK(ioctl(fd, FQMI_IOC_SET_STATE, &st) == 0, "ioctl SET_STATE (_IOW) SYS_ERR");
	rl = build(req, 2, cid, 5, 0x0023, NULL, 0);
	errno = 0;
	CHECK(write(fd, req, rl) < 0 && errno == ENETDOWN, "write while SYS_ERR -> ENETDOWN");
	st = 42;
	CHECK(ioctl(fd, FQMI_IOC_SET_STATE, &st) < 0 && errno == EINVAL, "SET_STATE out of range -> EINVAL");

	/* reset recovers to M0 and forgets client ids */
	CHECK(ioctl(fd, FQMI_IOC_RESET) == 0, "ioctl RESET (_IO)");
	CHECK(ioctl(fd, FQMI_IOC_GET_STATE, &st) == 0 && st == FQMI_STATE_M0, "state back to M0");
	rl = build(req, 2, cid, 6, 0x0023, NULL, 0);
	write(fd, req, rl); n = read(fd, rsp, sizeof(rsp));
	v = tlv(rsp, n, 0x02, &vl);
	CHECK(v && le16(v + 2) == 0x0022, "old client id invalid after reset");

	CHECK(ioctl(fd, FQMI_IOC_GET_STATS, &stats) == 0, "ioctl GET_STATS");
	CHECK(stats.tx_frames == 5 && stats.rx_frames == 5 && stats.bad_frames == 2 && stats.resets == 1,
	      "stats tx=%u rx=%u bad=%u resets=%u", stats.tx_frames, stats.rx_frames, stats.bad_frames, stats.resets);
	errno = 0;
	CHECK(ioctl(fd, _IO('Q', 99)) < 0 && errno == ENOTTY, "unknown ioctl -> ENOTTY");
	close(fd);

	printf("test_fake_qmi: %s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
	return g_fail ? 1 : 0;
}

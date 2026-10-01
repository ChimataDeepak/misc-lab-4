/* User-space test for edu_pci.ko: ioctl -> driver -> BAR0 MMIO -> MSI back. */
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../include/edu_uapi.h"
#include "check.h"

int main(void)
{
	struct edu_info info, after;
	struct edu_xfer x;

	printf("== test_edu: /dev/edu0 (PCI 1234:11e8) ==\n");
	int fd = open("/dev/edu0", O_RDWR);
	CHECK(fd >= 0, "open /dev/edu0 (exists only if probe() bound the device)");
	if (fd < 0) return 1;
	CHECK(ioctl(fd, EDU_IOC_INFO, &info) == 0, "ioctl INFO");
	CHECK((info.ident & 0xff) == 0xed, "BAR0 ident register = 0x%08x", info.ident);
	CHECK(info.bar0_len == 1 << 20, "BAR0 size = %u bytes (1 MiB)", info.bar0_len);
	printf("  info irq kind: %s\n", info.irq_kind ? "MSI" : "legacy INTx");

	x.in = 0x12345678;
	CHECK(ioctl(fd, EDU_IOC_LIVENESS, &x) == 0 && x.out == ~0x12345678u,
	      "liveness MMIO write/read: ~0x12345678 = 0x%08x", x.out);

	x.in = 10; x.out = 0;
	CHECK(ioctl(fd, EDU_IOC_FACTORIAL, &x) == 0 && x.out == 3628800,
	      "factorial(10) via device + completion IRQ = %u", x.out);
	x.in = 12; x.out = 0;
	CHECK(ioctl(fd, EDU_IOC_FACTORIAL, &x) == 0 && x.out == 479001600, "factorial(12) = %u", x.out);

	x.in = 0x100; x.out = 0;
	CHECK(ioctl(fd, EDU_IOC_RAISE_IRQ, &x) == 0 && x.out == 0x100, "software-raised IRQ, status 0x%x seen in ISR", x.out);

	CHECK(ioctl(fd, EDU_IOC_INFO, &after) == 0 && after.irq_count == info.irq_count + 3,
	      "ISR ran exactly 3 more times (%u -> %u)", info.irq_count, after.irq_count);
	errno = 0;
	CHECK(ioctl(fd, _IO('E', 99)) < 0 && errno == ENOTTY, "unknown ioctl -> ENOTTY");
	close(fd);
	printf("test_edu: %s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
	return g_fail ? 1 : 0;
}

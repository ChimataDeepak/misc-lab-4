/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* User-space ABI for /dev/edu0 (edu_pci.ko, driver for QEMU's "-device edu"). */
#ifndef EDU_UAPI_H
#define EDU_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct edu_info {
	__u32 ident;      /* BAR0+0x00: 0xRRrr00ed (major/minor version + 0xed) */
	__u32 irq_kind;   /* 1 = MSI, 0 = legacy INTx                           */
	__u32 irq_count;  /* interrupts handled so far                          */
	__u32 bar0_len;   /* size of BAR0 in bytes                               */
};

struct edu_xfer {
	__u32 in;
	__u32 out;
};

#define EDU_IOC_MAGIC     'E'
#define EDU_IOC_INFO      _IOR(EDU_IOC_MAGIC, 1, struct edu_info)
#define EDU_IOC_LIVENESS  _IOWR(EDU_IOC_MAGIC, 2, struct edu_xfer) /* out = ~in        */
#define EDU_IOC_FACTORIAL _IOWR(EDU_IOC_MAGIC, 3, struct edu_xfer) /* out = in!, by IRQ */
#define EDU_IOC_RAISE_IRQ _IOWR(EDU_IOC_MAGIC, 4, struct edu_xfer) /* out = status seen */

#endif

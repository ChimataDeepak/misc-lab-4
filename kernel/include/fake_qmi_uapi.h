/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * User-space ABI for /dev/fake_qmi0 (fake_mhi_modem.ko).
 *
 * Shared by the kernel module and the user-space tests, exactly like a real
 * driver's include/uapi/linux/<driver>.h. Only fixed-width __u32 types are used
 * so the struct layout is identical for 32- and 64-bit callers.
 *
 * The states mirror the MHI state machine names (drivers/bus/mhi/common.h):
 * RESET -> READY -> M0 (mission mode, channels usable). SYS_ERR is the state a
 * real modem enters when its firmware crashes.
 */
#ifndef FAKE_QMI_UAPI_H
#define FAKE_QMI_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum fqmi_state {
	FQMI_STATE_RESET   = 0,
	FQMI_STATE_READY   = 1,
	FQMI_STATE_M0      = 2,
	FQMI_STATE_SYS_ERR = 3,
};

struct fqmi_stats {
	__u32 state;
	__u32 tx_frames;   /* frames host -> modem (write())            */
	__u32 rx_frames;   /* frames modem -> host (read())             */
	__u32 bad_frames;  /* write()s rejected as malformed QMUX       */
	__u32 resets;      /* FQMI_IOC_RESET count                      */
};

#define FQMI_IOC_MAGIC     'Q'
#define FQMI_IOC_GET_STATE _IOR(FQMI_IOC_MAGIC, 1, __u32)
#define FQMI_IOC_RESET     _IO(FQMI_IOC_MAGIC, 2)
#define FQMI_IOC_SET_STATE _IOW(FQMI_IOC_MAGIC, 3, __u32)
#define FQMI_IOC_GET_STATS _IOR(FQMI_IOC_MAGIC, 4, struct fqmi_stats)

#endif

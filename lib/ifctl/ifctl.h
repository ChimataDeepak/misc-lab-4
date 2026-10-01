/*
 * ifctl.h -- classic ioctl() interface control (what `ifconfig` does).
 * SIOCGIFFLAGS/SIOCSIFFLAGS read/write IFF_UP etc., SIOCGIFMTU/SIOCSIFMTU the MTU.
 * The ioctl is issued on any AF_INET datagram socket; `struct ifreq` names the
 * interface. Setting needs CAP_NET_ADMIN. All functions return 0 or -errno.
 */
#ifndef MLAB_IFCTL_H
#define MLAB_IFCTL_H

#ifdef __cplusplus
extern "C" {
#endif

int ifctl_get_flags(const char *ifname, unsigned *flags);
int ifctl_set_up(const char *ifname, int up);
int ifctl_get_mtu(const char *ifname, int *mtu);
int ifctl_set_mtu(const char *ifname, int mtu);

#ifdef __cplusplus
}
#endif
#endif

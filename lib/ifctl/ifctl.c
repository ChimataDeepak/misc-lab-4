#include "ifctl.h"

#include <errno.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Every call opens a throwaway socket: ioctls need *a* socket fd, the type
 * barely matters. Long-running daemons would keep one around. */
static int with_ifreq(const char *ifname, unsigned long req, struct ifreq *ifr)
{
    if (!ifname || strlen(ifname) >= IFNAMSIZ)
        return -EINVAL;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    strncpy(ifr->ifr_name, ifname, IFNAMSIZ - 1);
    ifr->ifr_name[IFNAMSIZ - 1] = '\0';
    int rc = ioctl(fd, req, ifr) < 0 ? -errno : 0;
    close(fd);
    return rc;
}

int ifctl_get_flags(const char *ifname, unsigned *flags)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    int rc = with_ifreq(ifname, SIOCGIFFLAGS, &ifr);
    if (rc == 0)
        *flags = (unsigned short)ifr.ifr_flags; /* ifr_flags is a short */
    return rc;
}

int ifctl_set_up(const char *ifname, int up)
{
    /* Read-modify-write: SIOCSIFFLAGS replaces ALL flags, so we must start
     * from the current set or we'd clobber e.g. IFF_MULTICAST/IFF_NOARP. */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    int rc = with_ifreq(ifname, SIOCGIFFLAGS, &ifr);
    if (rc)
        return rc;
    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= (short)~IFF_UP;
    return with_ifreq(ifname, SIOCSIFFLAGS, &ifr);
}

int ifctl_get_mtu(const char *ifname, int *mtu)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    int rc = with_ifreq(ifname, SIOCGIFMTU, &ifr);
    if (rc == 0)
        *mtu = ifr.ifr_mtu;
    return rc;
}

int ifctl_set_mtu(const char *ifname, int mtu)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_mtu = mtu;
    return with_ifreq(ifname, SIOCSIFMTU, &ifr);
}

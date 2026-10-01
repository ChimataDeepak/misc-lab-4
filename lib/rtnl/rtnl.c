#include "rtnl.h"

#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int rtnl_open(unsigned groups, int nonblock)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), NETLINK_ROUTE);
    if (fd < 0)
        return -errno;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = groups;   /* legacy bitmask multicast subscription, e.g. RTMGRP_LINK */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

int rtnl_request_link_dump(int fd, uint32_t seq)
{
    struct {
        struct nlmsghdr nh;
        struct ifinfomsg ifi;
    } req;
    memset(&req, 0, sizeof req);
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nh.nlmsg_type = RTM_GETLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;  /* "give me all of them" */
    req.nh.nlmsg_seq = seq;
    req.ifi.ifi_family = AF_UNSPEC;

    struct sockaddr_nl kernel;
    memset(&kernel, 0, sizeof kernel);
    kernel.nl_family = AF_NETLINK;  /* nl_pid 0 == the kernel */
    if (sendto(fd, &req, req.nh.nlmsg_len, 0, (struct sockaddr *)&kernel, sizeof kernel) < 0)
        return -errno;
    return 0;
}

static void parse_link(const struct nlmsghdr *nh, rtnl_link_cb cb, void *user)
{
    const struct ifinfomsg *ifi = NLMSG_DATA(nh);
    rtnl_link l;
    memset(&l, 0, sizeof l);
    l.is_new = nh->nlmsg_type == RTM_NEWLINK;
    l.ifindex = ifi->ifi_index;
    l.flags = ifi->ifi_flags;

    /* Attributes (struct rtattr TLVs -- same idea as QMI TLVs!) follow the ifinfomsg. */
    int alen = (int)IFLA_PAYLOAD(nh);
    for (const struct rtattr *rta = IFLA_RTA(ifi); RTA_OK(rta, alen); rta = RTA_NEXT(rta, alen)) {
        if (rta->rta_type == IFLA_IFNAME) {
            size_t n = RTA_PAYLOAD(rta);
            if (n >= sizeof l.name)
                n = sizeof l.name - 1;
            memcpy(l.name, RTA_DATA(rta), n);
            l.name[n] = '\0';  /* kernel includes NUL, but never trust it */
        } else if (rta->rta_type == IFLA_MTU && RTA_PAYLOAD(rta) >= sizeof(uint32_t)) {
            uint32_t mtu;
            memcpy(&mtu, RTA_DATA(rta), sizeof mtu);
            l.mtu = mtu;
        }
    }
    cb(&l, user);
}

int rtnl_parse(const void *buf, size_t len, rtnl_link_cb cb, void *user)
{
    int done = 0;
    int remaining = (int)len;
    for (const struct nlmsghdr *nh = buf; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
        switch (nh->nlmsg_type) {
        case NLMSG_DONE:
            done = 1;
            break;
        case NLMSG_ERROR: {
            const struct nlmsgerr *e = NLMSG_DATA(nh);
            if (nh->nlmsg_len < NLMSG_LENGTH(sizeof *e))
                return -EBADMSG;
            if (e->error)
                return e->error;  /* already a negative errno */
            break;                /* error 0 == ACK */
        }
        case RTM_NEWLINK:
        case RTM_DELLINK:
            if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg)))
                return -EBADMSG;
            parse_link(nh, cb, user);
            break;
        default:
            break;
        }
    }
    if (remaining != 0)
        return -EBADMSG; /* trailing garbage / truncated message */
    return done;
}

int rtnl_recv(int fd, rtnl_link_cb cb, void *user)
{
    /* 32K is plenty for links; real code should handle MSG_TRUNC / ENOBUFS
     * (ENOBUFS = we were too slow and the kernel dropped notifications). */
    static __thread unsigned char buf[32768] __attribute__((aligned(4)));
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < 0)
        return -errno;
    return rtnl_parse(buf, (size_t)n, cb, user);
}

int rtnl_dump_links(rtnl_link_cb cb, void *user)
{
    int fd = rtnl_open(0, 0);
    if (fd < 0)
        return fd;
    int rc = rtnl_request_link_dump(fd, 1);
    while (rc == 0)
        rc = rtnl_recv(fd, cb, user);  /* multi-part: loop until NLMSG_DONE */
    close(fd);
    return rc < 0 ? rc : 0;
}

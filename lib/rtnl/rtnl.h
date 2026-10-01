/*
 * rtnl.h -- tiny RTNETLINK (NETLINK_ROUTE) client for network LINKS.
 *
 * Netlink is a socket family (AF_NETLINK) the kernel uses to talk to user
 * space. NETLINK_ROUTE ("rtnetlink") carries routing-table objects: links
 * (interfaces), addresses, routes, neighbours. Two ways we use it:
 *   1. request/response: send RTM_GETLINK with NLM_F_DUMP, kernel answers with
 *      a multi-part stream of RTM_NEWLINK messages terminated by NLMSG_DONE.
 *   2. multicast subscription: bind() with nl_groups = RTMGRP_LINK and the
 *      kernel pushes RTM_NEWLINK / RTM_DELLINK whenever any link changes.
 * This is how ModemManager / NetworkManager / Android netd notice "wwan0 went up".
 */
#ifndef MLAB_RTNL_H
#define MLAB_RTNL_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int is_new;             /* 1 = RTM_NEWLINK (added/changed), 0 = RTM_DELLINK */
    int ifindex;
    unsigned flags;         /* IFF_UP, IFF_RUNNING, ... */
    unsigned mtu;           /* 0 if IFLA_MTU absent */
    char name[IF_NAMESIZE]; /* "" if IFLA_IFNAME absent */
} rtnl_link;

typedef void (*rtnl_link_cb)(const rtnl_link *link, void *user);

/* Opens a NETLINK_ROUTE socket. groups = 0 for request/response only,
 * RTMGRP_LINK to also receive link change notifications. Returns fd or -errno. */
int rtnl_open(unsigned groups, int nonblock);

/* Sends RTM_GETLINK|NLM_F_DUMP. Returns 0 or -errno. */
int rtnl_request_link_dump(int fd, uint32_t seq);

/* Parses a buffer holding one or more netlink messages (pure function, no IO).
 * Calls cb for each RTM_NEWLINK/RTM_DELLINK.
 * Returns 1 if NLMSG_DONE was seen, 0 if not, negative on NLMSG_ERROR/malformed. */
int rtnl_parse(const void *buf, size_t len, rtnl_link_cb cb, void *user);

/* One recv() + rtnl_parse(). Returns like rtnl_parse, or -EAGAIN. */
int rtnl_recv(int fd, rtnl_link_cb cb, void *user);

/* Blocking convenience: open, dump all links, close. Returns 0 or -errno. */
int rtnl_dump_links(rtnl_link_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif

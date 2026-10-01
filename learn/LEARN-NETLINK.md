# LEARN: RTNETLINK (NETLINK_ROUTE)

## 1. What

Netlink is a socket family (`AF_NETLINK`) for messages between user space and the kernel. `NETLINK_ROUTE` ("rtnetlink") carries networking objects: links (interfaces), addresses, routes. You can **ask** for a snapshot (a dump), and you can **subscribe** to change events (multicast groups).

## 2. Why it exists

`ip link`, NetworkManager, ModemManager and Android's netd all learn about interface changes this way, with no polling. When a modem's data interface (`wwan0`, `rmnet_data0`, `mhi_hwip0`) goes up or down, the connection manager hears about it over rtnetlink.

## 3. How it works

### Message layout

```
recv() buffer = one or more netlink messages:
+------------------ nlmsghdr (16 B) ------------------+
| u32 nlmsg_len | u16 type (RTM_NEWLINK/...) | u16 flags | u32 seq | u32 pid |
+-----------------------------------------------------+
| struct ifinfomsg: family, type, ifi_index, ifi_flags (IFF_UP...), change |
+-----------------------------------------------------+
| rtattr TLVs:  u16 rta_len | u16 rta_type (IFLA_IFNAME, IFLA_MTU) | value | pad to 4 |
+-----------------------------------------------------+
| next nlmsghdr ... | NLMSG_DONE (end of a dump) or NLMSG_ERROR (struct nlmsgerr) |
```

It is the same TLV idea as QMI, with 4-byte alignment.

### Snapshot plus stream without a race

```
1. fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE); bind(nl_groups = RTMGRP_LINK)   // subscribe
2. dump: send RTM_GETLINK with NLM_F_REQUEST|NLM_F_DUMP on a second socket; recv until NLMSG_DONE
3. add fd to epoll; each RTM_NEWLINK/RTM_DELLINK -> diff against last_flags_ -> callback
```

If you dump first and subscribe second, a change in between is lost. `daemon/link_monitor.cpp:13-19` subscribes **first**.

### ENOBUFS

If you read too slowly, the kernel drops multicast messages and the next `recv` returns `ENOBUFS`. Your view may now be stale, so re-dump and diff (`daemon/link_monitor.cpp:42-49`).

## 4. Where in this repo

| item | location |
|---|---|
| open and subscribe | `lib/rtnl/rtnl.c:10-25` (`nl_groups` at `:18`) |
| dump request | `lib/rtnl/rtnl.c:27-46` (`NLM_F_DUMP` at `:36`) |
| per-link attribute parsing | `lib/rtnl/rtnl.c:48-73` (`RTA_OK` loop `:59`) |
| pure parser (unit-testable) | `lib/rtnl/rtnl.c:75-105` (`NLMSG_OK` `:79`, `-EBADMSG` `:87`, `:95`) |
| multi-part dump loop | `lib/rtnl/rtnl.c:118-128` |
| LinkMonitor | `daemon/link_monitor.cpp` |
| daemon log lines | `daemon/modem_manager.cpp:68-82` |
| bionic portability hit (`NLMSG_OK` sign-compare) | `android/build-android.sh:37` |

## 5. How it is verified

```bash
./run-tests.sh
```

- `test_rtnl_parse`: hand-built buffers covering multi-part, DELLINK, error and truncation (`tests/test_rtnl_parse.cpp:48,63,72`). No kernel needed.
- `test_netlink_ioctl`: a real kernel, with a dummy link created through `ip link add ... type dummy` (needs `--cap-add NET_ADMIN`).
- `e2e_daemon`: expects `netlink: link wwan0 DOWN -> UP mtu=1430` and `UP -> DOWN`.

By hand:

```bash
ip monitor link &          # same events LinkMonitor sees
ip link set wwan0 up
```

## 6. Real vs simulated

Real netlink against a real kernel (the Rancher VM kernel, through the container). The interface is a `dummy` link, not a modem netdev.

## 7. Interview Q&A

1. **Why netlink and not polling `/sys/class/net`?** Netlink is event-driven, atomic per message, and carries more structure.
2. **How does a dump end?** With an `NLMSG_DONE` message. A dump can span several `recv()` calls.
3. **What does ENOBUFS mean, and how do you recover?** The kernel dropped events because your socket buffer filled up. Re-dump the state and diff it against what you had.
4. **Subscribe before dump, why?** To close the window where a change happens between the snapshot and the subscription.
5. **`RTMGRP_*` vs `NETLINK_ADD_MEMBERSHIP`?** The legacy bitmask in `bind` only covers groups 1 to 32. The setsockopt form handles any group.
6. **Why validate `nlmsg_len`?** Truncated or malformed messages must not cause out-of-bounds reads (`-EBADMSG` paths).
7. **Generic netlink?** A multiplexed family (nl80211, devlink) with families resolved at runtime. The same header idea applies.

## 8. Try it yourself

1. Subscribe to `RTMGRP_IPV4_IFADDR` and log address changes (`RTM_NEWADDR`) on `wwan0`.
2. Write `rtnl_set_link_up` using `RTM_NEWLINK` with `ifi_change = IFF_UP` (the netlink way, instead of ioctl), and compare with `strace`.
3. Shrink the socket receive buffer (`SO_RCVBUF`), flap the link 1000 times, and confirm the ENOBUFS re-dump path runs.

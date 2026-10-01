# misc-lab-4: Modem Control Lab

A small, **working and tested** lab for learning the user-space side of cellular
modem control on Linux: **C and C++17, CMake, GoogleTest, the QMI protocol, Unix-socket IPC
with epoll, RTNETLINK, ioctl, and debugging with sanitizers, gdb and strace**.

The modem is **simulated**: a process that speaks real QMI wire format. No real modem
hardware is involved (see "Not covered & why" below).

```
 +----------------------+   QMUX/QMI frames over a    +----------------------+
 | modem_manager (C++17)| <-- Unix stream socket -->  | modem_sim (C)        |
 |  EventLoop (epoll)   |   (stands in for            |  epoll + timerfd +   |
 |  QmiClient: txn ids, |    /dev/cdc-wdm0 or          |  signalfd; answers   |
 |   timeouts, indic.   |    /dev/wwan0qmi0)           |  CTL/DMS/NAS, pushes |
 |  LinkMonitor (rtnl)  |                              |  NAS indications     |
 +----------+-----------+                              +----------------------+
            | ioctl SIOCSIFFLAGS / SIOCSIFMTU
            v
      kernel netdev "wwan0" (a dummy interface) --RTM_NEWLINK--> LinkMonitor
```

## Run everything

```bash
./run-tests.sh        # needs Docker (Rancher Desktop: DOCKER_HOST=unix://$HOME/.rd/docker.sock)
```

It builds the `debian:bookworm` image and runs the full ctest suite **twice**: once as a
normal `-Wall -Wextra -Werror` build and once with **ASan + UBSan**. The container gets
`--cap-add NET_ADMIN` (create dummy links, ioctl, netlink) and `--cap-add SYS_PTRACE`
(strace, gdb, LeakSanitizer).

| ctest name | kind | what it proves |
|---|---|---|
| `test_qmi` | unit (gtest) | exact QMUX/QMI bytes, 1-byte CTL vs 2-byte txn id, malformed-input rejection |
| `test_sim_core` | unit | simulator handlers: CID allocation, invalid client id, revision, IDs, signal, indication |
| `test_frame_buffer` | unit | byte-by-byte delivery, 2.5 frames per read, resync after garbage |
| `test_qmi_client_socketpair` | unit (socketpair) | split responses, out-of-order replies matched by txn id, timeout and late reply, QMI error, indications, peer close, txn-id exhaustion |
| `test_rtnl_parse` | unit | netlink message and attribute parsing on hand-built buffers |
| `test_e2e_sim` | integration | C++ client against the real `modem_sim` process, including a simulated modem crash |
| `test_netlink_ioctl` | integration (NET_ADMIN) | ioctl up/down/MTU on a dummy link, and RTNETLINK reporting each change |
| `e2e_daemon` | system | sim + daemon + `wwan0`: full bring-up, indications, ioctl UP, netlink DOWN->UP->DOWN |
| `strace_ioctl` | tooling | `strace` shows our `SIOCGIFFLAGS`/`SIOCSIFFLAGS` calls |
| `test_libqmi_interop` | interop | **upstream libqmi 1.32.2** parses our bytes, and we parse libqmi's |

## Try it by hand

```bash
docker run --rm -it --cap-add NET_ADMIN --cap-add SYS_PTRACE misc-lab-4 bash
cmake -S . -B build && cmake --build build -j
ip link add wwan0 type dummy
./build/modem_sim --socket /tmp/m.sock --ind-ms 500 &
./build/modem_manager --socket /tmp/m.sock --iface wwan0 --bring-up --run-ms 3000
```

## Layout

| path | language | role |
|---|---|---|
| `lib/qmi/` | C | QMUX + QMI SDU framing and TLV codec (zero-copy parse) |
| `lib/rtnl/` | C | RTNETLINK: link dump and RTMGRP_LINK subscription, message parsing |
| `lib/ifctl/` | C | ioctl: SIOCG/SIOCSIFFLAGS, SIOCG/SIOCSIFMTU |
| `sim/` | C | simulated modem: `sim_core` (pure handlers) and `modem_sim` (epoll server) |
| `daemon/` | C++17 | `EventLoop`, `FrameBuffer`, `QmiClient`, `LinkMonitor`, `modem_manager` main |
| `tools/ifctl_cli.c` | C | tiny ifconfig-like tool, handy for strace |
| `tests/` | C++/C/sh | everything listed in the table above |

## Debugging

**Sanitizers (automated).** `-DMLAB_SANITIZE=ON` adds `-fsanitize=address,undefined`
with `-fno-sanitize-recover=all`, so any heap overflow, use-after-free, leak, signed overflow
or misaligned access fails the test instead of printing a warning. `run-tests.sh` always runs
this build. To see it work, break something on purpose. For example, in `qmi_find_tlv`
change `p + 3 <= m->tlv_len` to `p <= m->tlv_len` and rerun.

**gdb** (inside the container; the build uses `-g`):
```bash
gdb --args ./build/test_qmi_client_socketpair --gtest_filter='*OutOfOrder*'
(gdb) break mlab::QmiClient::handle_frame
(gdb) run
(gdb) p/x r.msg.txn_id          # which response arrived first?
(gdb) p pending_.size()
(gdb) bt
# attach to a running daemon:
gdb -p $(pidof modem_manager)   # needs SYS_PTRACE
```
Use `catch syscall epoll_wait` to stop each time the loop goes to sleep.

**strace** shows the kernel boundary, which is often the fastest way to see what a daemon is doing:
```bash
strace -f -e trace=ioctl ./build/ifctl_cli wwan0 up        # SIOCGIFFLAGS then SIOCSIFFLAGS
strace -f -e trace=epoll_wait,sendto,recvfrom,read,write -s 64 -x \
       ./build/modem_manager --socket /tmp/m.sock --run-ms 500   # watch QMI bytes go by (\x01...)
strace -e trace=socket,bind,sendto,recvmsg ./build/test_rtnl_parse  # netlink setup
```
`ip monitor link` in another shell shows the same RTNETLINK events that `LinkMonitor` gets.

## Not covered & why

These are real parts of the job description that this lab **does not** implement.
They are not faked.

* **MHI / PCIe modem drivers.** MHI (Modem Host Interface) is the Qualcomm transport
  used by PCIe M.2 modems. The host maps the device's BARs, sets up ring buffers (transfer
  and event rings in host memory) and channels, and the modem DMAs into them. Doing this
  needs real PCIe modem hardware and loading kernel modules (`mhi`, `mhi_pci_generic`,
  `mhi_wwan_ctrl`, `mhi_net`). The Rancher Desktop VM has neither, and a container cannot
  load modules into the shared kernel.
  *How it relates:* on such a modem, the `mhi_wwan_ctrl` driver exposes the **QMI channel**
  as `/dev/wwan0qmi0` through the kernel WWAN subsystem, and `mhi_net` exposes the data path
  as a netdev such as `mhi_hwip0` / `wwan0`. The **same QMUX bytes** this lab encodes are
  written to that char device. So `lib/qmi` and `QmiClient` would sit on top of an fd from
  `open("/dev/wwan0qmi0")` instead of a Unix socket. The framing and transaction logic
  stay the same. `LinkMonitor` and `ifctl` would act on the real netdev. (Newer Qualcomm
  platforms can also carry QMI over **QRTR** sockets (`AF_QIPCRTR`) instead of QMUX.)
* **Android framework (RIL / telephony).** That needs an AOSP tree and device or emulator
  builds, which are far beyond this lab.
  *How it relates:* Android's Java `TelephonyManager`/`ServiceState` talk through the
  **IRadio HAL** (AIDL, formerly HIDL) to a vendor **RIL daemon** (`rild` plus the vendor
  RIL). The vendor RIL plays the same role as this lab's `modem_manager`: it speaks QMI to
  the modem, maps indications such as signal strength to `unsol` responses
  (`currentSignalStrength`), and works with `netd`, which configures the data interface
  through netlink and ioctl as `ifctl`/`rtnl` do here.
* **Real radio behaviour** (registration, data calls through WDS, SIM/UIM). The simulator
  implements only CTL Allocate CID, DMS Get Revision, DMS Get IDs, NAS Get Signal Strength
  and a NAS Event Report indication.

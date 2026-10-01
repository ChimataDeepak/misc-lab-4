# learn/: one guide per skill in the Modem Control Lab

Every guide uses the same layout:

1. **What**: the idea in plain language
2. **Why it exists**
3. **How it works**: ASCII diagrams, byte layouts, call flows
4. **Where in this repo**: `file:line` pointers, checked against commit `1837752`
5. **How it is verified**: the exact command
6. **Real vs simulated vs blocked** in this lab
7. **Interview Q&A**: 6 to 8 questions
8. **Try it yourself**: exercises

## Suggested learning order

Each step builds on the ones before it.

| # | guide | builds on | rough time |
|---|---|---|---|
| 1 | [LEARN-C.md](LEARN-C.md) | nothing | 2-3 h |
| 2 | [LEARN-C++.md](LEARN-C++.md) | C | 2-3 h |
| 3 | [LEARN-CMake-and-GoogleTest.md](LEARN-CMake-and-GoogleTest.md) | C, C++ | 1-2 h |
| 4 | [LEARN-Sockets-and-IPC.md](LEARN-Sockets-and-IPC.md) | C | 2 h |
| 5 | [LEARN-epoll-event-loop.md](LEARN-epoll-event-loop.md) | Sockets | 2-3 h |
| 6 | [LEARN-QMI-protocol.md](LEARN-QMI-protocol.md) | C, Sockets | 3 h |
| 7 | [LEARN-libqmi.md](LEARN-libqmi.md) | QMI | 1 h |
| 8 | [LEARN-NETLINK.md](LEARN-NETLINK.md) | Sockets, epoll | 2 h |
| 9 | [LEARN-IOCTL.md](LEARN-IOCTL.md) | NETLINK (optional) | 1-2 h |
| 10 | [LEARN-Debugging.md](LEARN-Debugging.md) | all of 1-9 | 2 h |
| 11 | [LEARN-Linux-kernel-modules-and-char-devices.md](LEARN-Linux-kernel-modules-and-char-devices.md) | IOCTL, epoll | 3-4 h |
| 12 | [LEARN-PCIe-drivers.md](LEARN-PCIe-drivers.md) | kernel modules | 3 h |
| 13 | [LEARN-MHI.md](LEARN-MHI.md) | PCIe, QMI | 2 h (concepts) |
| 14 | [LEARN-Android-NDK-JNI-and-RIL.md](LEARN-Android-NDK-JNI-and-RIL.md) | C++, QMI, epoll | 3 h |

## The whole lab on one page

```
   Java app / telephony (NOT run here)              android/  (compiled; static parts run)
        | JNI  libmodemjni.so                       AIDL IModemControl (compiled only)
        v
   ModemHal (blocking facade)  ----pumps---->  EventLoop (epoll, timers)       daemon/
        |                                         |      |
        v                                         v      v
   QmiClient: txn ids, timeouts, indications   LinkMonitor (RTNETLINK)  ifctl (ioctl)  lib/
        | QMUX/QMI bytes (lib/qmi)                ^                        |
        v                                         | RTM_NEWLINK            v SIOCSIFFLAGS/MTU
   AF_UNIX stream socket  <---------------->  kernel netdev "wwan0" (a dummy link)
        |
        v
   modem_sim (C, epoll + timerfd + signalfd)                                sim/

   Optional kernel stage (kernel/, inside a QEMU-booted Debian 6.1 kernel):
     /dev/fake_qmi0 <- fake_mhi_modem.ko (char device that speaks the same QMUX bytes)
     /dev/edu0      <- edu_pci.ko (real PCI driver: BAR0 MMIO + MSI, on QEMU's "edu" device)
```

## Verification status (checked while writing these notes)

| command | status |
|---|---|
| `./run-tests.sh` | **rerun on 2026-10-01: PASSED.** 10/10 normal build, 10/10 ASan+UBSan, both optional stages printed SKIPPED |
| `MLAB_KERNEL=1 ./run-tests.sh` | passed at commit `1837752` (26/26 + 10/10 checks inside QEMU). **Not rerun** while writing these notes (it takes many minutes under TCG emulation) |
| `MLAB_ANDROID=1 ./run-tests.sh` | passed at commit `1837752`. **Not rerun** while writing these notes |
| `HOST_INSMOD=1 ./kernel/run-kernel-tests.sh` | **broken as committed.** It calls `kernel/host/host-insmod-attempt.sh`, and that file is not in the repo (see LEARN-Linux-kernel-modules-and-char-devices.md) |

## Two discrepancies found while writing these notes

These are not fixed yet. They are recorded here so nobody is misled.

1. `kernel/run-kernel-tests.sh:15` runs `/lab/host/host-insmod-attempt.sh`, but there is no `kernel/host/` directory. The `HOST_INSMOD=1` path would fail with "file not found". It would not fail with the vermagic refusal the README describes.
2. `kernel/fake_mhi_modem/fake_mhi_modem.c:46` defines `QMI_ERR_INVALID_CLIENT_ID 0x0022`. Upstream libqmi (`qmi-errors.h` in the lab image) says `QMI_PROTOCOL_ERROR_INVALID_CLIENT_ID = 7`, and 34 (0x22) is `AUTHENTICATION_FAILED`. The user-space simulator uses the correct value, 0x0007 (`lib/qmi/qmi.h:63`). The kernel test (`kernel/tests/test_fake_qmi.c:88`) asserts the wrong value, so it passes anyway. That is a good lesson in why the libqmi interop test exists.

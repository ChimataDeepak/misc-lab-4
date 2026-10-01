# Resume bullets (honest; personal learning project)

- Built a C/C++17 modem-control lab on Linux: a QMI (QMUX + TLV) codec in C, and an epoll-based
  C++ daemon talking to a **simulated modem** over Unix-domain-socket IPC. The daemon handles
  partial-read framing, request/response matching by transaction id, timeouts and asynchronous
  indications. The codec was cross-validated against upstream **libqmi** in both directions.
- Added RTNETLINK link monitoring (dump plus RTMGRP_LINK subscription) and ioctl-based interface
  control (SIOCSIFFLAGS/SIOCSIFMTU). Integration tests show that netlink observes the
  ioctl-driven up/down and MTU changes on a dummy `wwan0`.
- Set up CMake + GoogleTest/ctest (10 suites) in Docker with `-Werror` and an ASan/UBSan
  configuration, plus strace and gdb workflows. MHI/PCIe and Android RIL are documented as
  out of scope; no real hardware was used.

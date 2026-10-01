# LEARNING.md — Modem Control Lab, concept by concept

Every section follows the same pattern: **What** it is, **Why** it matters (especially on a
modem/Android team), **How** this code does it (with `file:line` pointers), and **Verified by**
(the test that proves it). Read them in order. Each one builds on the one before.

---

## 1. C for low-level libraries, C++17 for the daemon

**What.** `lib/` and `sim/` are plain C11. `daemon/` is C++17. They link together because the C
headers wrap their declarations in `extern "C"` (`lib/qmi/qmi.h`, near the top and bottom).

**Why.** Modem stacks, HALs and kernel-adjacent libraries (libqmi, libmbim, vendor RILs) are
mostly C: it has a stable ABI, is easy to call from any language, and makes memory layout
explicit. Daemons with lots of state (pending-request tables, callbacks) are easier to keep
correct in C++, where RAII closes fds automatically and `std::function` stores callbacks.

**How.** `extern "C"` turns off C++ name mangling so `qmi_parse` keeps its C symbol name.
C++ RAII example: `QmiClient::~QmiClient` cancels timers and closes the fd
(`daemon/qmi_client.cpp`). `Reply` is **move-only** (`daemon/qmi_client.hpp`) because
its `qmi_msg` holds pointers into its own `frame` vector. A copy would point into the
*other* object's buffer, so copying is deleted at compile time.

**Verified by.** Every C++ test links the C libraries. `-Werror` makes the build fail on
any warning.

---

## 2. CMake + GoogleTest + ctest

**What.** `CMakeLists.txt` declares static libraries (`qmi`, `rtnl`, `ifctl`, `simcore`,
`mmcore`), programs and tests. `mlab_gtest()` is a helper that adds a gtest executable and
registers it with ctest.

**Why.** CMake is the standard build system for C/C++ on Linux and in AOSP-adjacent native code.
`ctest` lets one command run unit tests, shell-based system tests and plain C tests.

**How.** `target_include_directories(... PUBLIC ...)` passes include paths on to every
target that links the library. `$<TARGET_FILE:modem_sim>` is a *generator expression*:
CMake fills in the binary's real path, which the e2e test receives as `MODEM_SIM_PATH`.
`RUN_SERIAL` stops tests that create network interfaces from running at the same time.
`MLAB_REQUIRE_LIBQMI` makes configure fail if libqmi is missing, so the interop test
can't be skipped without anyone noticing.

**Verified by.** `./run-tests.sh` → `100% tests passed, 0 tests failed out of 10` (twice).

---

## 3. QMI wire format: QMUX + SDU + TLV

**What.** QMI (Qualcomm MSM Interface) is the binary request/response/indication protocol
between a host and a Qualcomm modem. Each message is a **QMUX header** (marker `0x01`,
LE16 length, flags, service, client id), then a **QMI SDU header** (flags, transaction id,
message id, TLV length), then **TLVs** (type u8, length LE16, value).
Diagram: `lib/qmi/qmi.h:7`.

**Why.** It is the control plane of most Android and Linux cellular devices. ModemManager,
qmicli and vendor RILs all speak it. The usual bugs are length miscounts and the two
CTL-vs-service differences described below.

**How.**
* Builder pattern with a *sticky error*: `qmi_build_*` never overflow. The first overflow
  is recorded and later calls do nothing (`lib/qmi/qmi.c`, `put()`).
* **Gotcha 1, txn-id width:** CTL uses a 1-byte transaction id, other services use 2 bytes
  (`lib/qmi/qmi.c:64`).
* **Gotcha 2, flag values:** the "response" bit is `0x01` for CTL but `0x02` for services,
  and "indication" is `0x02` vs `0x04` (`lib/qmi/qmi.c:11`).
* **Gotcha 3, length:** the QMUX length counts everything *except* the marker byte. Both
  length fields are filled in at the end (`lib/qmi/qmi.c:96`).
* Parsing is **zero-copy**: `qmi_msg.tlvs` points into the caller's buffer. The parser
  checks that the two length fields agree (`lib/qmi/qmi.c:153`) and walks the whole TLV area
  once (`lib/qmi/qmi.c:157`), so later `qmi_find_tlv` calls can't read past the end.
  This is defensive parsing: never trust a length field that came from the device.
* Every response has **result TLV 0x02** = `{u16 result, u16 error}`, written by
  `qmi_build_result` and read by `qmi_get_result`.

**Verified by.** `tests/test_qmi.cpp:19` compares encoded bytes with a hand-written byte array.
`RejectsMalformed` covers a bad marker, both length mismatches, a TLV overrun, unknown flags
and an oversized length.

---

## 4. The QMI services used: CTL, DMS, NAS

**What.**
* **CTL (0x00) Allocate Client ID 0x0022**: before a host can talk to DMS or NAS it
  asks CTL for a *client id*. Request TLV 0x01 = service; response TLV 0x01 = {service, cid}.
* **DMS (0x02) Get Revision 0x0023** → TLV 0x01 firmware string. **Get IDs 0x0025**
  ("device serial numbers") → TLVs 0x10 ESN, 0x11 IMEI, 0x12 MEID.
* **NAS (0x03) Get Signal Strength 0x0020** → TLV 0x01 {int8 dBm, u8 radio interface}.
  **Event Report 0x0002** is sent as an *indication* with TLV 0x10 = signal strength.

**Why.** Client ids let several programs (ModemManager, qmicli, a RIL) share one modem
without getting each other's responses. Indications are how the modem *pushes* changes
(signal, registration) instead of being polled.

**How.** `sim/sim_core.c:37` allocates client ids and records them in a bitmap. Requests on
an unallocated cid fail with `QMI_ERR_INVALID_CLIENT_ID`. Responses copy the request's
service, cid, txn and msg id (`sim/sim_core.c:24`). QMI strings are not NUL-terminated
(`sim/sim_core.c:75`). Indications come from `sim/sim_core.c:92`.

**Verified by.** `tests/test_sim_core.cpp` (4 tests), `test_e2e_sim`, `e2e_daemon`.

---

## 5. IPC over a Unix domain socket, and why framing is needed

**What.** `AF_UNIX, SOCK_STREAM` sockets connect two processes on the same machine. The simulator
listens on a path. The daemon connects to it.

**Why.** A stream is a sequence of bytes with **no message boundaries**: one `recv()`
can return half a frame, or one and a half. The same is true of the real QMI char devices
(`/dev/cdc-wdm0`, `/dev/wwan0qmi0`). Any IPC on Android (binder aside), such as rild
sockets or netd, has to deal with this.

**How.** `FrameBuffer` (`daemon/frame_buffer.cpp`) appends bytes and only returns a frame once
`qmi_frame_length()` says the whole frame has arrived. It needs at least 3 bytes to read the
length (`lib/qmi/qmi.c:106`). If the stream is corrupt, it **drops one byte and looks for
the next 0x01** (`daemon/frame_buffer.cpp:21`) instead of throwing the whole buffer away.
The simulator does the same in C (`sim/modem_sim.c:111`).

**Verified by.** `tests/test_frame_buffer.cpp` (byte-by-byte, 2.5 frames, garbage then resync).
`test_qmi_client_socketpair.cpp:75` delivers a response in 3 chunks of 2, 9 and the rest.

---

## 6. epoll and nonblocking IO: an event loop with no threads

**What.** `epoll` lets one thread wait on many fds and tells it which ones are ready.
With `O_NONBLOCK`, `recv`/`send` return `EAGAIN` instead of blocking.

**Why.** Daemons like rild, netd and ModemManager handle the modem, netlink, timers and
clients all at once. One thread with epoll avoids locks and data races. A blocking call
in one place would stall everything else.

**How.**
* `EventLoop::run_once` (`daemon/event_loop.cpp:48`) uses the nearest timer deadline as the
  `epoll_wait` timeout. That is how timeouts work without timer threads.
* Handlers are held in `shared_ptr` and copied before they are called (`event_loop.cpp:63`),
  so a handler can remove *itself* (`del_fd`) safely in the middle of dispatch.
* **Writes:** `send()` may accept only part of the data. The rest stays in `tx_`
  (`daemon/qmi_client.cpp:147`), and **EPOLLOUT is enabled only while there is a backlog**.
  With level-triggered EPOLLOUT always on, the loop would spin at 100% CPU (see the same
  idea in `sim/modem_sim.c:69`).
* The simulator also puts a **timerfd** (`sim/modem_sim.c:190`, periodic indications) and a
  **signalfd** (`sim/modem_sim.c:188`, clean shutdown on SIGTERM) into epoll. On Linux,
  timers and signals can both be handled as file descriptors.

**Verified by.** Every client test runs on this loop. `e2e_daemon` runs the real daemon
for 800 ms while indications arrive.

---

## 7. Request/response matching, timeouts, async indications

**What.** Each request gets a transaction id. The modem copies it into the response. The
client keeps a table of in-flight requests keyed by `(service, client id, txn)`
(`daemon/qmi_client.hpp:83`).

**Why.** Responses can arrive **out of order**, late, or never (the modem resets). A
production RIL must never deliver a reply to the wrong caller, hang forever, or call a
callback twice.

**How.** (`daemon/qmi_client.cpp`)
* Txn allocation skips 0 and any id still in flight. The search is **bounded**: with
  255 CTL requests in flight there is no free 8-bit id, so the request is refused and no
  live id is reused (`:72`).
* The timeout timer captures the map **key**, not an iterator (`:110`). Iterators into an
  `unordered_map` become invalid on rehash.
* On a response the entry is erased **before** the callback runs (`:193`), because callbacks
  usually send the next request. That is how the whole bring-up chain in
  `modem_manager.cpp:116` works.
* A late reply after a timeout finds no entry and is ignored.
* Indications (SDU flag *indication*) go to a separate handler and never touch the pending
  table (`:177`).
* On EOF, `fail_all` (`:200`) completes every pending request with `Disconnected`.

**Verified by.** `test_qmi_client_socketpair.cpp`: `:95` out-of-order, `:114` timeout and late
reply (callback count stays 1), `:156` peer close, `:169` txn exhaustion. `test_e2e_sim` stops
the simulator process mid-request.

### Why a `socketpair()` test

`socketpair(AF_UNIX, SOCK_STREAM)` creates two connected sockets inside one process. The test
plays the modem on one end and decides exactly which bytes arrive and when (split, reordered,
never). That is deterministic, so the tests don't flake, unlike testing with a real
process and timing.

---

## 8. RTNETLINK: watching network interfaces

**What.** `AF_NETLINK / NETLINK_ROUTE` is how user space and the kernel exchange routing
objects. We use it in two ways: **dump** (`RTM_GETLINK` + `NLM_F_DUMP`, `lib/rtnl/rtnl.c:36`)
and **subscribe** (`bind` with `nl_groups = RTMGRP_LINK`, `lib/rtnl/rtnl.c:18`).

**Why.** When a modem's data interface comes up, goes down or changes MTU, connection managers
(ModemManager, NetworkManager, Android netd/ConnectivityService) learn about it this way,
not by polling.

**How.** A reply is a series of `nlmsghdr` messages. A dump ends with `NLMSG_DONE`, and errors
come back as `NLMSG_ERROR`. Each `RTM_NEWLINK` carries an `ifinfomsg` (index, flags) followed
by **rtattr TLVs** (`IFLA_IFNAME`, `IFLA_MTU`). That is the same TLV idea as QMI
(`lib/rtnl/rtnl.c:57`). Parsing is a pure function (`rtnl_parse`, `:75`), so it can be
unit-tested without a kernel.
`LinkMonitor` **subscribes first, then dumps** (`daemon/link_monitor.cpp:13`), so a change
that happens between the snapshot and the subscription is still seen. `ENOBUFS` means the
kernel dropped events because we read too slowly (`rtnl.c:110`).

**Verified by.** `tests/test_rtnl_parse.cpp` (hand-built messages, error, truncation) and
`tests/test_netlink_ioctl.cpp` (a real kernel with a dummy link, including RTM_DELLINK).

---

## 9. ioctl: configuring the interface

**What.** `ioctl(sockfd, SIOCGIFFLAGS/SIOCSIFFLAGS/SIOCGIFMTU/SIOCSIFMTU, &ifreq)` is
the classic `ifconfig` way to read and set interface flags and MTU. It works on any
`AF_INET` socket. Setting needs `CAP_NET_ADMIN`.

**Why.** Older code paths and vendor RIL code still bring the data netdev up and set its
MTU this way after a data call connects. Carriers often require an MTU such as 1430.

**How.** `ifctl_set_up` reads the flags, changes `IFF_UP`, and writes them back
(`lib/ifctl/ifctl.c:38`). SIOCSIFFLAGS *replaces all* flags, so writing only `IFF_UP` would
clear the others. `ifr_flags` is a `short`, so it is cast through `unsigned short`. Errors
are returned as `-errno` (for example `-ENODEV`, or `-EINVAL` for names of IFNAMSIZ or longer).
The daemon calls this after "modem ready" (`daemon/modem_manager.cpp:103`).

**Verified by.** `test_netlink_ioctl.cpp:64`: ioctl sets UP, then **netlink independently
reports** IFF_UP. The same is checked for DOWN and MTU 1430. `e2e_daemon` checks the log lines
`netlink: link wwan0 DOWN -> UP mtu=1430` and `UP -> DOWN`.

---

## 10. libqmi interoperability: is our wire format the real one?

**What.** `tests/test_libqmi_interop.c` passes our bytes to **upstream libqmi**
(`qmi_message_new_from_raw`, `:24`). libqmi is the library behind ModemManager and qmicli.
In the other direction, libqmi builds a message and our parser reads it (`:140`).

**Why.** A codec that only passes its own tests can be wrong in a self-consistent way.
An independent implementation catches that.

**How.** We check service, client id, txn id, message id and TLV values using libqmi's TLV
readers. We also check libqmi's **generated pretty-printer**, which knows the real message
definitions: it prints our bytes as `"Get Revision" (0x0023)`, `"Imei" (0x11)` and
`strength = '-77' radio_interface = 'lte'`. Two things came up along the way: libqmi keeps its
`QMI_MESSAGE_*` enums private, so the test uses raw hex message ids, and libqmi masks the IMEI
as `###` in printables because it is personal data.

**Verified by.** `test_libqmi_interop` passes against libqmi-glib **1.32.2** (Debian bookworm),
in both the normal and the ASan build.

---

## 11. Debugging: -Werror, ASan/UBSan, gdb, strace

**What/Why.** C and C++ bugs usually show up as memory corruption much later than the
mistake itself. Treating warnings as errors (`CMakeLists.txt:11`) and running sanitizers in CI
(`CMakeLists.txt:17`) catch many of them at the line where they happen.

**How.** `run-tests.sh` runs the full suite under ASan+UBSan with leak detection, using
`-fno-sanitize-recover` so any UB fails the test. `tests/lsan.supp` suppresses only GLib's
intentional process-lifetime allocations. LeakSanitizer can't run under ptrace, so the strace
test disables leak checking for the traced process only. gdb and strace recipes are in
README → Debugging.

**Verified by.** The ASan pass of `run-tests.sh` (10/10). `strace_ioctl` checks that strace
output contains `SIOCGIFFLAGS` and `SIOCSIFFLAGS`.

---

## 12. What this lab does not teach (be honest in interviews)

MHI/PCIe transport and Android RIL/telephony are explained conceptually in
README → "Not covered & why". A good way to say it: *"I built the user-space QMI/netlink/ioctl
layer against a simulated modem. On MHI hardware the same QMUX bytes go to
`/dev/wwan0qmi0`, and in Android the vendor RIL does what my daemon does, behind the IRadio HAL."*

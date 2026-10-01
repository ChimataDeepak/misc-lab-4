# LEARN: C (C11) for protocol and systems code

## 1. What

C is a small language that maps almost directly to machine memory. A variable is a fixed number of bytes at an address. A pointer is an address. Memory is never freed for you. In this lab, everything that touches **bytes on a wire** or **raw syscalls** is C11:

- `lib/qmi`: the QMI codec
- `lib/rtnl`: netlink
- `lib/ifctl`: ioctl
- `sim/`: the simulated modem

## 2. Why it exists (and why modem code uses it)

- **A stable ABI.** A C function can be called from C++, Java (JNI), Rust or Python without glue code. libqmi, libmbim, vendor RILs and HALs expose C interfaces for this reason.
- **Explicit memory layout.** A protocol header is a fixed sequence of bytes, and C lets you write exactly those bytes.
- **The kernel is C.** The kernel modules in `kernel/` are C too, and the user-space side reads the same `uapi` headers.

## 3. How it works: the patterns this lab relies on

### 3.1 Never cast a struct onto wire bytes; read byte by byte

A modem sends little-endian u16 values, and the field you want may be unaligned. Casting `*(uint16_t*)p` is undefined behaviour on an unaligned address and is wrong on a big-endian host. So the lab reads and writes one byte at a time:

```
 p[0] = 0x34, p[1] = 0x12   ->   qmi_get_le16(p) = 0x34 | (0x12 << 8) = 0x1234
```

`lib/qmi/qmi.h:139-140` defines `qmi_get_le16` / `qmi_put_le16`. The kernel does the same with `get_unaligned_le16` (`kernel/fake_mhi_modem/fake_mhi_modem.c`).

### 3.2 A builder with a "sticky" error

```
 qmi_build_begin(b, buf, cap, ...)   b->err = 0
 qmi_build_tlv(...)  -> put()        if len + n > cap: b->err = QMI_E_SHORT, write nothing
 qmi_build_tlv(...)  -> put()        later calls see the space is gone; nothing is written
 qmi_build_finish(b)                 returns b->err if set, else the length
```

The caller checks for an error **once** at the end and can never overflow `buf`. See `put()` at `lib/qmi/qmi.c:36` and `qmi_build_finish` at `lib/qmi/qmi.c:96`.

### 3.3 Zero-copy parsing with up-front validation

`qmi_parse` does not copy anything. It fills a `qmi_msg` whose `tlvs` field **points into the caller's buffer** (`lib/qmi/qmi.c:121`). That is cheap, but there is a rule: the buffer must outlive the `qmi_msg`. Because of that rule, the C++ `Reply` type cannot be copied (see LEARN-C++). Before it returns, the parser walks the whole TLV area once (`lib/qmi/qmi.c:157`), so any later lookup is safe.

### 3.4 Return `-errno`, not -1 plus a global

```c
int rc = ioctl(fd, req, ifr) < 0 ? -errno : 0;   // lib/ifctl/ifctl.c:21
```

Callers get `-ENODEV`, `-EPERM` and so on directly. This is the same convention the kernel uses.

### 3.5 `extern "C"` so C++ can link against C

`lib/qmi/qmi.h:35` and `:143` wrap the declarations in `extern "C" { ... }` when the header is compiled as C++. This turns off name mangling, so the C++ daemon finds the symbol `qmi_parse` and not `_Z9qmi_parsePKhmP7qmi_msg`.

### 3.6 Fixed-size buffers plus length checks everywhere

`sim/modem_sim.c` gives each client a 4 KiB read buffer (`RBUF`) and a 16 KiB write buffer (`WBUF`), and memmoves the unparsed tail down after each read (`sim/modem_sim.c:98-138`). There is no malloc on the hot path.

## 4. Where in this repo

| idea | location |
|---|---|
| LE16 helpers | `lib/qmi/qmi.h:139-140` |
| sticky-error builder | `lib/qmi/qmi.c:36` (`put`), `:48` (`begin`), `:96` (`finish`) |
| zero-copy parse with validation | `lib/qmi/qmi.c:121-168` |
| bounded TLV search | `lib/qmi/qmi.c:173` (`p + 3 <= m->tlv_len`) |
| strings are not NUL-terminated on the wire | `sim/sim_core.c:75`; `qmi_tlv_string` at `lib/qmi/qmi.c:196` adds the NUL |
| bitmap of allocated client ids | `sim/sim_core.c:50` |
| `-errno` convention | `lib/ifctl/ifctl.c:21`, `lib/rtnl/rtnl.c:12-24` |
| C11 + warnings as errors | `CMakeLists.txt:4`, `:11` |

## 5. How it is verified

```bash
export DOCKER_HOST=unix://$HOME/.rd/docker.sock
./run-tests.sh          # test_qmi, test_sim_core, test_rtnl_parse exercise the C code
```

The suite is built with `-Wall -Wextra -Werror -Wshadow -Wformat=2`, then rebuilt and rerun under ASan+UBSan. An out-of-bounds read in the C code would make the second pass fail.

## 6. Real vs simulated

All of the C code here is real and runs. Only the *modem* behind it is simulated.

## 7. Interview Q&A

1. **Why not `memcpy` a packed struct onto the frame?** Packing is a compiler extension. Endianness and alignment depend on the platform. With byte-wise LE helpers the code is portable and has no undefined behaviour.
2. **What is the risk of zero-copy parsing?** Lifetime. The parsed view points into a buffer. If that buffer is freed or moved (for example, a `std::vector` reallocates), you get a use-after-free. The lab makes `Reply` move-only and keeps the frame inside it.
3. **Why validate the whole TLV area during parse?** It is defensive parsing. Lengths come from the device. If you check them once, every later `find_tlv` stays within bounds.
4. **What does `extern "C"` do?** It gives functions C linkage, so their names are not mangled and C++ can link to them.
5. **`strncpy` pitfalls?** It may not write a NUL terminator. `lib/ifctl/ifctl.c:14-19` rejects names of `IFNAMSIZ` bytes or longer, then copies `IFNAMSIZ-1` bytes into a zeroed struct.
6. **Signed vs unsigned with `short ifr_flags`?** `IFF_*` bits above 0x7fff would sign-extend. The lab casts through `unsigned short` (`lib/ifctl/ifctl.c:32`).
7. **How do you report errors from a C library?** Return negative errno, or an enum with a `strerror`-style function such as `qmi_strerror` (`lib/qmi/qmi.c:210`).

## 8. Try it yourself

1. In `qmi_find_tlv`, change `p + 3 <= m->tlv_len` to `p <= m->tlv_len` and run `./run-tests.sh`. Watch ASan report it, then revert.
2. Write `qmi_build_le16(b, type, v)` next to `qmi_build_u8`, and add a gtest that checks its bytes.
3. Remove `extern "C"` from `qmi.h` and read the linker error. Every C++ reference to `qmi_*` becomes an undefined, mangled symbol.
4. Make the builder's `cap` 10 bytes and confirm `qmi_build_finish` returns `QMI_E_SHORT` (see `OverflowIsReportedNotWritten`, `tests/test_qmi.cpp:49`).

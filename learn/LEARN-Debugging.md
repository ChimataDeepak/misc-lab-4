# LEARN: debugging with -Werror, ASan/UBSan, gdb and strace

## 1. What

| tool | what it does |
|---|---|
| `-Wall -Wextra -Werror` | the compiler refuses code with warnings |
| **ASan** (AddressSanitizer) | instruments memory accesses; catches overflows, use-after-free, leaks (with LSan) |
| **UBSan** | catches undefined behaviour: signed overflow, misaligned loads, bad shifts |
| **gdb** | stops a program, inspects variables and stacks, attaches to running processes |
| **strace** | logs every system call at the boundary between user space and the kernel |

## 2. Why it exists

Memory bugs in C/C++ often show up far away from where they were made. Sanitizers move the failure to the faulty line. strace answers "what is this daemon actually asking the kernel?" faster than reading the code.

## 3. How it works

### Sanitizers in CI

```
cmake -DMLAB_SANITIZE=ON  -> -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
ctest with ASAN_OPTIONS=detect_leaks=1:abort_on_error=1  UBSAN_OPTIONS=halt_on_error=1
          LSAN_OPTIONS=suppressions=tests/lsan.supp     (GLib/libqmi lifetime caches only)
```

`-fno-sanitize-recover` means any undefined behaviour **fails** the test instead of only printing a message.

```
  ASan shadow memory:  every 8 app bytes <-> 1 shadow byte; redzones around allocations
  heap buffer [ redzone | 32 bytes | redzone ]  -> a read of byte 33 hits a redzone -> report + abort
```

### strace and ptrace clash with LSan

LeakSanitizer itself uses ptrace, so it cannot run inside a process that is already being straced. `tests/strace_ioctl.sh:10` sets `ASAN_OPTIONS=detect_leaks=0` for the traced process only.

### gdb recipes

```
gdb --args ./build/test_qmi_client_socketpair --gtest_filter='*OutOfOrder*'
(gdb) break mlab::QmiClient::handle_frame
(gdb) run
(gdb) p/x r.msg.txn_id
(gdb) p pending_.size()
(gdb) bt
(gdb) catch syscall epoll_wait          # stop each time the loop sleeps
gdb -p $(pidof modem_manager)           # attach (needs SYS_PTRACE in Docker)
```

### strace recipes

```
strace -f -e trace=ioctl ./build/ifctl_cli wwan0 up
strace -f -e trace=epoll_wait,sendto,recvfrom -s 64 -x ./build/modem_manager --socket /tmp/m.sock --run-ms 500
```

The `-x` output shows QMI bytes such as `\x01\x0f\x00...`. You can decode them with LEARN-QMI-protocol.

### Kernel-side debugging

The equivalent tools in the kernel stage:

- `dmesg`: the guest prints `fake_mhi|edu_pci|taint` lines (`kernel/qemu/init`).
- `/proc/interrupts`: shows the edu IRQ count rising.
- `modinfo`: shows vermagic (`kernel/qemu/build-and-boot.sh:11`).

## 4. Where in this repo

| item | location |
|---|---|
| warning flags | `CMakeLists.txt:11` |
| sanitizer option | `CMakeLists.txt:15-19` |
| sanitizer run | `run-tests.sh` (second build) |
| LSan suppressions | `tests/lsan.supp` |
| strace test | `tests/strace_ioctl.sh` |
| container caps | `run-tests.sh` (`--cap-add NET_ADMIN --cap-add SYS_PTRACE`) |
| gdb/strace in image | `Dockerfile` |

## 5. How it is verified

```bash
./run-tests.sh    # 2nd half = ASan+UBSan pass; strace_ioctl is a ctest entry
```

Rerun 2026-10-01: 10/10 under ASan+UBSan.

## 6. Real vs simulated

All real, inside a Debian bookworm container with SYS_PTRACE.

## 7. Interview Q&A

1. **ASan vs Valgrind?** ASan instruments at compile time, has about 2x overhead and needs a rebuild. Valgrind emulates the CPU, is 10-50x slower, and needs no rebuild.
2. **Why `-fno-sanitize-recover=all`?** Otherwise UBSan only prints, and the test still passes.
3. **Can ASan and TSan run together?** No. Their shadow memory schemes conflict, so you build them separately.
4. **What does `strace -f` add?** It follows child processes and threads.
5. **How do you debug a daemon that hangs?** `gdb -p`, then `thread apply all bt`. Or `strace -p` to see which syscall it is blocked in.
6. **Why suppress some leaks?** Library-lifetime allocations (GLib types) are by design. Suppress them narrowly, by library.
7. **How do you debug a kernel module?** dmesg/`pr_debug` with dynamic debug, ftrace, KASAN builds, and gdb on QEMU through `-s -S` plus `vmlinux` symbols.

## 8. Try it yourself

1. Break `qmi_find_tlv` (`lib/qmi/qmi.c:173`: `p <= m->tlv_len`). Run `./run-tests.sh` and read the ASan report. Revert.
2. Add `int x = INT_MAX; x++;` in a test and watch UBSan halt it.
3. Run `strace -c ./build/modem_manager ...` for a syscall histogram. Which syscall dominates?
4. Add `-s -S` to the QEMU command in `kernel/qemu/build-and-boot.sh`, connect `gdb-multiarch` to port 1234, and break on `fqmi_write`.

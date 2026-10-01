# LEARN: Linux kernel modules and character devices

## 1. What

A **kernel module** (`.ko`) is code that is loaded into a running kernel with `insmod` or `modprobe`. A **character device** is a `/dev` node whose `open/read/write/poll/ioctl` calls land in a driver's `file_operations`. `fake_mhi_modem.ko` creates `/dev/fake_qmi0`, a QMI control channel shaped like `/dev/wwan0qmi0`.

## 2. Why it exists

Hardware is controlled from the kernel, and user space reaches it through device nodes. A modem's QMI channel *is* a char device. Knowing both sides (the driver's fops and the user's syscalls) explains blocking reads, `EAGAIN`, `poll` readiness and ioctl ABIs.

## 3. How it works

### Build and load

```
Kbuild:  obj-m := fake_mhi_modem.o
make -C /lib/modules/$KVER/build M=$PWD modules      # uses THAT kernel's headers + config
modinfo fake_mhi_modem.ko | grep vermagic             # "6.1.0-53-arm64 SMP mod_unload aarch64"
insmod fake_mhi_modem.ko   -> module_init(fqmi_init) -> misc_register -> devtmpfs makes /dev/fake_qmi0
rmmod fake_mhi_modem       -> module_exit(fqmi_exit) -> misc_deregister
```

**vermagic**: a module only loads into a kernel with the same version string and config. That is why the lab boots the *same* Debian kernel whose headers it built against.

### Data path inside the module

```
user write(fd, frame)                       user read(fd, buf)
   -> fqmi_write                               -> fqmi_read
      memdup_user (copy_from_user)                lock; while count==0:
      lock                                           unlock; O_NONBLOCK? -> -EAGAIN
      state != M0 -> -ENETDOWN                       wait_event_interruptible(readq, count>0)
      queue full  -> -EAGAIN                         (signal -> -ERESTARTSYS)
      fqmi_handle -> reply into ring slot         n < frame len -> -EMSGSIZE (whole frames only)
      count++; wake_up_interruptible(readq)       copy_to_user; head++; count--
      unlock                                      unlock

poll(): poll_wait(readq); IN if count>0; OUT if state==M0 && queue not full
```

The ring holds 16 reply frames of up to 512 bytes each. One mutex protects it. The reader sleeps **without** holding the lock.

### State machine (named like MHI's)

```
RESET -> READY -> M0 (usable)        SET_STATE(SYS_ERR): writes fail -ENETDOWN
FQMI_IOC_RESET: flush the queue + client ids, go back to M0 (like a modem warm reset)
```

## 4. Where in this repo

| item | location |
|---|---|
| module + header comment (what it models) | `kernel/fake_mhi_modem/fake_mhi_modem.c:1-24` |
| uapi (shared with tests) | `kernel/include/fake_qmi_uapi.h` |
| in-kernel QMI handler | `kernel/fake_mhi_modem/fake_mhi_modem.c:129` |
| write | `:195` |
| read + wait queue | `:233`, `:245` |
| poll | `:267` |
| ioctl | `:281` |
| fops / miscdevice | `:324-339` |
| init / exit | `:341`, `:358` |
| Kbuild | `kernel/fake_mhi_modem/Kbuild` |
| guest test (26 checks) | `kernel/tests/test_fake_qmi.c` |
| build + QEMU boot | `kernel/qemu/build-and-boot.sh`, guest PID 1 `kernel/qemu/init` |

## 5. How it is verified

```bash
MLAB_KERNEL=1 ./run-tests.sh      # or: ./kernel/run-kernel-tests.sh
```

Inside Docker (`kernel/Dockerfile`, Debian bookworm), this:

1. builds against `linux-headers-6.1.0-*-arm64`
2. packs a busybox initramfs
3. boots that `vmlinuz` under `qemu-system-aarch64 -M virt,gic-version=3` with TCG (no KVM)
4. runs `insmod`, then `/test_fake_qmi`, then `rmmod`
5. greps the console for `GUEST-RESULT: rc=0`

Status: passed 26/26 at commit `1837752`. **Not rerun** while writing these notes.

## 6. Real vs simulated vs blocked

- **Real:** kernel code running in a real Linux 6.1 kernel. That covers fops, `copy_*_user`, the wait queue, poll, the ioctl ABI, misc device registration, and module load and unload.
- **Simulated:** the "modem" is a few lines of C inside the module. There is no MHI and no PCIe underneath. The state names are only labels.
- **Known bug:** the module's `QMI_ERR_INVALID_CLIENT_ID` is 0x0022. The real value is 0x0007 (libqmi `qmi-errors.h`). The guest test asserts 0x22, so it passes anyway.
- **Blocked:** loading into the Rancher VM's own `6.6.119-0-virt` kernel.
  - Headers for that exact version are gone from the Alpine mirror; only 6.6.142 is available.
  - Loading into the kernel every container shares was not permitted.
  - `HOST_INSMOD=1 ./kernel/run-kernel-tests.sh` is meant to try it, but **the script it calls, `kernel/host/host-insmod-attempt.sh` (`kernel/run-kernel-tests.sh:15`), does not exist in the repo.** As committed, that path fails with "file not found". It would not reach the vermagic refusal.

## 7. Interview Q&A

1. **What is vermagic, and why did host insmod not happen?** It is the version and config string checked at load time. A module built for 6.1 or 6.6.142 will not load into 6.6.119.
2. **Why release the mutex before sleeping in read?** A writer needs the lock to queue a reply and wake you. Sleeping while holding it would deadlock.
3. **What does `-ERESTARTSYS` do?** A signal interrupted the sleep. The kernel either restarts the syscall or returns EINTR, depending on `SA_RESTART`.
4. **How does `poll` work in a driver?** `poll_wait` registers the caller on the wait queue (it does not sleep), and the function returns the current readiness mask. `wake_up` later re-triggers it.
5. **misc device vs `alloc_chrdev_region` + `cdev`?** misc gives you one minor under major 10 with very little code. A full cdev is for drivers with many minors and their own major.
6. **Why `memdup_user` or `copy_from_user` and not a plain pointer access?** User pointers may be invalid or paged out, and SMAP or PAN forbids direct access. These helpers handle faults.
7. **What does `.compat_ioctl = compat_ptr_ioctl` give you?** 32-bit user processes can use the ioctls when the structs have the same layout.
8. **Global `fdev` with several openers?** All openers share one queue. A real driver keeps per-open state in `file->private_data`.

## 8. Try it yourself

1. Add per-open client state: allocate a struct in `.open`, store it in `file->private_data`, and route replies by cid.
2. Fix the error code to 0x0007 in both the module and `test_fake_qmi.c:88`, then rerun the kernel stage.
3. Write the missing `kernel/host/host-insmod-attempt.sh`. Have it install `linux-virt-dev`, build, `insmod`, and print the exact refusal. Run it only if you accept loading code into the shared VM kernel.
4. Add a debugfs file that dumps `fdev.stats`.

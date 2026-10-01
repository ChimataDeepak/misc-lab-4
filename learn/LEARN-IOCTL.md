# LEARN: ioctl (user space and driver side)

## 1. What

`ioctl(fd, request, arg)` is a catch-all system call for "device-specific operations" that do not fit `read`/`write`. The request number says what to do. `arg` is usually a pointer to a struct that is copied in, copied out, or both.

This lab uses ioctl from **both sides**:

- **user space:** the classic network ioctls `SIOCGIFFLAGS`, `SIOCSIFFLAGS`, `SIOCGIFMTU`, `SIOCSIFMTU`
- **kernel space:** custom ioctl ABIs on `/dev/fake_qmi0` and `/dev/edu0`

## 2. Why it exists

Control operations need a typed, synchronous call to a driver, such as "reset the modem", "get stats" or "bring the netdev up". Older RIL code and `ifconfig` configure interfaces with ioctl. Newer tools (`ip`) use netlink, but both paths are still in use.

## 3. How it works

### Network ioctl (read-modify-write)

```
struct ifreq { char ifr_name[IFNAMSIZ]; union { short ifr_flags; int ifr_mtu; ... } }

s = socket(AF_INET, SOCK_DGRAM)          // any socket works as the "handle"
ioctl(s, SIOCGIFFLAGS, &ifr)             // read the current flags
ifr.ifr_flags |= IFF_UP                  // change ONE bit
ioctl(s, SIOCSIFFLAGS, &ifr)             // write ALL flags back (needs CAP_NET_ADMIN)
```

SIOCSIFFLAGS replaces **every** flag, so writing only `IFF_UP` would clear the others (`lib/ifctl/ifctl.c:36-50`).

### Request number encoding (driver side)

```
_IOR('Q', 1, __u32)   bits: [dir 2][size 14][type 8][nr 8]
 dir  = _IOC_READ (kernel writes to user)    size = sizeof(__u32)
 type = 'Q' (magic)                           nr   = 1
```

The fake modem defines its ABI in `kernel/include/fake_qmi_uapi.h`:

| request | encoding |
|---|---|
| GET_STATE | `_IOR` |
| RESET | `_IO` |
| SET_STATE | `_IOW` |
| GET_STATS | `_IOR` (struct) |

The edu driver uses `_IOR`/`_IOWR` (`kernel/include/edu_uapi.h`).

### Driver dispatch rules

```
unlocked_ioctl(file, cmd, arg):
  unknown cmd           -> return -ENOTTY      (not -EINVAL, not -EFAULT)
  _IOW: get_user / copy_from_user, then validate the value -> -EINVAL if out of range
  _IOR: copy_to_user / put_user -> -EFAULT if the user pointer is bad
```

**Bug the kernel tests caught:** `edu_ioctl` used to `copy_from_user` *before* checking whether the command was known. An unknown ioctl with a junk argument then returned EFAULT instead of ENOTTY. The fix validates `_IOC_TYPE` and `_IOC_NR` first (`kernel/edu_pci/edu_pci.c:83-86`).

`compat_ioctl = compat_ptr_ioctl` (`kernel/fake_mhi_modem/fake_mhi_modem.c:330`) lets 32-bit processes on a 64-bit kernel use the same ABI. That is safe here only because the uapi uses fixed-width `__u32` fields.

## 4. Where in this repo

| item | location |
|---|---|
| ifreq helper | `lib/ifctl/ifctl.c:12-24` |
| set up/down (RMW) | `lib/ifctl/ifctl.c:36-50` |
| set MTU | `lib/ifctl/ifctl.c:62-68` |
| CLI for strace | `tools/ifctl_cli.c` |
| daemon bring-up | `daemon/modem_manager.cpp:103-113` |
| fake modem ioctl ABI | `kernel/include/fake_qmi_uapi.h` |
| fake modem dispatch | `kernel/fake_mhi_modem/fake_mhi_modem.c:281-322` |
| edu dispatch | `kernel/edu_pci/edu_pci.c:73-132` |

## 5. How it is verified

```bash
./run-tests.sh                   # test_netlink_ioctl, strace_ioctl, e2e_daemon
MLAB_KERNEL=1 ./run-tests.sh     # test_fake_qmi + test_edu inside QEMU (passed at 1837752; not rerun here)
```

Checks:

- `test_netlink_ioctl.cpp:70`: ioctl sets UP, and netlink independently reports IFF_UP.
- `:88`: MTU 1430.
- `:103`: errors come back as `-errno`.
- `kernel/tests/test_fake_qmi.c:113-132`: SET_STATE, EINVAL on out-of-range, RESET, GET_STATS, and ENOTTY.
- `kernel/tests/test_edu.c:39`: ENOTTY.

## 6. Real vs simulated

- The network ioctls run against the real VM kernel, on a dummy link.
- The driver-side ioctls ran in a real Debian 6.1 kernel inside QEMU. They were not run in the Rancher VM's own kernel.

## 7. Interview Q&A

1. **Why read-modify-write for SIOCSIFFLAGS?** It replaces the whole flags word.
2. **Which errno for an unknown ioctl?** `ENOTTY`, by convention ("inappropriate ioctl for device").
3. **What do `_IOR` and `_IOW` mean?** Direction from user space's point of view. `_IOR`: the kernel writes to user memory, so user space reads.
4. **Why validate the cmd before `copy_from_user`?** Otherwise a bad pointer turns an unknown command into EFAULT, and you touch user memory needlessly.
5. **Why fixed-width types in uapi structs?** So the layout is identical for 32- and 64-bit callers, which `compat_ioctl` depends on.
6. **Which capability do network ioctls need?** CAP_NET_ADMIN for setting values. Getting values needs no privilege.
7. **ioctl vs netlink vs sysfs?** ioctl suits synchronous device commands. Netlink suits structured, async, multicast events. sysfs suits single attributes.

## 8. Try it yourself

1. Add `SIOCSIFNAME` (rename) to `ifctl`. Note that the interface must be down first.
2. Add `FQMI_IOC_INJECT_IND` (`_IOW` with a signal value) to the fake module, which queues a NAS indication. Test it with `poll`.
3. Run `strace -e trace=ioctl -e raw=ioctl ./build/ifctl_cli wwan0 up` and decode the numbers by hand.

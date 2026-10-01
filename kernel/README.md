# kernel/: out-of-tree modules, tested in a real booted kernel

`./kernel/run-kernel-tests.sh` builds two modules against Debian bookworm's
`linux-headers-6.1.0-*-arm64`, boots that exact `linux-image` under
`qemu-system-aarch64 -M virt,gic-version=3 -device edu` (TCG, inside Docker, no KVM),
and runs static user-space tests from a busybox initramfs.

| module | what is real | what is modelled |
|---|---|---|
| `fake_mhi_modem.ko` -> `/dev/fake_qmi0` | misc char device, read/write of QMUX frames, blocking read on a wait queue, poll, `_IOR/_IOW/_IO` ioctls (GET_STATE, SET_STATE, RESET, GET_STATS) | the modem itself, MHI states (RESET/READY/M0/SYS_ERR); no MHI rings, no PCIe. Plays the role `mhi_wwan_ctrl` plays for `/dev/wwan0qmi0` |
| `edu_pci.ko` -> `/dev/edu0` | a PCI driver bound by the PCI core to QEMU's emulated PCIe device 1234:11e8: `pcim_enable_device`, BAR0 MMIO, `pci_alloc_irq_vectors` (gets MSI via GICv3 ITS), ISR + completion | the device is QEMU's teaching device, not a modem |

Tests: `tests/test_fake_qmi.c` (26 checks), `tests/test_edu.c` (10 checks).

Host Rancher VM kernel (`6.6.119-0-virt`): not loaded. Exact headers are no longer on
the Alpine mirror (404), and loading code into the kernel every container shares is
opt-in only: `HOST_INSMOD=1 ./kernel/run-kernel-tests.sh` (builds against the nearest
`linux-virt-dev`, expect a vermagic refusal unless versions match).

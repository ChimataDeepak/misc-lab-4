#!/bin/bash
# Runs inside the kernel/Dockerfile image. Builds both modules against the
# installed Debian headers, builds static test binaries, packs an initramfs,
# boots the matching Debian kernel in QEMU with an emulated edu PCI device.
set -euo pipefail
KVER=$(ls /lib/modules | head -1)
KIMG=/boot/vmlinuz-$KVER
echo "HOST(container): building for $KVER"
for m in fake_mhi_modem edu_pci; do
  make -s -C /lib/modules/$KVER/build M=/lab/$m modules
  modinfo /lab/$m/$m.ko | grep -E '^(vermagic|alias)' | sed "s/^/  $m /"
done
mkdir -p /out/root/{bin,proc,sys,dev}
gcc -O2 -Wall -Wextra -Werror -static -o /out/root/test_fake_qmi /lab/tests/test_fake_qmi.c
gcc -O2 -Wall -Wextra -Werror -Wno-unused-result -static -o /out/root/test_edu /lab/tests/test_edu.c
cp /bin/busybox /out/root/bin/busybox
cp /lab/qemu/init /out/root/init && chmod +x /out/root/init
cp /lab/fake_mhi_modem/fake_mhi_modem.ko /lab/edu_pci/edu_pci.ko /out/root/
(cd /out/root && find . | cpio -o -H newc --quiet | gzip) > /out/initramfs.gz
ACCEL=tcg; [ -e /dev/kvm ] && ACCEL=kvm
echo "HOST(container): booting $KIMG under qemu-system-aarch64 ($ACCEL) with -device edu"
CPU=max; [ $ACCEL = kvm ] && CPU=host
timeout 600 qemu-system-aarch64 -M virt,gic-version=3 -accel $ACCEL -cpu $CPU -smp 2 -m 1024 \
  -nographic -no-reboot -kernel "$KIMG" -initrd /out/initramfs.gz \
  -append "console=ttyAMA0 panic=-1 quiet loglevel=4" \
  -device edu | tee /out/console.log
grep -q "GUEST-RESULT: rc=0" /out/console.log

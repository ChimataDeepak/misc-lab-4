# Kernel-module lab: Debian bookworm kernel + its exact headers + QEMU.
# The module is built against linux-headers-<ver>-arm64 and booted on the SAME
# linux-image-<ver>-arm64 under qemu-system-aarch64, so vermagic matches.
FROM debian:bookworm
RUN sed -i 's|http://|https://|g' /etc/apt/sources.list.d/debian.sources \
 && apt-get -o Acquire::https::Verify-Peer=false update \
 && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates \
 && apt-get update && apt-get install -y --no-install-recommends \
      build-essential kmod cpio busybox-static \
      linux-image-arm64 linux-headers-arm64 \
      qemu-system-arm ipxe-qemu qemu-efi-aarch64 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /lab
COPY . /lab

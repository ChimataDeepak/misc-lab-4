#!/bin/bash
# Kernel-module tests for misc-lab-4.
#  1. Build fake_mhi_modem.ko + edu_pci.ko against Debian bookworm's kernel headers.
#  2. Boot that exact Debian kernel under qemu-system-aarch64 (inside Docker) with
#     an emulated PCIe "edu" device, insmod both modules, run the user-space tests.
#  3. Optional (HOST_INSMOD=1): try insmod into the Rancher Desktop VM kernel itself.
#     Off by default: it loads code into the kernel every container shares.
set -euo pipefail
cd "$(dirname "$0")"
: "${DOCKER_HOST:=unix://$HOME/.rd/docker.sock}"; export DOCKER_HOST
docker build -q -t misc-lab-4-kernel . >/dev/null
docker run --rm misc-lab-4-kernel /lab/qemu/build-and-boot.sh
echo "kernel tests (QEMU guest): PASSED"
if [ "${HOST_INSMOD:-0}" = 1 ]; then
  docker run --rm --privileged -v "$PWD:/lab:ro" alpine:3.20 sh /lab/host/host-insmod-attempt.sh
fi

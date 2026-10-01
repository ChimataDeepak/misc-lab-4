#!/usr/bin/env bash
# One command: build the Docker image, then inside it build + run every test
# twice: a normal -Werror build, and an ASan/UBSan build.
#   --cap-add NET_ADMIN : create dummy links, ioctl SIOCSIF*, netlink events
#   --cap-add SYS_PTRACE: strace/gdb (and LeakSanitizer) inside the container
set -euo pipefail
cd "$(dirname "$0")"
: "${DOCKER_HOST:=unix://$HOME/.rd/docker.sock}"; export DOCKER_HOST
docker build -q -t misc-lab-4 . >/dev/null
docker run --rm -e REQUIRE_NET_ADMIN=1 --cap-add NET_ADMIN --cap-add SYS_PTRACE misc-lab-4 bash -euo pipefail -c '
  echo "===== build: -Wall -Wextra -Werror ====="
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMLAB_REQUIRE_LIBQMI=ON >/dev/null
  cmake --build build -j"$(nproc)" > build.log 2>&1 || { cat build.log; exit 1; }
  (cd build && ctest --output-on-failure)
  echo "===== build: ASan + UBSan ====="
  cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMLAB_SANITIZE=ON -DMLAB_REQUIRE_LIBQMI=ON >/dev/null
  cmake --build build-asan -j"$(nproc)" > build-asan.log 2>&1 || { cat build-asan.log; exit 1; }
  (cd build-asan && ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
     UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
     LSAN_OPTIONS=suppressions=/src/tests/lsan.supp \
     ctest --output-on-failure)
'

# ---- Optional stages (off by default; each prints RUN/PASS, FAIL or SKIPPED + reason) ----
#   MLAB_KERNEL=1  : build kernel/ modules, boot Debian 6.1 arm64 under QEMU (TCG), insmod + test
#   MLAB_ANDROID=1 : NDK r27c cross-build (bionic, AIDL, JNI) + run static bionic bins on this kernel
optional_stage() { # name envvar script...
  local name=$1 var=$2; shift 2
  if [ "${!var:-0}" != 1 ]; then echo "===== optional: $name: SKIPPED (set $var=1 to run; slow, downloads images)"; return 0; fi
  echo "===== optional: $name: RUNNING"
  if "$@"; then echo "===== optional: $name: PASSED"; else echo "===== optional: $name: FAILED"; return 1; fi
}
optional_stage "kernel modules in QEMU guest" MLAB_KERNEL ./kernel/run-kernel-tests.sh
optional_stage "android NDK build + static bionic run" MLAB_ANDROID ./android/build-android.sh
echo "===== not run by any stage: insmod into the Rancher VM kernel (HOST_INSMOD=1 in kernel/, no matching 6.6.119 headers);"
echo "      AIDL service / libmodemjni.so execution (no binder, no ART, no linker64 in this VM)"

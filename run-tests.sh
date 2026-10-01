#!/usr/bin/env bash
# One command: build the Docker image, then inside it build + run every test
# twice: a normal -Werror build, and an ASan/UBSan build.
#   --cap-add NET_ADMIN : create dummy links, ioctl SIOCSIF*, netlink events
#   --cap-add SYS_PTRACE: strace/gdb (and LeakSanitizer) inside the container
set -euo pipefail
cd "$(dirname "$0")"
: "${DOCKER_HOST:=unix://$HOME/.rd/docker.sock}"; export DOCKER_HOST
docker build -q -t misc-lab-4 . >/dev/null
docker run --rm --cap-add NET_ADMIN --cap-add SYS_PTRACE misc-lab-4 bash -euo pipefail -c '
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

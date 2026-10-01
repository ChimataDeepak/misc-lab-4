#!/usr/bin/env bash
# Debugging-tools check: strace really shows the ioctls our code issues.
set -euo pipefail
CLI=$1; IF=mlabstrace0
cleanup() { ip link del "$IF" 2>/dev/null || true; }
trap cleanup EXIT
ip link add "$IF" type dummy
# LeakSanitizer refuses to run under ptrace, so disable just leak checking
# for the traced process (no-op for the non-sanitizer build).
OUT=$(ASAN_OPTIONS=detect_leaks=0 strace -f -e trace=ioctl "$CLI" "$IF" up 2>&1) || { echo "$OUT"; exit 1; }
echo "$OUT"
echo "$OUT" | grep -q "SIOCGIFFLAGS" && echo "$OUT" | grep -q "SIOCSIFFLAGS"
"$CLI" "$IF" show | grep -q " UP "
echo "strace_ioctl: PASS"

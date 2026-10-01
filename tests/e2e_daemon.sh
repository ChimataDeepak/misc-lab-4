#!/usr/bin/env bash
# Full-system test: modem_sim process + modem_manager daemon + a dummy "wwan0".
# Proves: QMI bring-up chain, async indications, ioctl bring-up, netlink observing it.
set -euo pipefail
SIM=$1; MM=$2
IF=wwan0
SOCK=/tmp/mlab-daemon-$$.sock
LOG=$(mktemp)
cleanup() { kill "$SIM_PID" 2>/dev/null || true; ip link del "$IF" 2>/dev/null || true; rm -f "$LOG"; }
trap cleanup EXIT

ip link del "$IF" 2>/dev/null || true
ip link add "$IF" type dummy          # stand-in for a real modem netdev (needs NET_ADMIN)

"$SIM" --socket "$SOCK" --ind-ms 100 > /dev/null 2>&1 &
SIM_PID=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

"$MM" --socket "$SOCK" --iface "$IF" --bring-up --mtu 1430 --run-ms 800 | tee "$LOG"

expect() { grep -q -- "$1" "$LOG" || { echo "MISSING: $1"; exit 1; }; }
expect "netlink: found link $IF .* state=DOWN"
expect "DMS revision: MLAB-SIM-MODEM"
expect "DMS IMEI: 490154203237518"
expect "NAS signal strength: -70 dBm"
expect "modem ready"
expect "ioctl: $IF is now UP mtu=1430"
expect "netlink: link $IF DOWN -> UP mtu=1430"
expect "indication: NAS signal"
expect "netlink: link $IF UP -> DOWN"
expect "errors=0"
echo "e2e_daemon: PASS"

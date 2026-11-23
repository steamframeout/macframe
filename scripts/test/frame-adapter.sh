#!/bin/bash
# guest/layer/usr/lib/steamac/frame-adapter against a stand-in USB/IP server (scripts/test/fake-usbipd.py: two
# devices, the adapter second, SuperSpeed) and a stand-in vhci-hcd directory: the adapter is found in the device
# list, imported by its bus id and attached on a SuperSpeed port with the server's devid and speed; a used port
# means no new attach; no server is a quiet retry; steamac.frame_adapter=0 does nothing.
# usage: scripts/test/frame-adapter.sh
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
S=$R/guest/layer/usr/lib/steamac/frame-adapter
T=$(mktemp -d)
trap 'kill $srv 2>/dev/null; rm -rf "$T"' EXIT
port=$((20000 + RANDOM % 20000))
python3 "$R/scripts/test/fake-usbipd.py" "$port" > "$T/srv.log" 2>&1 & srv=$!
sleep 0.5
vhci() {
    mkdir -p "$T/vhci"
    { echo "hub port sta spd dev      sockfd local_busid"
      for i in $(seq 0 7); do printf "hs  %04u 004 000 00000000 000000 0-0\n" "$i"; done
      for i in $(seq 8 15); do printf "ss  %04u ${1:-004} 000 00000000 000000 0-0\n" "$i"; done; } > "$T/vhci/status"
    : > "$T/vhci/attach"
}
run() { STEAMAC_CMDLINE="$T/cmdline" STEAMAC_USBIP_HOST=127.0.0.1 STEAMAC_USBIP_PORT=${2:-$port} STEAMAC_VHCI="$T/vhci" \
        STEAMAC_ONCE=1 bash "$S" > "$T/out" 2>&1; cat "$T/out"; }
fail() { echo "FAIL: $*"; exit 1; }

echo "steamac.country=DE" > "$T/cmdline"
vhci; run
[ "$(cat "$T/vhci/attach")" = "8 3 131075 5" ] || fail "attach wrote '$(cat "$T/vhci/attach")'"
grep -q "import 2-1" "$T/srv.log" || fail "no import of 2-1"

vhci 006; run
[ ! -s "$T/vhci/attach" ] || fail "attached although a port is in use"

vhci; run "" $((port + 1))
[ ! -s "$T/vhci/attach" ] || fail "attached without a server"
grep -qi "error\|refused" "$T/out" && fail "noise without a server: $(cat "$T/out")"

echo "steamac.frame_adapter=0" > "$T/cmdline"
vhci; run
grep -q "off" "$T/out" && [ ! -s "$T/vhci/attach" ] || fail "steamac.frame_adapter=0 not honoured"
echo "frame-adapter: ok"

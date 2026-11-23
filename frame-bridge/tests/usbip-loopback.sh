#!/bin/bash
# USB/IP path end to end, without the adapter: the bridge image in MODE=usbip exports an emulated USB device (QEMU's
# usb-kbd, 0627:0001, instead of 28de:2432), and the SteamOS side (guest/layer/usr/lib/steamac/frame-adapter, run
# with bash and GNU coreutils as on SteamOS) attaches that export over TCP with vhci-hcd inside the same VM. Passes
# when the device enumerates a second time behind vhci-hcd (descriptors read over USB/IP) and the export reports
# itself in use.
#
#   frame-bridge/tests/usbip-loopback.sh WORK [kbd|storage]   (WORK: the build-image.sh work dir, with out/ and apk/)
#     kbd       usb-kbd, a high-speed device (vhci "hs" ports)
#     storage   usb-storage, SuperSpeed on QEMU's xHCI like the adapter (vhci "ss" ports)
# Needs qemu-system-aarch64 (TCG is fine), python3, readelf for aarch64 (binutils-aarch64-linux-gnu on x86-64).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${1:?usage: $0 WORK [kbd|storage]}
T=$(mktemp -d); truncate -s 16M "$T/stick.img"
trap 'rm -rf "$T"' EXIT
case ${2:-kbd} in
  kbd) DEV=(-device usb-kbd,bus=xhci.0); ID=0627:0001 ;;
  storage) DEV=(-drive if=none,id=stick,format=raw,file=$T/stick.img -device usb-storage,bus=xhci.0,drive=stick); ID=46f4:0001 ;;
  *) echo "unknown device $2" >&2; exit 64 ;;
esac
READELF=$(command -v aarch64-linux-gnu-readelf || command -v readelf)
KREL=$(ls "$WORK/root/lib/modules")

echo "== test userspace (bash, GNU coreutils) and vhci-hcd"
python3 -I "$REPO/frame-bridge/adapter-bridge/tools/fetch_alpine.py" --cache "$WORK/apk" --root "$T/root" bash coreutils >/dev/null
S=$T/stage
mkdir -p "$S/bin" "$S/usr/bin" "$S/lib" "$S/usr/lib" "$S/usr/lib/steamac" "$S/etc/bridge" "$S/test"
copy_elf() {  # copy a binary and the libraries it needs from the Alpine root
  local rel=$1 lib
  cp -L "$T/root/$rel" "$S/$rel"
  for lib in $("$READELF" -d "$T/root/$rel" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    for d in lib usr/lib; do
      if [ -e "$T/root/$d/$lib" ] && [ ! -e "$S/$d/$lib" ]; then cp -L "$T/root/$d/$lib" "$S/$d/$lib"; copy_elf "$d/$lib"; fi
    done
  done
}
copy_elf bin/bash
copy_elf bin/coreutils
for t in head od tr; do ln -sf ../../bin/coreutils "$S/usr/bin/$t"; done
gzip -dc "$WORK/root/lib/modules/$KREL/kernel/drivers/usb/usbip/vhci-hcd.ko.gz" > "$S/test/vhci-hcd.ko"
install -m 755 "$REPO/guest/layer/usr/lib/steamac/frame-adapter" "$S/usr/lib/steamac/frame-adapter"
cat > "$S/etc/bridge/usbip-test.sh" <<'EOF'
#!/bin/sh
say() { echo "SELFTEST: $*" > /dev/console; }
i=0
until ls /sys/bus/usb/devices/*/usbip_status >/dev/null 2>&1 && pidof usbipd >/dev/null; do
  sleep 1; i=$((i + 1)); [ $i -gt 120 ] && { say "FAIL usbip loopback: no export"; exit 1; }
done
insmod /test/vhci-hcd.ko || { say "FAIL usbip loopback: vhci-hcd did not load"; exit 1; }
sleep 1
out=$(STEAMAC_CMDLINE=/dev/null STEAMAC_USBIP_HOST=127.0.0.1 STEAMAC_ADAPTER_ID=$USBIP_ID STEAMAC_ONCE=1 \
      bash /usr/lib/steamac/frame-adapter 2>&1)
echo "$out" | sed 's/^/SELFTEST:   /' > /dev/console
i=0
while :; do
  n=0; via=
  for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor"):$(cat "$d/idProduct")" = "$USBIP_ID" ] || continue
    n=$((n + 1))
    case "$(readlink -f "$d")" in *vhci_hcd*) via="$(cat "$d/product" 2>/dev/null) speed $(cat "$d/speed")" ;; esac
  done
  st=$(cat /sys/bus/usb/devices/*/usbip_status 2>/dev/null | head -1)
  if [ "$n" = 2 ] && [ -n "$via" ] && [ "$st" = 2 ]; then say "PASS usbip loopback: attached behind vhci-hcd ($via)"; exit 0; fi
  i=$((i + 1)); [ $i -gt 30 ] && break; sleep 1
done
say "FAIL usbip loopback: devices $n, vhci '$via', export status $st"
dmesg | grep -iE "usbip|vhci|usb " | tail -15 | sed 's/^/SELFTEST:   /' > /dev/console
EOF
chmod 755 "$S/etc/bridge/usbip-test.sh"
python3 - "$S" "$T/test.cpio" <<'EOF'
import os, sys, stat
stage, out = sys.argv[1:]
ino = 1000
def entry(f, name, st, data=b""):
    global ino; ino += 1
    hdr = "070701" + "".join("%08x" % v for v in (ino, st, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0))
    f.write(hdr.encode() + name.encode() + b"\0"); f.write(b"\0" * ((4 - (110 + len(name) + 1) % 4) % 4))
    f.write(data); f.write(b"\0" * ((4 - len(data) % 4) % 4))
with open(out, "wb") as f:
    for dp, dns, fns in os.walk(stage):
        for n in sorted(dns): p = os.path.join(dp, n); entry(f, os.path.relpath(p, stage), stat.S_IFDIR | 0o755)
        for n in sorted(fns):
            p = os.path.join(dp, n); rel = os.path.relpath(p, stage)
            if os.path.islink(p): entry(f, rel, stat.S_IFLNK | 0o777, os.readlink(p).encode())
            else: entry(f, rel, stat.S_IFREG | (os.stat(p).st_mode & 0o777), open(p, "rb").read())
    entry(f, "TRAILER!!!", 0)
EOF
gzip -9 < "$T/test.cpio" > "$T/test.cpio.gz"
cat "$WORK/out/bridge-initramfs.img" "$T/test.cpio.gz" > "$T/initrd"
printf 'MODE=usbip\nUSBIP_ID=%s\nDEBUG_SHELL=0\n' "$ID" > "$T/link.conf"

echo "== boot (TCG)"
timeout 600 qemu-system-aarch64 -M virt -cpu max -smp 2 -m 512 -nographic -no-reboot \
  -kernel "$WORK/out/bridge-kernel" -initrd "$T/initrd" -append "console=ttyAMA0 quiet" \
  -fw_cfg "name=opt/framebridge/link.conf,file=$T/link.conf" \
  -device qemu-xhci,id=xhci "${DEV[@]}" \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= > "$T/console.log" 2>&1 &
q=$!
rc=1
for _ in $(seq 1 600); do
  if grep -q "SELFTEST: PASS" "$T/console.log"; then rc=0; break; fi
  grep -q "SELFTEST: FAIL\|BRIDGE: ERROR" "$T/console.log" && break
  kill -0 $q 2>/dev/null || break
  sleep 1
done
kill $q 2>/dev/null; wait $q 2>/dev/null || true
grep -aE "BRIDGE:|SELFTEST:" "$T/console.log" | tr -d '\r'
[ $rc = 0 ] && echo "usbip-loopback: ok" || { echo "usbip-loopback: FAILED"; tail -30 "$T/console.log" | tr -d '\r'; }
exit $rc

#!/bin/bash
# Build the Steam Frame adapter bridge image: an ARM64 Linux kernel + initramfs that drives the Steam Frame Wireless
# Adapter (RTL8852CU) under QEMU on an Apple Silicon Mac.
#
# Runs on a Linux build box (x86-64 or arm64) with: python3, curl, make, flex, bison, bc, libssl-dev, libelf-dev,
# and an aarch64 cross compiler on x86-64 (aarch64-linux-gnu-gcc, binutils).
#
#   ./build-image.sh [WORK]    -> WORK/out/bridge-kernel, WORK/out/bridge-initramfs.img
#
# Userspace and the kernel come from Alpine edge (linux-lts). Alpine's kernel leaves CONFIG_RTW89_8852CU off, so the
# rtw89 stack (core, usb, 8852c, 8852cu) is rebuilt from the matching kernel.org source with Alpine's own config
# and Module.symvers: same release string, same symbol versions for everything it imports from the stock kernel.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$HERE/work}
KVER_UPSTREAM=${KVER_UPSTREAM:-7.3.5}
KREL=${KREL:-7.3.5-0-lts}
JOBS=${JOBS:-$(nproc)}
rm -rf "$WORK/root" "$WORK/stage"
mkdir -p "$WORK"/{apk,root,out}
case "$(uname -m)" in
  aarch64) CROSS= ; READELF=readelf ;;
  *) CROSS=aarch64-linux-gnu- ; READELF=aarch64-linux-gnu-readelf ;;
esac
export READELF

fetch() { python3 -I "$HERE/tools/fetch_alpine.py" --cache "$WORK/apk" --root "$WORK/root" "$@"; }

echo "== Alpine packages"
# the kernel package depends on firmware meta packages and initramfs tools that the bridge does not need
fetch --skip linux-firmware-any initramfs-generator mkinitfs -- linux-lts >/dev/null
fetch --skip linux-lts -- linux-lts-dev >/dev/null
fetch --skip bash bash-binsh busybox busybox-binsh -- \
  busybox-static linux-firmware-rtw89 wireless-regdb wpa_supplicant hostapd nftables iw dnsmasq linux-tools-usbip >/dev/null
grep -qx "$KREL" "$WORK"/root/usr/src/linux-headers-*/include/config/kernel.release ||
  { echo "Alpine linux-lts is not $KREL any more: set KVER_UPSTREAM/KREL to match it" >&2; exit 1; }
HDR=$(ls -d "$WORK"/root/usr/src/linux-headers-*)

echo "== rtw89 stack with RTW89_8852CU (kernel.org $KVER_UPSTREAM, Alpine config)"
SRC=$WORK/linux-$KVER_UPSTREAM
if [ ! -d "$SRC" ]; then
  TAR=$WORK/linux-$KVER_UPSTREAM.tar.xz
  [ -f "$TAR" ] || curl -fsSL -o "$TAR" "https://cdn.kernel.org/pub/linux/kernel/v${KVER_UPSTREAM%%.*}.x/linux-$KVER_UPSTREAM.tar.xz"
  want=$(curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v${KVER_UPSTREAM%%.*}.x/sha256sums.asc" |
         awk -v f="linux-$KVER_UPSTREAM.tar.xz" '$2 == f {print $1}')
  echo "$want  $TAR" | sha256sum -c - >/dev/null || { echo "kernel tarball checksum mismatch" >&2; exit 1; }
  tar -C "$WORK" -xJf "$TAR"
fi
K="make -C $SRC ARCH=arm64 CROSS_COMPILE=$CROSS LOCALVERSION=-${KREL#*-} -j$JOBS"
cp "$HDR/.config" "$SRC/.config"
"$SRC/scripts/config" --file "$SRC/.config" --module RTW89_8852CU
$K olddefconfig >/dev/null
$K modules_prepare >/dev/null
cp "$HDR/Module.symvers" "$SRC/Module.symvers"
$K M=drivers/net/wireless/realtek/rtw89 modules >/dev/null
mkdir -p "$WORK/rtw89"
for m in rtw89_core rtw89_usb rtw89_8852c rtw89_8852cu; do
  cp "$SRC/drivers/net/wireless/realtek/rtw89/$m.ko" "$WORK/rtw89/"
done
${CROSS}strip --strip-debug "$WORK"/rtw89/*.ko
vm=$(${CROSS}readelf -p .modinfo "$WORK/rtw89/rtw89_8852cu.ko" | sed -n 's/.*vermagic=\([^ ]*\).*/\1/p')
[ "$vm" = "$KREL" ] || { echo "vermagic $vm != $KREL" >&2; exit 1; }
${CROSS}readelf -p .modinfo "$WORK/rtw89/rtw89_8852cu.ko" | grep -q "usb:v28DEp2432" ||
  { echo "rtw89_8852cu has no Steam Frame adapter id (28de:2432)" >&2; exit 1; }
python3 -I "$HERE/tools/check_modversions.py" "$HDR/Module.symvers" "$WORK"/rtw89/*.ko

echo "== initramfs"
python3 -I "$HERE/tools/mkinitramfs.py" --root "$WORK/root" --kver "$KREL" --extra-modules "$WORK/rtw89" \
  --modules qemu_fw_cfg af_packet virtio_pci virtio_net xhci_pci usbcore rtw89_8852cu nf_tables nft_chain_nat nft_nat \
            nft_masq ctr ccm gcm cmac seqiv aes_ce_ccm aes_ce_blk ghash_ce mac80211_hwsim usbip_host \
  --bins bin/busybox.static sbin/wpa_supplicant sbin/wpa_cli usr/sbin/hostapd usr/bin/hostapd_cli \
         usr/sbin/nft usr/sbin/iw usr/sbin/dnsmasq usr/sbin/usbipd usr/sbin/usbip \
  --firmware "rtw89/rtw8852c_fw*" "regulatory.db" "regulatory.db.p7s" \
  --overlay "$HERE/overlay" --stage "$WORK/stage" --out "$WORK/out/bridge-initramfs.img"
cp "$WORK/root/boot/vmlinuz-lts" "$WORK/out/bridge-kernel"
( cd "$WORK/out" && sha256sum bridge-kernel bridge-initramfs.img > SHA256SUMS )
echo "== done: $WORK/out"
ls -la "$WORK/out"

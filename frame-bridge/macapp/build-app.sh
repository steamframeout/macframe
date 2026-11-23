#!/bin/bash
# Build "Frame Bridge.app" (Apple Silicon, macOS 15+) on a Linux box.
#
#   ./build-app.sh [WORK]   -> WORK/Frame Bridge.app and WORK/FrameBridge.zip
#
# Needs: python3 with macholib and Pillow, rcodesign (cargo install apple-codesign), the adapter bridge image
# (../adapter-bridge/image, from ../adapter-bridge/build-image.sh).
# QEMU comes from Homebrew's arm64_sequoia bottles (checksum-verified), made self-contained (bundle_qemu.py) and
# ad-hoc signed; qemu-system-aarch64 carries the com.apple.security.hypervisor entitlement for Apple's hypervisor.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$HERE/work}
RCODESIGN=${RCODESIGN:-rcodesign}
VERSION=${VERSION:-0.1.0}
APP="$WORK/Frame Bridge.app"
IMAGE="$HERE/../adapter-bridge/image"
command -v "$RCODESIGN" >/dev/null || { echo "rcodesign not found (cargo install apple-codesign)" >&2; exit 1; }
for f in bridge-kernel bridge-initramfs.img SHA256SUMS; do [ -f "$IMAGE/$f" ] || { echo "missing $IMAGE/$f" >&2; exit 1; }; done
mkdir -p "$WORK"

echo "== QEMU (Homebrew bottles)"
python3 -I "$HERE/tools/fetch_bottles.py" --cache "$WORK/bottles" --out "$WORK/cellar" qemu | tr '\n' ' '; echo
python3 "$HERE/tools/bundle_qemu.py" --cellar "$WORK/cellar" --out "$WORK/qemu" | head -1

echo "== signing"
for f in "$WORK"/qemu/lib/*.dylib; do
  "$RCODESIGN" sign --binary-identifier "$(basename "$f" .dylib)" "$f" >/dev/null 2>&1 || { echo "signing $f failed" >&2; exit 1; }
done
"$RCODESIGN" sign --binary-identifier org.qemu.qemu-system-aarch64 \
  --entitlements-xml-file "$HERE/qemu-entitlements.plist" "$WORK/qemu/bin/qemu-system-aarch64" >/dev/null 2>&1
"$RCODESIGN" print-signature-info "$WORK/qemu/bin/qemu-system-aarch64" 2>/dev/null | grep -q "com.apple.security.hypervisor" ||
  { echo "qemu lacks the hypervisor entitlement" >&2; exit 1; }

echo "== app bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/image"
cp "$HERE/FrameBridge" "$APP/Contents/MacOS/FrameBridge"
chmod 755 "$APP/Contents/MacOS/FrameBridge"
cp -R "$WORK/qemu" "$APP/Contents/Resources/qemu"
cp "$IMAGE"/{bridge-kernel,bridge-initramfs.img,SHA256SUMS} "$APP/Contents/Resources/image/"
cp "$HERE/../adapter-bridge/frame/probe-on-frame.sh" "$APP/Contents/Resources/"
python3 "$HERE/tools/make_icon.py" "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>Frame Bridge</string>
	<key>CFBundleDisplayName</key><string>Frame Bridge</string>
	<key>CFBundleIdentifier</key><string>app.framebridge.FrameBridge</string>
	<key>CFBundleExecutable</key><string>FrameBridge</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>LSMinimumSystemVersion</key><string>15.0</string>
	<key>LSArchitecturePriority</key><array><string>arm64</string></array>
	<key>LSUIElement</key><true/>
	<key>NSLocalNetworkUsageDescription</key><string>Frame Bridge connects to your Steam Frame on your network to read its wireless link settings.</string>
	<key>NSHumanReadableCopyright</key><string>Scripts Apache-2.0. Includes QEMU (GPL-2.0) and a Linux image (GPL-2.0 kernel, Alpine packages).</string>
</dict>
</plist>
EOF
printf 'APPL????' > "$APP/Contents/PkgInfo"

echo "== zip"
rm -f "$WORK/FrameBridge.zip"
(cd "$WORK" && zip -qry FrameBridge.zip "Frame Bridge.app")
du -sh "$APP" "$WORK/FrameBridge.zip"

#!/bin/bash
# Runs ON the Steam Frame (SteamOS, developer mode), as root. Read-only: changes nothing on the headset.
#
# Finds the headset's dedicated 6 GHz link (the network the Steam Frame Wireless Adapter joins) and prints:
#   - a report of the radios, access points and network services involved
#   - between "---- link.conf ----" markers: settings for the Mac's adapter bridge
# Secrets (the link passphrase) are printed only in the link.conf block, so the report can be shared.
set -u
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

say() { printf '%s\n' "$*"; }
hdr() { printf '\n== %s\n' "$*"; }
SSID= PSK= KEY_MGMT= FREQ= IFACE= COUNTRY= SOURCE=

hdr "system"
grep -E '^(PRETTY_NAME|VERSION_ID|BUILD_ID)=' /etc/os-release 2>/dev/null
uname -r
iw reg get 2>/dev/null | sed -n '1,3p'
COUNTRY=$(iw reg get 2>/dev/null | sed -n 's/^country \([A-Z0-9][A-Z0-9]\):.*/\1/p' | head -1)

hdr "wireless interfaces"
iw dev 2>/dev/null | grep -E 'phy#|Interface|type|ssid|channel|addr' | sed 's/^\t*/  /'
# an interface in AP mode on 6 GHz is the link
while read -r ifc; do
  info=$(iw dev "$ifc" info 2>/dev/null)
  type=$(printf '%s\n' "$info" | sed -n 's/.*type \(.*\)/\1/p' | head -1)
  fr=$(printf '%s\n' "$info" | sed -n 's/.*channel [0-9]* (\([0-9]*\) MHz).*/\1/p' | head -1)
  if [ "$type" = AP ] && [ -n "$fr" ] && [ "$fr" -ge 5925 ]; then
    IFACE=$ifc FREQ=$fr
    SSID=$(printf '%s\n' "$info" | sed -n 's/^[[:space:]]*ssid \(.*\)/\1/p' | head -1)
    say "  -> $ifc is an access point on 6 GHz ($fr MHz), ssid \"$SSID\""
  fi
done < <(iw dev 2>/dev/null | awk '/Interface/ {print $2}')
[ -n "$IFACE" ] || say "  (no 6 GHz access point running right now)"

hdr "addresses"
ip -br addr 2>/dev/null

hdr "hostapd"
if pgrep -a hostapd >/dev/null 2>&1; then
  pgrep -a hostapd
  for pid in $(pgrep hostapd); do
    for f in $(tr '\0' '\n' < /proc/$pid/cmdline | grep -E '\.conf$'); do
      [ -f "$f" ] && grep -q '^ssid=' "$f" || continue
      say "  config $f:"
      grep -vE '^(#|$)|passphrase|password|psk=' "$f" | sed 's/^/    /'
      if [ -z "$SOURCE" ] || [ "$(sed -n 's/^interface=//p' "$f")" = "$IFACE" ]; then
        s=$(sed -n 's/^ssid=//p' "$f" | head -1); [ -n "$s" ] && SSID=$s
        p=$(sed -n 's/^sae_password=//p; s/^wpa_passphrase=//p' "$f" | head -1); [ -n "$p" ] && PSK=$p
        k=$(sed -n 's/^wpa_key_mgmt=//p' "$f" | head -1); [ -n "$k" ] && KEY_MGMT=$k
        # 6 GHz operating classes 131-137: frequency = 5950 + 5 x channel
        oc=$(sed -n 's/^op_class=//p' "$f" | head -1); ch=$(sed -n 's/^channel=//p' "$f" | head -1)
        if [ -z "$FREQ" ] && [ -n "$oc" ] && [ "$oc" -ge 131 ] 2>/dev/null && [ "$oc" -le 137 ] && [ "$ch" -gt 0 ] 2>/dev/null; then
          FREQ=$((5950 + 5 * ch))
        fi
        SOURCE="hostapd $f"
      fi
    done
  done
else
  say "  not running"
fi

hdr "NetworkManager"
if command -v nmcli >/dev/null 2>&1; then
  nmcli -t -f NAME,TYPE,DEVICE,ACTIVE connection show 2>/dev/null | sed 's/^/  /'
  while IFS=: read -r uuid type; do
    [ "$type" = 802-11-wireless ] || continue
    mode=$(nmcli -g 802-11-wireless.mode connection show "$uuid" 2>/dev/null)
    band=$(nmcli -g 802-11-wireless.band connection show "$uuid" 2>/dev/null)
    name=$(nmcli -g connection.id connection show "$uuid" 2>/dev/null)
    say "  wifi \"$name\": mode $mode, band ${band:-any}"
    if [ "$mode" = ap ] || [ "$band" = 6GHz ]; then
      s=$(nmcli -g 802-11-wireless.ssid connection show "$uuid" 2>/dev/null)
      p=$(nmcli -s -g 802-11-wireless-security.psk connection show "$uuid" 2>/dev/null)
      k=$(nmcli -g 802-11-wireless-security.key-mgmt connection show "$uuid" 2>/dev/null)
      [ -n "$s" ] && [ -z "$SOURCE" ] && { SSID=$s; PSK=$p; KEY_MGMT=$k; SOURCE="NetworkManager \"$name\""; }
    fi
  done < <(nmcli -t -f UUID,TYPE connection show 2>/dev/null)
else
  say "  nmcli not present"
fi

hdr "iwd / wpa_supplicant access points"
ls /var/lib/iwd/*.ap 2>/dev/null | sed 's/^/  /' || true
pgrep -a wpa_supplicant 2>/dev/null | sed 's/^/  /' || true
pgrep -a iwd 2>/dev/null | sed 's/^/  /' || true

hdr "DHCP on the link"
pgrep -a dnsmasq 2>/dev/null | sed 's/^/  /' || say "  no dnsmasq"
[ -n "$IFACE" ] && ip -4 addr show "$IFACE" 2>/dev/null | sed 's/^/  /'

hdr "services and files that mention the adapter link (names only)"
systemctl list-units --all --no-legend 2>/dev/null | grep -iE 'hostap|dongle|adapter|vrlink|link|wifi|wlan' | sed 's/^/  /' | head -20
grep -rIl -iE 'hostapd|dongle|wireless.?adapter|6ghz' /etc /run /usr/lib/systemd/system 2>/dev/null | head -20 | sed 's/^/  /'

hdr "result"
if [ -n "$SSID" ]; then
  say "  link found via ${SOURCE:-iw}: ssid \"$SSID\" ${FREQ:+on $FREQ MHz}${KEY_MGMT:+, $KEY_MGMT}"
  [ -n "$PSK" ] || say "  passphrase not found: look at the hostapd/NetworkManager sections above"
else
  say "  no dedicated link is up. It may only come up while a host asks for it: check the report above, and"
  say "  run this again while the headset is connecting to a computer (Steam menu > a computer's name)."
fi

case "$KEY_MGMT" in *SAE*|sae) KM=SAE ;; *PSK*|wpa-psk) KM=WPA-PSK ;; none|NONE) KM=NONE ;; *) KM=SAE ;; esac
printf '\n---- link.conf ----\n'
printf 'MODE=station\nCOUNTRY=%s\nSSID=%s\nPSK=%s\nKEY_MGMT=%s\nFREQ=%s\nHOST_ADDR=auto\nPORTS=9757\n' \
  "${COUNTRY:-US}" "$SSID" "$PSK" "$KM" "$FREQ"
printf -- '---- end ----\n'

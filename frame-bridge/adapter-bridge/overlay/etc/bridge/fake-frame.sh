#!/bin/sh
# Self-test (link.conf SELFTEST=hwsim): plays the Steam Frame's dedicated link in its own network namespace.
# init moves the second simulated radio (mac80211_hwsim phy1) in here. This side hosts a 6 GHz WPA3-SAE network
# with link.conf's SSID/PSK and a DHCP server, waits for the bridge to join, then sends TCP and UDP to the bridge's
# address on the stream port. The bridge's NAT must take them to the "Mac" (QEMU host), whose test server answers.
say() { echo "SELFTEST: $*" > /dev/console; }
conf_get() { v=$(grep -m1 "^$1=" /run/link.conf | cut -d= -f2-); [ -n "$v" ] && echo "$v" || echo "$2"; }
SSID=$(conf_get SSID test-frame); PSK=$(conf_get PSK selftest-pass); COUNTRY=$(conf_get COUNTRY US)
# test-only overrides: make the fake Frame differ from what the bridge expects (exercises the diagnostics)
SSID=$(conf_get SELFTEST_SSID "$SSID"); PSK=$(conf_get SELFTEST_PSK "$PSK")
# A Linux radio may not start a 6 GHz network where the regulatory database marks 6 GHz NO-IR / PASSIVE-SCAN
# (the US and most countries: only the headset's own link may beacon there). The self-test plays the Frame on 5 GHz
# channel 36 by default; the bridge's join (WPA3-SAE with H2E), DHCP and NAT are the same. SELFTEST_BAND=6 tries 6 GHz.
BAND=$(conf_get SELFTEST_BAND 5)
PORT=$(conf_get PORTS 9757); PORT=${PORT%% *}
if [ "$BAND" = 6 ]; then CHANNEL=$(conf_get SELFTEST_CHANNEL 37); FREQ=$((5950 + 5 * CHANNEL)); OPC="op_class=131"
else CHANNEL=$(conf_get SELFTEST_CHANNEL 36); FREQ=$((5000 + 5 * CHANNEL)); OPC="ieee80211n=1"; fi

ip link set lo up
i=0
until ip link show wlan1 >/dev/null 2>&1; do sleep 1; i=$((i + 1)); [ $i -gt 30 ] && { say "FAIL no wlan1 in the frame namespace"; exit 1; }; done
ip link set wlan1 up
cat > /run/fake-frame-hostapd.conf <<EOF
interface=wlan1
driver=nl80211
ctrl_interface=/run/fake-frame-hostapd
country_code=$COUNTRY
ieee80211d=1
hw_mode=a
$OPC
channel=$CHANNEL
ieee80211ax=1
ssid=$SSID
wpa=2
wpa_key_mgmt=SAE
rsn_pairwise=CCMP
ieee80211w=2
sae_pwe=1
sae_password=$PSK
EOF
hostapd /run/fake-frame-hostapd.conf > /run/fake-frame-hostapd.log 2>&1 &
ip addr add 10.42.0.1/24 dev wlan1
dnsmasq --keep-in-foreground --log-facility=- --user=root --no-resolv --no-hosts --port=0 --interface=wlan1 --bind-interfaces \
  --dhcp-range=10.42.0.100,10.42.0.110,255.255.255.0,12h --dhcp-leasefile=/run/fake-frame.leases \
  --pid-file=/run/fake-frame-dnsmasq.pid > /run/fake-frame-dhcp.log 2>&1 &
sleep 1
kill -0 $! 2>/dev/null || { say "FAIL fake frame DHCP server:"; cat /run/fake-frame-dhcp.log > /dev/console; }
say "fake frame up: \"$SSID\" on $BAND GHz channel $CHANNEL ($FREQ MHz), WPA3-SAE (H2E)"

# wait for the bridge's address (the one READY reports) and test the path to the Mac
i=0
until [ -s /run/addr.wlan0 ]; do sleep 1; i=$((i + 1)); [ $i -gt 120 ] && { say "FAIL the bridge never got an address"; exit 1; }; done
B=$(cat /run/addr.wlan0)
sleep 2
r=$(printf 'GET /selftest HTTP/1.0\r\n\r\n' | timeout 10 nc "$B" "$PORT" | tail -1)
case "$r" in *mac-says-hello*) say "PASS tcp frame -> $B:$PORT -> mac: \"$r\"" ;; *) say "FAIL tcp: got \"$r\"" ;; esac
r=$(echo frame-udp-ping | timeout 5 nc -u -w 3 "$B" "$PORT" | head -1)
case "$r" in *frame-udp-ping*) say "PASS udp frame -> $B:$PORT -> mac -> back: \"$r\"" ;; *) say "FAIL udp: got \"$r\"" ;; esac
say "done"

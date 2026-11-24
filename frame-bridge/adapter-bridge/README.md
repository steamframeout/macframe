# Steam Frame Wireless Adapter on an Apple Silicon Mac

macOS has no driver for the Steam Frame Wireless Adapter: a Realtek RTL8852CU Wi-Fi 6E chip (USB `28de:2432`)
driven on Linux by the in-kernel `rtw89_8852cu` module. Apple doesn't let third parties load Wi-Fi drivers, so this
project runs the Linux driver instead, inside a tiny ARM64 Linux VM that takes the adapter over USB passthrough:

```
Steam Frame ──6 GHz──▶ adapter ──USB passthrough──▶ bridge VM (QEMU + HVF, 384 MB)
                                                       wlan0: rtw89_8852cu, joins the Frame's link (WPA3-SAE)
                                                       NAT: TCP+UDP 9757 ──▶ eth0 ──▶ Mac: steamac-vrhost
```

The adapter does on the Mac what it does on a Linux PC. The headset runs its dedicated 6 GHz network, and the
adapter joins it as a client. That is also the only direction the regulatory rules allow in most countries: a
Linux radio may not start a 6 GHz network where 6 GHz is `NO-IR`/`PASSIVE-SCAN` (US, EU, ...), but it may join one
it hears. The stream that crosses the link is WiVRn's protocol, served by `steamac-vrhost` on the Mac (the
StonkGuy/steamac fork). The headset runs the WiVRn client.

## What is verified, and what is not

| part | status |
|---|---|
| kernel 7.3.5 (Alpine linux-lts) + our `rtw89_8852cu` with Valve's ID, symbol versions checked (243 imports, 0 mismatches) | built, boots, driver registers |
| WPA3-SAE (H2E) join, DHCP, NAT of TCP and UDP to the Mac and back | **passes** the built-in self-test (simulated radios, fake Frame in its own network namespace) |
| UDP stream return path through QEMU user networking | passes. The headset sends first and the server replies to that address, which is what WiVRn does |
| Real adapter under QEMU USB passthrough on macOS | **untested**: needs the hardware |
| HVF acceleration on the Mac | untested here (built and tested under emulated ARM64) |
| The Frame's link settings (SSID, passphrase, channel) | read from the headset by `frame-probe`. Untested against a real Frame |

## Setup

1. **Install QEMU:** `brew install qemu`
2. **Check the Mac side, no adapter needed:**
   `mac/frame-adapter-bridge --selftest`. Expect two green `SELFTEST: PASS` lines (TCP and UDP) within about a minute.
3. **Read the Frame's link settings.** On the headset: Settings > System > Enable Developer Mode (set a password).
   Then on the Mac: `mac/frame-probe frame.local` (or the headset's IP address). It copies a read-only script to the
   headset, runs it with sudo, and writes `~/Library/Application Support/FrameBridge/link.conf`. If it reports that
   no link is up, run it again while the headset is connecting to a computer (Steam menu in the VR dashboard, then a computer's name), and share the report (it contains no
   passphrase).
4. **Plug the adapter into the Mac** (USB 3 port or hub) and run `mac/frame-adapter-bridge`.
   `--scan` lists the 6 GHz networks the adapter hears, if you need to find the link.
   When it prints **READY connect the Frame's WiVRn client to 10.x.x.x port 9757**, the link is up.
5. **Start the VM with VR on** (MacFrame fork: Settings > VR). In the WiVRn client on the headset, add the
   server by the address from step 4. Bonjour discovery doesn't cross the bridge, so it won't appear in the list by
   itself.

## Options and troubleshooting

* `--vmnet` uses vmnet-shared instead of QEMU user networking (needs sudo). It has less per-packet overhead; try it if the
  stream stutters at high bitrates.
* **USB access denied** (`LIBUSB_ERROR_ACCESS`): run the launcher with `sudo`, or unplug and replug the adapter.
* **"no usable 6 GHz channel"**: set `COUNTRY=` in link.conf to where you are; it must be a country that allows 6 GHz.
* **Joins but no address**: the link may use fixed addresses; set `STATIC_ADDR=` (the probe report shows the
  headset's address on the link).
* The console is a root shell on the bridge (`DEBUG_SHELL=1`): `iw dev wlan0 link`, `cat /run/wpa.log`, `dmesg`.
  Ctrl-A X stops the VM.

## If the Frame's link isn't a plain WPA3 network

Steam on Linux drives the adapter through NetworkManager (per the NixOS wiki), so it should be standard Wi-Fi. If
the probe shows otherwise (per-session keys, or no access point outside a Steam session), two fallbacks:

* **Host the link ourselves from the headset.** With developer mode you have root on the Frame and can run hostapd
  on its dedicated radio with settings you choose. The headset is allowed to beacon on 6 GHz, and the bridge joins
  as today. Not written yet, because it depends on what the probe finds.
* **USB-C cable.** Valve's own Linux troubleshooting script suggests USB tethering in developer mode when the
  adapter can't be used.

## Building the image

`./build-image.sh [WORK]` on a Linux box (x86-64 with an aarch64 cross compiler, or arm64), producing
`WORK/out/bridge-kernel` and `bridge-initramfs.img`:

* Alpine edge aarch64 packages: kernel, firmware, wpa_supplicant, hostapd, nftables, iw, dnsmasq, busybox. They are fetched
  and checked against APKINDEX by `tools/fetch_alpine.py`, without running any Alpine code.
* Alpine's kernel leaves `CONFIG_RTW89_8852CU` off, so the rtw89 stack is rebuilt from kernel.org 7.3.5 (checksum
  checked) with Alpine's config and `Module.symvers`. The build checks vermagic, the adapter's USB alias, and every
  imported symbol CRC.
* `tools/mkinitramfs.py` assembles the initramfs: programs and their libraries, the module closure with its load
  order, modules.dep/alias for on-demand loading, firmware, and the regulatory database.

Licences: kernel and modules GPL-2.0; Alpine packages under their own licences; the scripts here are
Apache-2.0.

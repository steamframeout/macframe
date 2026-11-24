# MacFrame

**Play your Steam library on an Apple silicon Mac, and stream SteamVR games to Steam Frame the way a gaming PC does.**

MacFrame runs Valve's SteamOS in a virtual machine on your Mac. Steam inside it streams VR to a Steam Frame headset
through the **Steam Frame Wireless Adapter** plugged into the Mac (handed to SteamOS, so Steam drives it exactly as on
a PC) or over Wi-Fi. SteamVR's video is encoded by the Mac's own media engine (VideoToolbox).

## Get it

1. Download **MacFrame.zip** from [Releases](https://github.com/steamframeout/macframe/releases/latest).
2. Unzip it and drag **MacFrame** into your Applications folder.
3. The first time, right-click MacFrame and choose **Open** (it is not notarized by Apple).
4. The setup assistant installs SteamOS (a 4.5 GB download from Valve), helps you connect Steam Frame, and starts
   SteamOS. Sign in to Steam, then pair the headset in Steam's Steam Frame settings, as on a PC.

Needs an Apple silicon Mac (M1 or later) with macOS 15 or later; 16 GB of memory or more is recommended.

## Status

Early. The pieces are built and tested on their own (the adapter hand-off over USB/IP, the Vulkan Video encoder on
VideoToolbox); a full VR session on real hardware is still being tested. See [docs/frame-vr.md](docs/frame-vr.md).

## Credits

MacFrame is built on [steamac](https://github.com/fxgl/steamac) and its fork's work (see
[docs/upstream-README.md](docs/upstream-README.md)), libkrun, Mesa, MoltenVK/KosmicKrisp, FEX and DXMT. Not
affiliated with Valve. SteamOS and Steam are Valve's software, downloaded from Valve under Valve's licenses.
Licenses: [LICENSE](LICENSE), [NOTICE](NOTICE), [LICENSES/](LICENSES).

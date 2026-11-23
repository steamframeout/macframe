# Steam Frame on a Mac: how the pieces fit

Goal: a Steam Frame streams PC VR from an Apple silicon Mac exactly as it does from a Windows PC: the
headset's own Steam Link VR streaming, over the Steam Frame Wireless Adapter or Wi-Fi, with SteamVR and the
games running in the SteamOS VM.

```
Steam Frame ──6 GHz link──> Wireless Adapter (USB, RTL8852CU)
                                  │
                           Mac: Frame Bridge (QEMU, Linux rtw89 driver)  ── today
                                  │                                          later: USB passthrough into the VM
SteamOS VM (libkrun) ── Steam + SteamVR (x86_64 under FEX) ── vrlink (Valve's streaming driver)
        │                                                         │ Vulkan Video encode
        │                                            VK_LAYER_STEAMAC_video_encode (guest/venc)
        │                                                         │ SVR_VENC over wmtr (virtio-gpu)
        └── Venus (Vulkan) ── virglrenderer ── Metal         Mac: wmtr_venc.m → VideoToolbox
```

## Status

| Piece | State |
|---|---|
| App (launcher + bridge in one bundle) | builds on GitHub Actions (`.github/workflows/build.yml`); release assets |
| Wireless adapter on the Mac (Frame Bridge) | works with real hardware: joins the headset's hidden 6 GHz link at 160 MHz |
| Vulkan Video encode on Venus (guest/venc) | H.264 and H.265 pass `make check` (FFmpeg's Vulkan encoders, 11 cases) in-process and through the Mac protocol (stand-in host) |
| VideoToolbox executor (wmtr/src/wmtr_venc.m) | written, not compiled yet (needs the DXMT/wmtr host build on a Mac) |
| VR eyes from games (Vulkan path, D3D11/12 via DXVK/vkd3d) | written and cross-compiled; runtime untested |
| SteamVR + vrlink in the VM | not tried yet: vrlink ships for x86_64 Linux only (FEX) |
| Steam driving the adapter (NetworkManager, pairing) | not yet: needs the adapter inside the VM (USB passthrough for libkrun) |
| Headset discovers the VM | the launcher's LAN Remote Play relay (Settings > Advanced) |

## Encode layer notes (guest/venc)

- Implicit layer, active only in SteamVR's processes (vrserver, vrcompositor, vrstartup) or with
  `STEAMAC_VENC=1`; every other process sees the driver unchanged, and devices without video stay on the
  driver's own function pointers.
- Adds a video-encode queue family and VK_KHR_video_queue / video_encode_queue / encode_h264 / encode_h265 /
  video_maintenance1. Source pictures are copied on a real queue after the batch's waits into host memory
  shared with the Mac (exported Venus memory = POSIX shm); the bitstream is written in place by VideoToolbox.
- Applications write their own parameter sets: the layer writes the application's VPS/SPS/PPS, reports the
  encoder's coding tools as overrides, and rewrites every slice header for the application's parameter sets
  (frame_num/POC, reference picture sets, ids, QP, deblocking, SAO, temporal MVP).
- `make check` (guest/venc/Makefile): FFmpeg ≥ 7.1 with Vulkan, lavapipe, x264/x265. FFmpeg 7.1 has two bugs
  that show up here and are not the layer's: an uninitialized return in `write_extra_headers` (patched in the
  test FFmpeg) and a race in its host-memory-import upload (VK_EXT_external_memory_host is hidden in tests).

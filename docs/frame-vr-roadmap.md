# What's left, and how

Status at 2026-11-22. Built and tested pieces are in `adapter-bridge/`, `macapp/` (Frame Bridge.app), `guest/` and
`steamac-patches/` (the Vulkan VR series against StonkGuy/steamac, docs/vr-vulkan.md inside it).

## 1. Steam Frame client (the player app on the headset)

**What exists.** WiVRn's development branch supports the Frame. The Linux client has a V4L2 hardware video decoder,
detects the panel resolution, plays audio through PipeWire, and has a Steam Frame controller profile. WiVRn's CI
builds it in Valve's Steam Runtime 4 ARM64 SDK (`build-steam-frame` job, artifact `wivrn-frame-client.zip`). It
is not in a release yet: the latest is v26.9.

**The catch.** WiVRn refuses a peer with a different protocol hash, and the development branch changed the protocol
since v26.9:
- reordered and new controller input IDs (Frame bumpers, d-pad, per-hand system and menu)
- the FoV was removed from the headset-info packet
- battery state, hand-tracking data source and eye-tracked foveation were added

`steamac-vrhost` vendors WiVRn v26.9's protocol (`host/vrhost/third_party/wivrn`), so today's Frame client and
vrhost cannot talk.

**Plan: move vrhost to WiVRn's next release, not backport the Frame to v26.9.**
1. When WiVRn tags the release with Frame support, re-vendor `common/` and `server/compositor/pacer` at that tag
   (`host/vrhost/third_party/wivrn/UPSTREAM.md`). Re-apply the marked `steamac` hardening in
   `wivrn_serialization.h` and update the pinned protocol hash in `tests/test_protocol.cpp`.
2. `src/wivrn/input_map.cpp`: rename the IDs (`X_CLICK`→`LEFT_X_CLICK`, `MENU_CLICK`→`LEFT_MENU_CLICK`,
   `SYSTEM_CLICK`→`RIGHT_SYSTEM_CLICK`, ...). Map the new bumper and d-pad IDs to the guest's input IDs (add
   `SVR_IN_*` entries and the OpenVR bindings for the Frame controllers).
3. `src/wivrn/wivrn_session.cpp` `on_headset_info`: the FoV is no longer in `headset_info_packet`. Take it from the
   first tracking packet's views, and hold the guest's SVR_HELLO view until that arrives (the runtime already
   handles a hello without a view, `hello_view_`).
4. On the headset: install WiVRn's Frame build from that release, add it to Steam, and launch it from the Steam
   library (it needs the Steam Runtime's `libopenxr_loader.so`). With the wireless adapter, connect to the bridge's
   address (Frame Bridge shows it); on home Wi-Fi, connect to the Mac.

Backporting the Frame client onto v26.9 instead would mean maintaining a WiVRn fork, and the Frame's extra buttons
can't be expressed in the v26.9 protocol.

## 2. OpenXR games

Unity's and Unreal's current VR plugins use OpenXR. Under Proton, `wineopenxr` loads the Linux OpenXR loader and
turns `XR_KHR_D3D11_enable`/`XR_KHR_D3D12_enable` into `XR_KHR_vulkan_enable` on the Linux runtime. So one Linux
OpenXR runtime with Vulkan swapchains covers D3D11, D3D12 and Vulkan OpenXR games, Proton and native alike. The guest
has none today.

**Plan: Monado in the guest, with a steamac driver and compositor target.** Monado (BSL-1.0) already implements the
OpenXR API surface: instance, session, frame loop, swapchains, spaces, the action system and interaction profiles.
Most of an OpenXR runtime's correctness is in those parts, so writing them again would be the expensive way. What
steamac adds:
- **`drivers/steamac`**: `xrt_device`s for the HMD and two controllers. Poses, input and haptics come over the same
  guest VR protocol as the OpenVR runtime (SVR_FRAME, SVR_HAPTIC; `guest/vr/runtime/transport.*`).
- **A compositor target**: Monado's compositor squashes the app's layers on its own Venus device, with distortion off.
  The target hands each frame's two eye images to `VkExporter` (`guest/vr/runtime/vk_export.*`, the path built for
  OpenVR Vulkan games), so the host side (`SVR_BUFFER`, `SVR_SUBMIT_BUF`, the wmtr executor's blit) is reused as is.
  Frame timing comes from SVR_FRAME's pacer (Monado's `u_pacing` app pacer, fed the host's predicted display time).
- **Packaging**: in-process Monado (no service), built for x86-64 (Proton under FEX) and ARM64 (native). It is
  installed as the guest's active runtime (`/usr/share/openxr/1/active_runtime.json`) next to the OpenVR runtime. The
  launcher's VR toggle sets both.

Rough size: driver ~600 lines, target ~800 lines, build glue. WiVRn's server is a Monado fork with exactly this
shape (its own driver plus a stream target) and is a useful reference.

## 3. Everything else

- **Run it on a Mac.** Build the fork with `steamac-patches/`, then test: libkrun patch 0031's unit test, the
  `wmtr_vr.m` buffer path, a D3D11 game on the default Vulkan path, and Alyx's ARM64 build (`bin/linuxarm64`).
- **Frame Bridge on real hardware.** QEMU USB passthrough of the adapter, joining the Frame's link, and the probe on
  a real headset.
- **Anti-cheat.** Easy Anti-Cheat works in the fork (VRChat); other systems are untested.

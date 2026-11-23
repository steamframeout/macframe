# VR eyes from Vulkan games

The guest runtime used to accept only DXMT textures (`TextureType_Metal`). That covers D3D11 games on the DXMT graphics
path. steamac's default graphics path is Vulkan (DXVK and vkd3d-proton on Venus), and there Proton's vrclient hands
every D3D11 and D3D12 eye image to the Linux runtime as a Vulkan image. Windows Vulkan games and native Linux ports
(Half-Life: Alyx's ARM64 build for the Steam Frame) submit Vulkan images directly. This adds a path for all of them.

```
game (Venus device)                     guest runtime (vrclient.so)                      host (steamac-vm)
eye VkImage, TRANSFER_SRC_OPTIMAL --->  vk_export.cpp: one vkCmdCopyImageToBuffer   ---> wmtr executor: wmtr_vr.m
                                        per eye, on the game's queue, into a ring       blit buffer -> IOSurface
                                        of 3 dma-buf buffers per eye (exportable        swapchain image, then
                                        memory on the game's own device); a worker      VRL_SUBMIT to vrhost
                                        thread waits the fences, then SVR_SUBMIT_BUF
```

* **Memory, zero copy across the VM boundary.** On macOS, virglrenderer's Venus backs host-visible exportable memory
  with POSIX shm (libkrun patch 0007). The runtime imports each buffer's dma-buf into its own wmtr context
  (`wmtr_transport.import_fd`: `PRIME_FD_TO_HANDLE`, then `RESOURCE_INFO` for the resource id). The kernel attaches
  the resource to that context. The context's attach hook (libkrun patch 0031) hands a duplicate of the resource's shm
  fd to the executor (`wmtr_resource_attach`), which maps it (`wmtr_vr_shm.c`) and wraps it in an MTLBuffer without a
  copy. Per frame, the GPU work is the guest's copy into the buffer and the host's blit into the swapchain.
* **A wmtr context of the runtime's own** (`guest/vr/runtime/wmtr_native.c`, on `wmtr/src/transport_virtgpu.c`). The
  host answers `SVR_SUBMIT_BUF` only after its blit completed, so the guest may reuse the buffer at the ack. That
  wait must not happen on a DXMT context, whose executor also runs the game's rendering. So buffer traffic always
  goes over the runtime's own context, also inside Proton. A native process (no Wine) uses that context for
  everything (`Transport::open_native`).
* **The game's thread never waits for the GPU.** Submit records and submits the copy and returns. A full ring (host
  or GPU behind) drops the frame instead of stalling.
* **Extensions.** `GetVulkanDeviceExtensionsRequired` returns `VK_KHR_external_memory_fd`,
  `VK_EXT_external_memory_dma_buf` and their dependencies, limited to what the device offers. DXVK and vkd3d-proton
  ask through Proton's vrclient and enable them.
* **Formats.** RGBA8, BGRA8 (UNORM and sRGB) and RGBA16F. Multisampled eyes must be resolved (as for SteamVR).
* **Builds.** `make` builds `bin/linux64/vrclient.so` (Proton, x86-64 under FEX). `make RUNTIME_ARCH=linuxarm64
  CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc` builds the one native ARM64 games load
  (`bin/linuxarm64/vrclient.so`).

## Status

| part | state |
|---|---|
| `vk_export.cpp` (copy ring, fences, worker, drops, resize) | tested end to end on lavapipe: a forked fake host checks every texel of every frame. Clean under ASan, UBSan and TSan |
| `wmtr_vr_shm.c` (host memory registry, eye checks) | `tests/wmtr_vr_shm_test.c` (runs on Linux and macOS) |
| libkrun patch 0031 | type-checked for `aarch64-apple-darwin` with its new unit test; the test is not run yet |
| runtime transport and submit integration, `wmtr_native.c`, `transport_virtgpu.c` import | builds for x86-64 and ARM64; record layout checked against `wmtr_host.m`'s parser |
| `wmtr_vr.m` buffer blit | written against the DXMT path's code; **not compiled** (needs the macOS SDK) |
| on a Mac with a headset | **not run** |

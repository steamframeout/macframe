// SPDX-License-Identifier: Apache-2.0
// Vulkan eye export for native (ARM64 Linux) OpenVR games: TextureType_Vulkan submits.
//
// The game's eye image lives on its own VkDevice (in the steamac guest: Venus, rendered on the Mac's GPU). Submit
// records one GPU copy per eye, on the game's queue, into a linear buffer of a small per-eye ring. The buffers are
// allocated on the game's device as exportable dma-buf memory and registered once with the host (BufferSink); on the
// steamac host that memory is shared memory the VMM already maps, so the host reads the copy in place. A worker
// thread waits for each frame's copies and sends the frame; the game's render thread never waits for the GPU.
//
// Threading: copy_eye / finish_frame / abandon_frame / required_device_extensions are called from the game's
// Submit thread (OpenVR: the queue is the caller's for the duration of the call). The worker only waits fences and
// talks to the sink.
#pragma once
#include <vulkan/vulkan.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "openvr.h"
#include "steamac_vr_guest.h"
#include "transport.h"

namespace svr {

// Frame data that is not pixels (filled by the runtime as for the DXMT path).
struct BufFrameMeta {
  uint64_t frame_id = 0;
  uint64_t display_time = 0;
  float pose[2][7] = {};
  float fov[2][4] = {};
};

struct VkExportStats {
  uint64_t eyes_copied = 0, frames_sent = 0, frames_dropped = 0, frames_incomplete = 0, errors = 0;
  uint64_t buffers_allocated = 0;
  uint64_t copy_wait_ns = 0; // sum over sent frames: Submit of the first eye -> both copies done
  uint64_t host_ns = 0;      // sum over sent frames: sink submit -> ack
};

class VkExporter {
 public:
  static constexpr int kRing = 3;  // per eye: one being copied, one at the host, one spare

  explicit VkExporter(BufferSink *sink);
  ~VkExporter();
  VkExporter(const VkExporter &) = delete;
  VkExporter &operator=(const VkExporter &) = delete;

  // OpenVR IVRCompositor::GetVulkan*ExtensionsRequired: the extensions copy_eye needs, space separated, limited
  // to what the physical device offers (a game enables what it gets; an unsupported name would fail its
  // vkCreateDevice).
  static std::string required_instance_extensions();
  static std::string required_device_extensions(VkPhysicalDevice pd);

  // One eye of the frame being built. Rect in texels of `slice`; w == 0 means the whole image. eye_flags: SVR_EYE_*.
  // Returns 0 when the copy is queued, 1 when the eye was dropped (ring full: the host or the GPU is behind),
  // a negative errno on error (logged).
  int copy_eye(int eye, const vr::VRVulkanTextureData_t &tex, uint32_t slice, uint32_t x, uint32_t y, uint32_t w,
               uint32_t h, uint32_t eye_flags);
  // Both eyes copied: send the frame once the copies complete. A frame missing an eye is not sent (its buffers
  // come back once the GPU is done with them). Never blocks on the GPU.
  void finish_frame(const BufFrameMeta &meta);
  // Forget the eyes copied so far (ClearLastSubmittedFrame).
  void abandon_frame();
  // Wait until every queued frame was sent or dropped (shutdown, tests).
  void drain();

  VkExportStats stats() const;

 private:
  enum class SlotState { Free, Copying, InFlight };
  struct Slot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint64_t size = 0;
    uint32_t id = 0;  // registered buffer id, 0 = none
    SlotState state = SlotState::Free;
    svr_buf_eye eye{};
  };
  struct Job {
    int slot[2] = {-1, -1};
    bool complete = false;
    BufFrameMeta meta;
    int64_t first_submit_ns = 0;
  };
  struct Device {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t family = ~0u;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    // dma-buf first; an opaque fd once a dma-buf allocation failed (SVR_VK_EXPORT_HANDLE=opaque forces it)
    VkExternalMemoryHandleTypeFlagBits handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
#define SVR_VK_DEV_FNS(X)                                                                                       \
  X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)    \
  X(vkBindBufferMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)             \
  X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)                \
  X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)             \
  X(vkResetFences) X(vkGetMemoryFdKHR) X(vkDeviceWaitIdle)
#define SVR_VK_DECL(f) PFN_##f f = nullptr;
    SVR_VK_DEV_FNS(SVR_VK_DECL)
#undef SVR_VK_DECL
    Slot slots[2][kRing];
  };

  bool ensure_device(const vr::VRVulkanTextureData_t &tex);
  void destroy_device_locked();
  bool ensure_slot(int eye, int i, uint64_t size);
  bool alloc_slot(Slot &s, uint64_t size, VkExternalMemoryHandleTypeFlagBits handle, uint32_t *type_out);
  void free_slot(Slot &s);
  int find_free_slot_locked(int eye);
  void worker_main();
  void release_slots_locked(const Job &j);

  BufferSink *sink_;
  mutable std::mutex m_;
  std::condition_variable cv_;       // job queued, or quit
  std::condition_variable idle_cv_;  // a job finished
  std::deque<Job> jobs_;
  bool quit_ = false;
  int busy_jobs_ = 0;
  std::thread worker_;
  Device d_;
  Job building_;  // eyes copied for the frame being built
  uint32_t next_id_ = 1;
  VkExportStats stats_;
  int warned_ = 0;
};

// VkFormat -> SVR_VK_FORMAT_* (0: unsupported)
uint32_t svr_format_from_vk(uint32_t vk_format);

}  // namespace svr

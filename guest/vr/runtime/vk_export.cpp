// SPDX-License-Identifier: Apache-2.0
// Vulkan eye export: see vk_export.h.
#include "vk_export.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include "log.h"

namespace svr {
namespace {

int64_t mono_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

// The loader the game already loaded (libvulkan.so.1); its exported trampolines take the game's handles.
void *loader() {
  static void *h = [] {
    void *l = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!l) l = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!l) SVR_ERR("vk-export: cannot load libvulkan.so.1: %s", dlerror());
    return l;
  }();
  return h;
}

template <class F>
F loader_fn(const char *name) {
  void *l = loader();
  return l ? (F)dlsym(l, name) : nullptr;
}

const char *const kDeviceExts[] = {
    "VK_KHR_external_memory",          // core in 1.1, listed for 1.0 devices
    "VK_KHR_external_memory_fd",       // vkGetMemoryFdKHR
    "VK_EXT_external_memory_dma_buf",  // dma-buf handles: what Venus exports as a virtio-gpu resource
    "VK_KHR_dedicated_allocation",     // core in 1.1
    "VK_KHR_get_memory_requirements2", // dependency of dedicated_allocation on 1.0
};
const char *const kInstanceExts[] = {
    "VK_KHR_external_memory_capabilities",
    "VK_KHR_get_physical_device_properties2",
};

}  // namespace

uint32_t svr_format_from_vk(uint32_t f) {
  switch (f) {
  case VK_FORMAT_R8G8B8A8_UNORM: return SVR_VK_FORMAT_R8G8B8A8_UNORM;
  case VK_FORMAT_R8G8B8A8_SRGB: return SVR_VK_FORMAT_R8G8B8A8_SRGB;
  case VK_FORMAT_B8G8R8A8_UNORM: return SVR_VK_FORMAT_B8G8R8A8_UNORM;
  case VK_FORMAT_B8G8R8A8_SRGB: return SVR_VK_FORMAT_B8G8R8A8_SRGB;
  case VK_FORMAT_R16G16B16A16_SFLOAT: return SVR_VK_FORMAT_R16G16B16A16_SFLOAT;
  default: return 0;
  }
}

std::string VkExporter::required_instance_extensions() {
  std::string s;
  for (const char *e : kInstanceExts) s += (s.empty() ? "" : " "), s += e;
  return s;
}

std::string VkExporter::required_device_extensions(VkPhysicalDevice pd) {
  auto enumerate = loader_fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
  std::vector<VkExtensionProperties> have;
  if (pd && enumerate) {
    uint32_t n = 0;
    if (enumerate(pd, nullptr, &n, nullptr) == VK_SUCCESS && n) {
      have.resize(n);
      if (enumerate(pd, nullptr, &n, have.data()) != VK_SUCCESS) have.clear();
      have.resize(n);
    }
  }
  std::string s;
  for (const char *e : kDeviceExts) {
    bool ok = have.empty();  // nothing to check against: ask for all of them
    for (auto &p : have) ok = ok || !strcmp(p.extensionName, e);
    if (ok) s += (s.empty() ? "" : " "), s += e;
  }
  return s;
}

VkExporter::VkExporter(BufferSink *sink) : sink_(sink) { worker_ = std::thread([this] { worker_main(); }); }

VkExporter::~VkExporter() {
  drain();
  {
    std::lock_guard<std::mutex> g(m_);
    quit_ = true;
  }
  cv_.notify_all();
  worker_.join();
  std::lock_guard<std::mutex> g(m_);
  destroy_device_locked();
}

VkExportStats VkExporter::stats() const {
  std::lock_guard<std::mutex> g(m_);
  return stats_;
}

void VkExporter::drain() {
  std::unique_lock<std::mutex> g(m_);
  idle_cv_.wait(g, [this] { return jobs_.empty() && busy_jobs_ == 0; });
}

// (re)binds to the game's device; a different device (the game recreated it) drops every slot of the old one
bool VkExporter::ensure_device(const vr::VRVulkanTextureData_t &tex) {
  VkDevice dev = (VkDevice)tex.m_pDevice;
  if (d_.dev == dev && d_.family == tex.m_nQueueFamilyIndex && d_.pool) return true;
  if (d_.dev) {
    // queued frames still reference the old device's buffers
    std::unique_lock<std::mutex> g(m_);
    idle_cv_.wait(g, [this] { return jobs_.empty() && busy_jobs_ == 0; });
    destroy_device_locked();
  }
  d_ = Device{};
  auto gdpa = loader_fn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
  auto gmp = loader_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
  if (!gdpa || !gmp || !dev || !tex.m_pPhysicalDevice) {
    SVR_ERR("vk-export: no Vulkan loader entry points or no device in the texture description");
    return false;
  }
  d_.dev = dev;
  d_.pd = (VkPhysicalDevice)tex.m_pPhysicalDevice;
  d_.instance = (VkInstance)tex.m_pInstance;
  d_.family = tex.m_nQueueFamilyIndex;
  d_.gdpa = gdpa;
  gmp(d_.pd, &d_.mem);
  if (const char *h = getenv("SVR_VK_EXPORT_HANDLE"); h && !strcmp(h, "opaque"))
    d_.handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#define SVR_VK_LOAD(f) d_.f = (PFN_##f)gdpa(dev, #f);
  SVR_VK_DEV_FNS(SVR_VK_LOAD)
#undef SVR_VK_LOAD
  bool ok = true;
#define SVR_VK_CHECK(f) if (!d_.f) { SVR_ERR("vk-export: device has no %s (extension not enabled?)", #f); ok = false; }
  SVR_VK_DEV_FNS(SVR_VK_CHECK)
#undef SVR_VK_CHECK
  if (!ok) {
    SVR_ERR("vk-export: the game's VkDevice lacks %s; it must enable GetVulkanDeviceExtensionsRequired's list",
            "VK_KHR_external_memory_fd");
    d_ = Device{};
    return false;
  }
  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = d_.family;
  if (VkResult r = d_.vkCreateCommandPool(dev, &pci, nullptr, &d_.pool); r != VK_SUCCESS) {
    SVR_ERR("vk-export: vkCreateCommandPool: %d", (int)r);
    d_ = Device{};
    return false;
  }
  SVR_INFO("vk-export: bound to the game's device %p (queue family %u)", (void *)dev, d_.family);
  return true;
}

void VkExporter::free_slot(Slot &s) {
  if (s.id && sink_) sink_->unregister_buffer(s.id);
  if (s.fence) d_.vkDestroyFence(d_.dev, s.fence, nullptr);
  if (s.cb) d_.vkFreeCommandBuffers(d_.dev, d_.pool, 1, &s.cb);
  if (s.buffer) d_.vkDestroyBuffer(d_.dev, s.buffer, nullptr);
  if (s.memory) d_.vkFreeMemory(d_.dev, s.memory, nullptr);
  s = Slot{};
}

void VkExporter::destroy_device_locked() {
  if (!d_.dev) return;
  // every job is done (callers drain first), so no fence or buffer is in use by the worker; the GPU may still
  // be on a copy of a dropped frame
  for (auto &eye : d_.slots)
    for (auto &s : eye)
      if (s.fence && s.state != SlotState::Free) d_.vkWaitForFences(d_.dev, 1, &s.fence, VK_TRUE, 1000000000ull);
  for (auto &eye : d_.slots)
    for (auto &s : eye) free_slot(s);
  if (d_.pool) d_.vkDestroyCommandPool(d_.dev, d_.pool, nullptr);
  d_ = Device{};
}

// buffer + exportable memory (+ command buffer, fence) for one slot with export handle type `handle`
bool VkExporter::alloc_slot(Slot &s, uint64_t size, VkExternalMemoryHandleTypeFlagBits handle, uint32_t *type_out) {
  VkExternalMemoryBufferCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  ext.handleTypes = handle;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.pNext = &ext;
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult r = d_.vkCreateBuffer(d_.dev, &bci, nullptr, &s.buffer);
  if (r != VK_SUCCESS) { SVR_ERR("vk-export: vkCreateBuffer(%llu): %d", (unsigned long long)size, (int)r); s.buffer = VK_NULL_HANDLE; return false; }
  VkMemoryRequirements req;
  d_.vkGetBufferMemoryRequirements(d_.dev, s.buffer, &req);
  // host-visible and coherent first: on the steamac host that is shm the VMM maps; then anything allowed
  const VkMemoryPropertyFlags want[] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
  uint32_t type = ~0u;
  for (VkMemoryPropertyFlags w : want) {
    for (uint32_t t = 0; t < d_.mem.memoryTypeCount && type == ~0u; t++)
      if ((req.memoryTypeBits & (1u << t)) && (d_.mem.memoryTypes[t].propertyFlags & w) == w) type = t;
    if (type != ~0u) break;
  }
  VkMemoryDedicatedAllocateInfo ded{};
  ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  ded.buffer = s.buffer;
  VkExportMemoryAllocateInfo exp{};
  exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  exp.pNext = &ded;
  exp.handleTypes = handle;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &exp;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  if (type == ~0u || (r = d_.vkAllocateMemory(d_.dev, &mai, nullptr, &s.memory)) != VK_SUCCESS) {
    SVR_INFO("vk-export: %s allocation of %llu bytes failed (type %d, %d)",
             handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? "dma-buf" : "opaque-fd",
             (unsigned long long)req.size, (int)type, type == ~0u ? 0 : (int)r);
    d_.vkDestroyBuffer(d_.dev, s.buffer, nullptr);
    s.buffer = VK_NULL_HANDLE, s.memory = VK_NULL_HANDLE;
    return false;
  }
  s.size = req.size;
  *type_out = type;
  return true;
}

// a slot of at least `size` bytes, its exported memory registered with the sink
bool VkExporter::ensure_slot(int eye, int i, uint64_t size) {
  Slot &s = d_.slots[eye][i];
  if (s.buffer && s.size >= size) return true;
  // the caller claimed the slot (Copying): reallocation must not hand it back as Free (free_slot resets it)
  SlotState claimed = s.state;
  free_slot(s);
  s.state = claimed;
  uint32_t type = 0;
  if (!alloc_slot(s, size, d_.handle, &type)) {
    if (d_.handle != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT || !alloc_slot(s, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, &type)) {
      SVR_ERR("vk-export: no exportable memory of %llu bytes on this device", (unsigned long long)size);
      free_slot(s);
      return false;
    }
    d_.handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    SVR_ERR("vk-export: dma-buf export unavailable, using opaque fds (works with the socket transport only)");
  }
  VkResult r = d_.vkBindBufferMemory(d_.dev, s.buffer, s.memory, 0);
  if (r != VK_SUCCESS) {
    SVR_ERR("vk-export: vkBindBufferMemory: %d", (int)r);
    free_slot(s);
    return false;
  }
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = d_.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (d_.vkAllocateCommandBuffers(d_.dev, &cai, &s.cb) != VK_SUCCESS ||
      d_.vkCreateFence(d_.dev, &fci, nullptr, &s.fence) != VK_SUCCESS) {
    SVR_ERR("vk-export: command buffer or fence creation failed");
    free_slot(s);
    return false;
  }
  VkMemoryGetFdInfoKHR gfi{};
  gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  gfi.memory = s.memory;
  gfi.handleType = d_.handle;
  int fd = -1;
  if ((r = d_.vkGetMemoryFdKHR(d_.dev, &gfi, &fd)) != VK_SUCCESS || fd < 0) {
    SVR_ERR("vk-export: vkGetMemoryFdKHR: %d", (int)r);
    free_slot(s);
    return false;
  }
  uint32_t id = next_id_++;
  if (!next_id_) next_id_ = 1;
  bool dmabuf = d_.handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  int rr = sink_ ? sink_->register_buffer(id, fd, s.size, dmabuf) : -ENOTCONN;
  close(fd);
  if (rr) {
    SVR_ERR("vk-export: host refused buffer %u (%d)", id, rr);
    free_slot(s);
    return false;
  }
  s.id = id;
  stats_.buffers_allocated++;
  SVR_INFO("vk-export: eye %d slot %d: %llu bytes, memory type %u, %s, buffer id %u", eye, i,
           (unsigned long long)s.size, type, dmabuf ? "dma-buf" : "opaque fd", id);
  return true;
}

int VkExporter::find_free_slot_locked(int eye) {
  for (int i = 0; i < kRing; i++)
    if (d_.slots[eye][i].state == SlotState::Free) return i;
  return -1;
}

int VkExporter::copy_eye(int eye, const vr::VRVulkanTextureData_t &tex, uint32_t slice, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t eye_flags) {
  if (eye < 0 || eye > 1) return -EINVAL;
  if (!tex.m_nImage || !tex.m_pQueue || !tex.m_nWidth || !tex.m_nHeight) return -EINVAL;
  if (tex.m_nSampleCount > 1) {
    if (warned_++ < 8) SVR_ERR("vk-export: multisampled eye textures must be resolved before Submit");
    return -EINVAL;
  }
  uint32_t fmt = svr_format_from_vk(tex.m_nFormat);
  uint32_t bpp = svr_vk_format_bytes(fmt);
  if (!bpp) {
    if (warned_++ < 8) SVR_ERR("vk-export: unsupported eye format VkFormat %u", tex.m_nFormat);
    return -ENOTSUP;
  }
  if (!w) x = 0, y = 0, w = tex.m_nWidth, h = tex.m_nHeight;
  if (!h || (uint64_t)x + w > tex.m_nWidth || (uint64_t)y + h > tex.m_nHeight) return -EINVAL;
  if (!ensure_device(tex)) return -ENODEV;

  int i;
  {
    std::lock_guard<std::mutex> g(m_);
    if (building_.slot[eye] >= 0) {
      // the same eye twice in one frame: the earlier copy is superseded; its slot comes back via a job
      Job stale;
      stale.slot[eye] = building_.slot[eye];
      building_.slot[eye] = -1;
      jobs_.push_back(stale);
      cv_.notify_one();
    }
    i = find_free_slot_locked(eye);
    if (i < 0) return 1;
    d_.slots[eye][i].state = SlotState::Copying;  // ours (game thread) until queued
  }
  uint64_t size = (uint64_t)w * h * bpp;
  if (!ensure_slot(eye, i, size)) {
    std::lock_guard<std::mutex> g(m_);
    d_.slots[eye][i].state = SlotState::Free;
    stats_.errors++;
    return -ENOMEM;
  }
  Slot &s = d_.slots[eye][i];
  VkImage image = (VkImage)(uintptr_t)tex.m_nImage;
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d_.vkBeginCommandBuffer(s.cb, &bi);
  // OpenVR: a submitted Vulkan image is in TRANSFER_SRC_OPTIMAL. Make the game's writes visible to the copy
  // without changing the layout (the game expects it unchanged after Submit).
  VkImageMemoryBarrier ib{};
  ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ib.image = image;
  ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slice, 1};
  d_.vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &ib);
  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;  // tight
  region.bufferImageHeight = 0;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, slice, 1};
  region.imageOffset = {(int32_t)x, (int32_t)y, 0};
  region.imageExtent = {w, h, 1};
  d_.vkCmdCopyImageToBuffer(s.cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &region);
  // the host reads the buffer as memory once the fence signals
  VkBufferMemoryBarrier bb{};
  bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.buffer = s.buffer;
  bb.offset = 0;
  bb.size = VK_WHOLE_SIZE;
  d_.vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
                          nullptr);
  VkResult r = d_.vkEndCommandBuffer(s.cb);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &s.cb;
  if (r == VK_SUCCESS) r = d_.vkQueueSubmit((VkQueue)tex.m_pQueue, 1, &si, s.fence);
  std::lock_guard<std::mutex> g(m_);
  if (r != VK_SUCCESS) {
    SVR_ERR("vk-export: eye copy submit failed: %d", (int)r);
    s.state = SlotState::Free;
    stats_.errors++;
    return -EIO;
  }
  s.state = SlotState::Copying;  // queued on the GPU; the worker takes it from the frame's job
  s.eye = svr_buf_eye{s.id, fmt, 0, w * bpp, w, h, eye_flags};
  if (building_.slot[0] < 0 && building_.slot[1] < 0) building_.first_submit_ns = mono_ns();
  building_.slot[eye] = i;
  stats_.eyes_copied++;
  return 0;
}

void VkExporter::finish_frame(const BufFrameMeta &meta) {
  std::lock_guard<std::mutex> g(m_);
  if (building_.slot[0] < 0 && building_.slot[1] < 0) return;
  Job j = building_;
  j.complete = j.slot[0] >= 0 && j.slot[1] >= 0;
  j.meta = meta;
  if (!j.complete) stats_.frames_incomplete++;
  building_ = Job{};
  jobs_.push_back(j);
  cv_.notify_one();
}

void VkExporter::abandon_frame() {
  std::lock_guard<std::mutex> g(m_);
  if (building_.slot[0] < 0 && building_.slot[1] < 0) return;
  Job j = building_;
  j.complete = false;
  building_ = Job{};
  jobs_.push_back(j);
  cv_.notify_one();
}

void VkExporter::release_slots_locked(const Job &j) {
  for (int e = 0; e < 2; e++)
    if (j.slot[e] >= 0) d_.slots[e][j.slot[e]].state = SlotState::Free;
}

void VkExporter::worker_main() {
  pthread_setname_np(pthread_self(), "svr-vk-export");
  std::unique_lock<std::mutex> g(m_);
  for (;;) {
    cv_.wait(g, [this] { return quit_ || !jobs_.empty(); });
    if (jobs_.empty()) {
      if (quit_) return;
      continue;
    }
    Job j = jobs_.front();
    jobs_.pop_front();
    busy_jobs_++;
    VkFence fences[2];
    uint32_t nf = 0;
    for (int e = 0; e < 2; e++)
      if (j.slot[e] >= 0) {
        d_.slots[e][j.slot[e]].state = SlotState::InFlight;
        fences[nf++] = d_.slots[e][j.slot[e]].fence;
      }
    Device &d = d_;
    g.unlock();
    // the copies follow the game's whole frame on its queue: this waits for the frame's rendering
    VkResult r = nf ? d.vkWaitForFences(d.dev, nf, fences, VK_TRUE, 2000000000ull) : VK_SUCCESS;
    if (r == VK_SUCCESS && nf) d.vkResetFences(d.dev, nf, fences);
    int64_t ready = mono_ns();
    int sent = 0, err = 0;
    int64_t host_ns = 0;
    if (r == VK_SUCCESS && j.complete) {
      svr_submit_buf_req req{};
      req.h = {SVR_SUBMIT_BUF, sizeof(req)};
      req.frame_id = j.meta.frame_id;
      req.display_time = j.meta.display_time;
      for (int e = 0; e < 2; e++) req.eye[e] = d.slots[e][j.slot[e]].eye;
      memcpy(req.pose, j.meta.pose, sizeof req.pose);
      memcpy(req.fov, j.meta.fov, sizeof req.fov);
      req.guest_submit_ns = j.first_submit_ns;
      req.guest_ready_ns = ready;
      int rr = sink_ ? sink_->submit_buffers(req) : -ENOTCONN;
      host_ns = mono_ns() - ready;
      if (rr == 0) sent = 1;
      else {
        err = 1;
        if (warned_++ < 8) SVR_ERR("vk-export: frame %llu not taken by the host (%d)", (unsigned long long)req.frame_id, rr);
      }
    } else if (r != VK_SUCCESS) {
      err = 1;
      SVR_ERR("vk-export: eye copy did not complete (%d); its buffers stay busy", (int)r);
    }
    g.lock();
    if (r == VK_SUCCESS) release_slots_locked(j);  // a copy still running keeps its slot (never reused)
    if (sent) {
      stats_.frames_sent++;
      stats_.copy_wait_ns += (uint64_t)(ready - j.first_submit_ns);
      stats_.host_ns += (uint64_t)host_ns;
    } else if (j.complete && !err) {
      stats_.frames_dropped++;
    }
    if (err) stats_.errors++;
    busy_jobs_--;
    idle_cv_.notify_all();
  }
}

}  // namespace svr

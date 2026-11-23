// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>

#include "steamac_vr_guest.h"

namespace svr {
// Shared eye buffers of the Vulkan path (vk_export.cpp, SVR_BUFFER / SVR_SUBMIT_BUF in steamac_vr_guest.h)
class BufferSink {
 public:
  virtual ~BufferSink() = default;
  // fd: `size` bytes of exported memory, a dma-buf (dmabuf = true: what Venus exports; the wmtr link needs it to
  // find the virtio-gpu resource) or an opaque fd of the same driver (socket transport only). The fd stays the
  // caller's. 0 or -errno.
  virtual int register_buffer(uint32_t id, int fd, uint64_t size, bool dmabuf) = 0;
  virtual int unregister_buffer(uint32_t id) = 0;
  // returns when the host no longer reads the frame's buffers. 0 or -errno.
  virtual int submit_buffers(const svr_submit_buf_req &req) = 0;
};

class Transport {
 public:
  virtual ~Transport() = default;
  // one request, one reply; returns 0 or a negative errno. Thread-safe.
  virtual int call(const svr_hdr *req, svr_hdr *rep, uint32_t rep_cap) = 0;
  // hand one frame to the host compositor (wmtr: WMTVRSubmit, asynchronous; socket: SVR_SUBMIT). 0 or -errno.
  virtual int submit(const svr_wmtvr_submit &info) = 0;
  virtual const char *name() const = 0;
  // where Vulkan eye buffers go; nullptr if this link has none (the runtime then refuses Vulkan textures)
  virtual BufferSink *buffer_sink() { return nullptr; }

  // STEAMAC_VR_SOCKET=<path> selects the socket transport, otherwise wmtr; nullptr if neither is available.
  static std::unique_ptr<Transport> open_default();
  static std::unique_ptr<Transport> open_socket(const char *path);
  static std::unique_ptr<Transport> open_wmtr();
  // a native (non-Wine) process: the runtime's own wmtr context (wmtr_native.c); Vulkan eyes only
  static std::unique_ptr<Transport> open_native();
};
}  // namespace svr

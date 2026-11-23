// SPDX-License-Identifier: Apache-2.0
// Host link for the guest runtime: wmtr (production) or a Unix socket (vr-fakehost, tests).
#include "transport.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "log.h"
#include "wmtr_native.h"

namespace svr {
namespace {
class SocketTransport : public Transport, public BufferSink {
 public:
  explicit SocketTransport(int fd) : fd_(fd) {}
  ~SocketTransport() override { close(fd_); }
  int call(const svr_hdr *req, svr_hdr *rep, uint32_t rep_cap) override { return call_fd(req, -1, rep, rep_cap); }
  // a request with a descriptor attached (SCM_RIGHTS): SVR_BUFFER's dma-buf
  int call_fd(const svr_hdr *req, int pass_fd, svr_hdr *rep, uint32_t rep_cap) {
    std::lock_guard<std::mutex> g(m_);
    iovec iov{const_cast<svr_hdr *>(req), req->size};
    msghdr m{};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int))];
    if (pass_fd >= 0) {
      m.msg_control = cbuf;
      m.msg_controllen = sizeof cbuf;
      cmsghdr *c = CMSG_FIRSTHDR(&m);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(c), &pass_fd, sizeof pass_fd);
    }
    if (sendmsg(fd_, &m, MSG_NOSIGNAL) != (ssize_t)req->size) return -errno;
    ssize_t n = recv(fd_, rep, rep_cap, 0);
    if (n < (ssize_t)sizeof(svr_hdr)) return n < 0 ? -errno : -EPROTO;
    if (rep->size != (uint32_t)n) return -EPROTO;
    return 0;
  }
  int submit(const svr_wmtvr_submit &info) override {
    svr_submit_req req{};
    req.h = {SVR_SUBMIT, sizeof(req)};
    req.info = info;
    svr_ack ack{};
    int r = call(&req.h, &ack.h, sizeof(ack));
    return r ? r : ack.status;
  }
  const char *name() const override { return "socket"; }
  BufferSink *buffer_sink() override { return this; }

  int register_buffer(uint32_t id, int fd, uint64_t size, bool dmabuf) override {
    svr_buffer_req r{};
    r.h = {SVR_BUFFER, sizeof(r)};
    r.buffer_id = id, r.action = SVR_BUFFER_REGISTER, r.flags = dmabuf ? SVR_BUFFER_DMABUF : 0, r.size = size;
    svr_ack ack{};
    int e = call_fd(&r.h, fd, &ack.h, sizeof(ack));
    return e ? e : ack.status;
  }
  int unregister_buffer(uint32_t id) override {
    svr_buffer_req r{};
    r.h = {SVR_BUFFER, sizeof(r)};
    r.buffer_id = id, r.action = SVR_BUFFER_UNREGISTER;
    svr_ack ack{};
    int e = call(&r.h, &ack.h, sizeof(ack));
    return e ? e : ack.status;
  }
  int submit_buffers(const svr_submit_buf_req &req) override {
    svr_ack ack{};
    int e = call(&req.h, &ack.h, sizeof(ack));
    return e ? e : ack.status;
  }

 private:
  int fd_;
  std::mutex m_;
};

// Vulkan eye buffers through the runtime's own wmtr context (wmtr_native.c), in a Wine process as in a native one:
// the context is dedicated to them, so the executor may wait there for the blit out of a buffer before it acks
// (the context of Wine's winemetal.so also carries DXMT's rendering and must never wait). One per process.
class NativeSink : public BufferSink {
 public:
  static NativeSink *get() {
    static NativeSink *s = svr_wmtr_native_open() == 0 ? new NativeSink() : nullptr;
    return s;
  }
  int register_buffer(uint32_t id, int fd, uint64_t size, bool dmabuf) override {
    if (!dmabuf) return -ENOTSUP;  // only a dma-buf names a virtio-gpu resource
    uint32_t bo = 0, res = 0;
    if (int e = svr_wmtr_native_import_fd(fd, &bo, &res)) {
      SVR_ERR("cannot import eye buffer %u into the wmtr context (%d)", id, e);
      return e;
    }
    svr_buffer_req r{};
    r.h = {SVR_BUFFER, sizeof(r)};
    r.buffer_id = id, r.action = SVR_BUFFER_REGISTER, r.res_id = res, r.flags = SVR_BUFFER_DMABUF, r.size = size;
    int e = ack_call(&r.h);
    if (e) {
      // a host without the attach hook (libkrun patch 0031) or without SVR_BUFFER
      SVR_ERR("host refused eye buffer %u (resource %u): %d", id, res, e);
      svr_wmtr_native_close_bo(bo);
      return e;
    }
    std::lock_guard<std::mutex> g(m_);
    bos_[id] = bo;
    return 0;
  }
  int unregister_buffer(uint32_t id) override {
    svr_buffer_req r{};
    r.h = {SVR_BUFFER, sizeof(r)};
    r.buffer_id = id, r.action = SVR_BUFFER_UNREGISTER;
    int e = ack_call(&r.h);
    uint32_t bo = 0;
    {
      std::lock_guard<std::mutex> g(m_);
      auto it = bos_.find(id);
      if (it != bos_.end()) bo = it->second, bos_.erase(it);
    }
    if (bo) svr_wmtr_native_close_bo(bo);  // after the host let go of the buffer
    return e;
  }
  int submit_buffers(const svr_submit_buf_req &req) override { return ack_call(&req.h); }

 private:
  static int ack_call(const svr_hdr *req) {
    svr_ack ack{};
    int e = svr_wmtr_native_call(req, req->size, &ack, sizeof(ack));
    if (e) return e;
    if (ack.h.size != sizeof(ack)) return -EPROTO;
    return ack.status;
  }
  std::mutex m_;
  std::map<uint32_t, uint32_t> bos_;  // buffer id -> GEM handle in the wmtr context (keeps the resource attached)
};

// a native (non-Wine) VR process: everything over the runtime's own wmtr context
class NativeTransport : public Transport {
 public:
  int call(const svr_hdr *req, svr_hdr *rep, uint32_t rep_cap) override {
    int r = svr_wmtr_native_call(req, req->size, rep, rep_cap);
    if (r) return r;
    if (rep->size < sizeof(svr_hdr) || rep->size > rep_cap) return -EPROTO;
    return 0;
  }
  int submit(const svr_wmtvr_submit &) override { return -ENOTSUP; }  // DXMT textures need Wine's winemetal.so
  const char *name() const override { return "wmtr-native"; }
  BufferSink *buffer_sink() override { return NativeSink::get(); }
};

// int wmtr_vr_call(const void *req, uint32_t req_size, void *rep, uint32_t rep_cap): exported by the wmtr
// client (wmtr/src/wmtr_client.c, loaded by Wine as winemetal.so). Returns 0 or a negative errno.
typedef int (*wmtr_vr_call_fn)(const void *, uint32_t, void *, uint32_t);
// winemetal unix call entry (the wmtr client's __wine_unix_call_funcs[]): NTSTATUS (*)(void *params)
typedef uint32_t (*unix_call_fn)(void *);

class WmtrTransport : public Transport {
 public:
  WmtrTransport(wmtr_vr_call_fn fn, unix_call_fn submit) : fn_(fn), submit_(submit) {}
  int submit(const svr_wmtvr_submit &info) override {
    svr_wmtvr_submit params = info;  // struct unixcall_wmtvr_submit { struct WMTVRSubmitInfo info; }
    uint32_t st = submit_(&params);
    if (st) SVR_ERR("WMTVRSubmit: status %#x", st);
    return st ? -EIO : 0;
  }
  int call(const svr_hdr *req, svr_hdr *rep, uint32_t rep_cap) override {
    int r = fn_(req, req->size, rep, rep_cap);
    if (r) return r;
    if (rep->size < sizeof(svr_hdr) || rep->size > rep_cap) return -EPROTO;
    return 0;
  }
  const char *name() const override { return "wmtr"; }
  // DXMT textures go through winemetal's context; Vulkan eye buffers through the runtime's own (NativeSink)
  BufferSink *buffer_sink() override { return NativeSink::get(); }

 private:
  wmtr_vr_call_fn fn_;
  unix_call_fn submit_;
};

int find_winemetal(struct dl_phdr_info *info, size_t, void *out) {
  const char *n = info->dlpi_name;
  size_t len = n ? strlen(n) : 0;
  if (len >= 13 && !strcmp(n + len - 13, "/winemetal.so")) {
    *(std::string *)out = n;
    return 1;
  }
  return 0;
}

// Wine's own unixlib directory (where Proton keeps winemetal.so): the one holding the loaded ntdll.so
int find_unixlib_dir(struct dl_phdr_info *info, size_t, void *out) {
  const char *n = info->dlpi_name;
  size_t len = n ? strlen(n) : 0;
  if (len >= 9 && !strcmp(n + len - 9, "/ntdll.so")) {
    *(std::string *)out = std::string(n, len - 8);
    return 1;
  }
  return 0;
}
}  // namespace

std::unique_ptr<Transport> Transport::open_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return nullptr;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  if (connect(fd, (sockaddr *)&a, sizeof(a))) {
    SVR_ERR("cannot connect to %s: %s", path, strerror(errno));
    close(fd);
    return nullptr;
  }
  return std::unique_ptr<Transport>(new SocketTransport(fd));
}

std::unique_ptr<Transport> Transport::open_wmtr() {
  std::string path;
  dl_iterate_phdr(find_winemetal, &path);
  void *h = path.empty() ? nullptr : dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!h) {
    // DXMT not loaded in this process (Proton's steam.exe runs VR_Init for the registry before any game
    // renders): load Wine's winemetal.so ourselves for its own wmtr context. Only reached on VR init.
    path.clear();
    dl_iterate_phdr(find_unixlib_dir, &path);
    if (path.empty() || path.back() != '/') return nullptr;
    path += "winemetal.so";
    if (!(h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL))) {
      SVR_ERR("cannot load %s: %s", path.c_str(), dlerror());
      return nullptr;
    }
  }
  auto fn = (wmtr_vr_call_fn)dlsym(h, "wmtr_vr_call");
  auto table = (const void *const *)dlsym(h, "__wine_unix_call_funcs");
  if (!fn || !table) {
    SVR_ERR("%s has no wmtr_vr_call (wmtr client too old)", path.c_str());
    return nullptr;
  }
  return std::unique_ptr<Transport>(new WmtrTransport(fn, (unix_call_fn)table[SVR_WMTVR_SUBMIT_CALL]));
}

std::unique_ptr<Transport> Transport::open_native() {
  int r = svr_wmtr_native_open();
  if (r) {
    SVR_ERR("no wmtr link of its own (virtio-gpu without the wmtr capset?): %d", r);
    return nullptr;
  }
  return std::unique_ptr<Transport>(new NativeTransport());
}

std::unique_ptr<Transport> Transport::open_default() {
  if (const char *s = getenv("STEAMAC_VR_SOCKET"); s && *s) return open_socket(s);
  // Wine processes (Proton games, steam.exe) go through winemetal.so; a native process has no Wine (no ntdll.so)
  std::string ntdll;
  dl_iterate_phdr(find_unixlib_dir, &ntdll);
  if (!ntdll.empty()) return open_wmtr();
  return open_native();
}
}  // namespace svr

/* wmtr client transport: how the client gets shared memory and wakes/waits for the host.
 * Implementations: socket+shm (two macOS processes, P2 prototype) and virtio-gpu (VM guest). */
#ifndef WMTR_TRANSPORT_H
#define WMTR_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>

struct wmtr_transport {
  /* create the ring blob (blob 0) and connect; returns its client mapping */
  void *(*open_ring)(struct wmtr_transport *t, uint64_t size);
  /* create arena blob number `blob` of `size` bytes, make the host map it; returns client mapping */
  void *(*add_blob)(struct wmtr_transport *t, uint32_t blob, uint64_t size);
  /* wake the host executor (it set host_idle) */
  void (*kick)(struct wmtr_transport *t);
  /* block until *done_seq >= seq (the host kicks waiting slots); poll for at most spin_ns first
   * (wmtr_spin.h: 0 for calls the host answers in milliseconds) */
  void (*wait)(struct wmtr_transport *t, volatile uint64_t *done_seq, uint64_t seq, uint32_t slot,
               uint64_t spin_ns);
  /* optional: give arena blob `blob` (client mapping `base`, `size` bytes, no live range left and the
   * host holds no buffer in it) back to the host; afterwards the number may be added again.
   * NULL: blobs are kept for the life of the process */
  void (*release_blob)(struct wmtr_transport *t, uint32_t blob, void *base, uint64_t size);
  void *priv;
  /* optional (virtio-gpu only; NULL elsewhere): import a dma-buf of another virtio-gpu context (a Venus allocation
   * of a native Vulkan game's VR eye buffer) into this context's DRM fd. The kernel attaches the resource to the
   * wmtr context, whose attach hook hands the resource's host memory to the executor. Stores the GEM handle (keep it
   * open while the host may read the memory) and the virtio-gpu resource id. 0 or a negative errno. */
  int (*import_fd)(struct wmtr_transport *t, int fd, uint32_t *bo, uint32_t *res_id);
  /* optional: close a handle from import_fd (the kernel detaches the resource from the context) */
  void (*close_bo)(struct wmtr_transport *t, uint32_t bo);
};

struct wmtr_transport *wmtr_transport_create(void);

#endif

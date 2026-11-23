/* Host memory of guest Vulkan eye buffers (the vm side of SVR_BUFFER / SVR_SUBMIT_BUF, steamac_vr_guest.h).
 *
 * A native-Vulkan game's VR eyes are copied by the guest runtime into buffers on the game's Venus device. On this
 * host that memory is POSIX shm (virglrenderer's macOS Venus); when the runtime imports a buffer into its wmtr
 * context, the context's attach hook (libkrun patch 0031) hands the resource's shm fd to the executor library
 * (wmtr_resource_attach), which maps it here by virtio-gpu resource id (only ever read). The VR exporter takes a
 * reference per registered buffer and blits from the mapping (MTLBuffer without a copy) into the headset swapchain.
 *
 * Plain C (no Metal), process-wide, thread-safe: the VMM's GPU worker attaches and detaches, executor threads use. */
#ifndef WMTR_VR_SHM_H
#define WMTR_VR_SHM_H
#include <stdint.h>

#include "steamac_vr_guest.h"

/* the attach hook: takes ownership of `fd` (closed here, also on failure). A resource attached to several contexts
 * is counted once per attach. 0 or a negative errno. */
int wmtr_shm_attach(uint32_t res_id, int fd, uint64_t size);
void wmtr_shm_detach(uint32_t res_id);
/* a use of an attached resource's mapping (the VR exporter's registered buffer): its address and mapped length
 * (whole host pages) are stored, and the mapping stays until the last attach and the last use are gone.
 * NULL if `res_id` is not attached. */
const void *wmtr_shm_acquire(uint32_t res_id, uint64_t *len);
/* the same for memory the executor writes (the video encoder's bitstreams): NULL also when the fd only allowed a
 * read-only mapping. Released with wmtr_shm_release. */
void *wmtr_shm_acquire_rw(uint32_t res_id, uint64_t *len);
void wmtr_shm_release(uint32_t res_id);
/* resources currently mapped (tests, stats) */
uint32_t wmtr_shm_count(void);

/* Metal pixel formats of buffer eyes (MTLPixelFormat values; this header stays free of Metal) */
#define WMTR_MTL_RGBA8 70u
#define WMTR_MTL_RGBA8_SRGB 71u
#define WMTR_MTL_BGRA8 80u
#define WMTR_MTL_BGRA8_SRGB 81u
#define WMTR_MTL_RGBA16F 115u

/* checks a guest eye description against its buffer's `len` bytes; stores the Metal pixel format and the bytes per
 * texel. 0, or a negative errno (-EINVAL: rectangle outside the buffer or bad pitch; -ENOTSUP: format). */
int wmtr_vr_buf_eye_check(const struct svr_buf_eye *e, uint64_t len, uint32_t *mtl_format, uint32_t *bpe);

#endif

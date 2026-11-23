/* SPDX-License-Identifier: Apache-2.0
 * Host link for native (non-Wine) VR games in the guest: a wmtr context of the runtime's own, used only for the
 * guest VR protocol (WMTR_CTL_VR_QUERY) and to import Vulkan eye buffers (dma-bufs) into the context.
 *
 * Proton games reach the executor through Wine's winemetal.so (DXMT's wmtr client). A native ARM64 Linux game
 * (Half-Life: Alyx's Steam Frame build, any Linux VR port) has no Wine: the runtime opens the virtio-gpu render node
 * itself with the wmtr capset (wmtr/src/transport_virtgpu.c), maps the ring, and sends synchronous control records
 * from one reply slot. The executor answers VR queries from any context (its VR exporter is process-wide). */
#ifndef SVR_WMTR_NATIVE_H
#define SVR_WMTR_NATIVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 0 when connected (idempotent), a negative errno otherwise (-ENODEV: no virtio-gpu with the wmtr capset) */
int svr_wmtr_native_open(void);
/* one guest VR message; the reply overwrites `rep` (at most rep_cap bytes). 0 or a negative errno. Thread-safe. */
int svr_wmtr_native_call(const void *req, uint32_t req_size, void *rep, uint32_t rep_cap);
/* import a dma-buf into the context (see wmtr_transport.import_fd). 0 or a negative errno. */
int svr_wmtr_native_import_fd(int fd, uint32_t *bo, uint32_t *res_id);
void svr_wmtr_native_close_bo(uint32_t bo);

#ifdef __cplusplus
}
#endif
#endif

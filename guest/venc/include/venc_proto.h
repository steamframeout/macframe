/* Guest <-> Mac protocol of the VideoToolbox encoder backend (backend_vt.so in the guest, wmtr_venc.m in the
 * executor). Messages travel like the guest VR protocol: one synchronous WMTR_CTL_VR_QUERY record whose buffer the
 * host overwrites with the reply (svr_wmtr_native_call). Pictures and bitstreams never travel in messages: they
 * live in guest memory the host maps (Venus host-visible memory is POSIX shm on macOS; the guest imports the
 * dma-buf into its wmtr context, the context's attach hook hands the shm to the executor: wmtr_vr_shm.h), named
 * by virtio-gpu resource id.
 *
 * Tests run the same messages over a unix socket (VENC_VT_SOCKET) with the memory fds passed as SCM_RIGHTS. */
#ifndef VENC_PROTO_H
#define VENC_PROTO_H

#include <stdint.h>

#include "venc_backend.h"

#define VENC_PROTO_OP 7u          /* svr_hdr.op (steamac_vr_guest.h: SVR_VENC) */
#define VENC_PROTO_VERSION 1u

enum venc_cmd {
    VENC_CMD_QUERY = 1,     /* codec -> limits */
    VENC_CMD_CREATE = 2,    /* config -> session */
    VENC_CMD_HEADERS = 3,   /* session -> parameter sets (Annex B) in data[] */
    VENC_CMD_ENCODE = 4,    /* session, picture memory, output memory -> bytes, idr */
    VENC_CMD_RECONFIG = 5,  /* session, config (rate control) */
    VENC_CMD_DESTROY = 6,   /* session */
    VENC_CMD_MEMORY = 7,    /* tests only: res_id of an fd sent with the message (SCM_RIGHTS) */
};

#define VENC_PROTO_DATA 2048u

/* One message; the reply reuses it (status < 0: negative errno). Fixed size: the buffer is also the reply's room. */
struct venc_msg {
    uint32_t op;          /* svr_hdr: VENC_PROTO_OP */
    uint32_t size;        /* svr_hdr: sizeof(struct venc_msg) */
    uint32_t version;     /* VENC_PROTO_VERSION */
    uint32_t cmd;         /* enum venc_cmd */
    int32_t status;       /* reply */
    uint32_t session;
    uint32_t codec;       /* QUERY */
    uint32_t pad0;
    struct venc_config config;      /* CREATE, RECONFIG */
    struct venc_limits limits;      /* QUERY reply */
    /* ENCODE: the picture, NV12/P010, in resource in_res; the bitstream goes to out_res at out_offset */
    uint32_t in_res, out_res;
    uint64_t luma_offset, chroma_offset;
    uint32_t luma_stride, chroma_stride;
    uint32_t width, height;
    uint64_t out_offset, out_capacity;
    uint32_t force_idr;
    uint32_t is_idr;      /* reply */
    uint64_t bytes;       /* reply: ENCODE bitstream bytes, HEADERS bytes in data[] */
    uint64_t pts;
    uint64_t mem_size;    /* MEMORY */
    uint8_t data[VENC_PROTO_DATA];
};

#endif

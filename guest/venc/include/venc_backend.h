/* Encoder backends for the steamac Vulkan Video encode layer.
 *
 * The layer (layer/venc_layer.cpp) gives drivers without Vulkan Video encode (Venus on macOS) a video-encode
 * queue family with VK_KHR_video_encode_h264/h265. It hands every encoded picture to a backend loaded with
 * dlopen(): backend_x26x.so (x264/x265 in the guest, for tests and as a software fallback) or backend_vt.so (the
 * Mac's VideoToolbox, reached over wmtr). A backend owns the bitstream syntax: it produces the parameter sets
 * (vkGetEncodedVideoSessionParametersKHR returns them, reported as overrides) and the slice NAL units of each
 * picture (Annex B, start codes, no SPS/PPS/VPS/SEI/AUD), so both always agree.
 */
#ifndef VENC_BACKEND_H
#define VENC_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VENC_ABI 3

enum venc_codec { VENC_H264 = 1, VENC_H265 = 2 };

enum venc_rc {
    VENC_RC_DEFAULT = 0,
    VENC_RC_CQP = 1, /* rate control disabled: constant QP */
    VENC_RC_CBR = 2,
    VENC_RC_VBR = 3,
};

/* What a stream looks like. Two configs with equal fields except the rate control ones produce identical
 * parameter sets: backends must keep that true, the layer relies on it to reconfigure without new headers. */
struct venc_config {
    uint32_t codec;          /* enum venc_codec */
    uint32_t width, height;  /* visible picture size */
    uint32_t profile_idc;    /* H.264: 66/77/100; H.265: 1 (Main) or 2 (Main 10); 0: backend default */
    uint32_t bit_depth;      /* 8 (10 for H.265 Main 10) */
    uint32_t cabac;          /* H.264 entropy coding: 1 CABAC, 0 CAVLC */
    uint32_t transform_8x8;  /* H.264: 8x8 transforms allowed (High profile) */
    int32_t chroma_qp_offset;/* H.264: wanted chroma_qp_index_offset (a hint) */
    /* H.265 coding tools of the application's SPS (hints: a backend that can, matches them; 0 = backend default) */
    uint32_t h265_log2_ctb, h265_log2_min_cb, h265_log2_min_tb, h265_log2_max_tb;
    uint32_t h265_tu_depth_inter, h265_tu_depth_intra;   /* max_transform_hierarchy_depth + 1 (0: default) */
    uint32_t h265_amp, h265_sao, h265_tmvp, h265_strong_intra;   /* 1 off, 2 on (0: default) */
    uint32_t fps_num, fps_den;
    /* rate control: changeable with venc_backend_ops.reconfigure */
    uint32_t rc;             /* enum venc_rc */
    uint64_t avg_bitrate;    /* bits per second (CBR/VBR) */
    uint64_t max_bitrate;
    uint32_t vbv_ms;         /* virtual buffer size in milliseconds, 0: backend default */
    uint32_t qp;             /* CQP */
};

struct venc_limits {
    uint32_t max_width, max_height;
    uint32_t min_width, min_height;
    uint64_t max_bitrate;
    uint32_t max_level_idc;  /* H.264: level_idc (e.g. 52); H.265: general_level_idc (e.g. 186 = 6.2) */
    uint32_t ten_bit;        /* H.265 Main 10 supported */
};

/* Picture in guest memory: 4:2:0, two planes (NV12, or P010 for 10 bit: 16-bit samples, high bits).
 * Pointers are always set. Backends that import memory (venc_backend_ops.import_memory) also get the memory
 * handle and offsets, and write the bitstream to out_mem at out_offset (the same bytes `out` points at). */
struct venc_picture {
    const uint8_t *luma;
    const uint8_t *chroma;   /* interleaved Cb Cr */
    uint64_t mem;            /* 0, or the handle of the memory holding both planes */
    uint64_t luma_offset, chroma_offset;
    uint64_t out_mem;        /* 0, or the handle of the memory `out` points into */
    uint64_t out_offset;
    uint32_t luma_stride, chroma_stride;
    uint32_t width, height;  /* the region to encode (= config size) */
    int force_idr;
    uint64_t pts;
};

struct venc_backend_ops {
    uint32_t abi;            /* VENC_ABI */
    const char *name;
    int (*query)(uint32_t codec, struct venc_limits *out);   /* 0 if the codec is supported */
    void *(*create)(const struct venc_config *cfg);
    /* Parameter sets as Annex B NAL units (H.265: VPS, SPS, PPS; H.264: SPS, PPS), ids 0. */
    int (*headers)(void *enc, uint8_t *out, size_t *inout_size);
    /* Encodes one picture. Writes the slice NAL units (Annex B) to out; returns bytes written, -ENOSPC when the
     * picture does not fit (nothing written, the encoder state still advanced), or another negative errno. */
    long (*encode)(void *enc, const struct venc_picture *pic, uint8_t *out, size_t out_size, int *is_idr);
    int (*reconfigure)(void *enc, const struct venc_config *cfg);  /* rate control only */
    void (*destroy)(void *enc);
    /* Optional: memory the backend reads pictures from / writes bitstreams to without copies (the Mac backend:
     * shared with the host). `fd` is a dma-buf (dmabuf = 1) or an opaque Vulkan memory fd; the backend dups it.
     * Returns 0 and a nonzero handle, or a negative errno (the layer then passes pointers only). */
    int (*import_memory)(int fd, uint64_t size, int dmabuf, uint64_t *handle);
    void (*release_memory)(uint64_t handle);
};

/* Every backend shared object exports this. */
const struct venc_backend_ops *venc_backend_get(void);
typedef const struct venc_backend_ops *(*venc_backend_get_fn)(void);

/* Helpers shared by backends (venc_nal.c). */
/* Walks Annex B (or, with length_size > 0, length-prefixed AVCC/HVCC) data and appends to out the NAL units for
 * which keep(type) is true, each with a 4-byte start code. Returns bytes written or -ENOSPC. */
long venc_nal_filter(uint32_t codec, const uint8_t *in, size_t in_size, int length_size,
                     uint8_t *out, size_t out_size, int (*keep)(uint32_t codec, uint32_t type));
uint32_t venc_nal_type(uint32_t codec, uint8_t first_byte);
int venc_nal_is_slice(uint32_t codec, uint32_t type);
int venc_nal_is_param_set(uint32_t codec, uint32_t type);

#ifdef __cplusplus
}
#endif
#endif

/* Software encoder backend: x264 (H.264) and x265 (H.265), in the guest process.
 *
 * Used by the layer's tests and as a fallback where the Mac's encoder is unavailable. Low-latency settings
 * (zerolatency: no B-frames, no lookahead, one picture in, one out), infinite GOP: IDR only when asked.
 */
#include "venc_backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x264.h>
#include <x265.h>

struct enc {
    struct venc_config cfg;
    x264_t *x264;
    x264_param_t p264;
    x265_encoder *x265;
    x265_param *p265;
    x265_picture *pic265;
    uint8_t *planes[3];       /* x265 wants I420: de-interleaved chroma */
    uint64_t frames;
};

static int keep_slices(uint32_t codec, uint32_t t) { return venc_nal_is_slice(codec, t); }
static int keep_params(uint32_t codec, uint32_t t) { return venc_nal_is_param_set(codec, t); }

static int query(uint32_t codec, struct venc_limits *l)
{
    memset(l, 0, sizeof(*l));
    l->min_width = l->min_height = 16;
    l->max_width = l->max_height = codec == VENC_H265 ? 8192 : 4096;
    l->max_bitrate = 2000000000ull;
    l->max_level_idc = codec == VENC_H265 ? 186 : 52;
    l->ten_bit = 0;
    return codec == VENC_H264 || codec == VENC_H265 ? 0 : -ENOTSUP;
}

static const char *h264_profile(uint32_t idc)
{
    switch (idc) {
    case 66: return "baseline";
    case 77: return "main";
    default: return "high";
    }
}

static void rc264(x264_param_t *p, const struct venc_config *c)
{
    uint32_t kbps = (uint32_t)(c->avg_bitrate / 1000), maxk = (uint32_t)(c->max_bitrate / 1000);
    uint32_t vbv = c->vbv_ms ? c->vbv_ms : 1000;
    switch (c->rc) {
    case VENC_RC_CQP:
        p->rc.i_rc_method = X264_RC_CQP;
        p->rc.i_qp_constant = (int)(c->qp ? c->qp : 26);
        break;
    case VENC_RC_CBR:
    case VENC_RC_VBR:
        if (!kbps)
            kbps = 20000;
        if (maxk < kbps)
            maxk = kbps;
        p->rc.i_rc_method = X264_RC_ABR;
        p->rc.i_bitrate = (int)kbps;
        p->rc.i_vbv_max_bitrate = (int)(c->rc == VENC_RC_CBR ? kbps : maxk);
        p->rc.i_vbv_buffer_size = (int)((uint64_t)p->rc.i_vbv_max_bitrate * vbv / 1000);
        break;
    default:
        p->rc.i_rc_method = X264_RC_CRF;
        p->rc.f_rf_constant = 23;
        break;
    }
}

static void rc265(x265_param *p, const struct venc_config *c)
{
    uint32_t kbps = (uint32_t)(c->avg_bitrate / 1000), maxk = (uint32_t)(c->max_bitrate / 1000);
    uint32_t vbv = c->vbv_ms ? c->vbv_ms : 1000;
    switch (c->rc) {
    case VENC_RC_CQP:
        p->rc.rateControlMode = X265_RC_CQP;
        p->rc.qp = (int)(c->qp ? c->qp : 28);
        break;
    case VENC_RC_CBR:
    case VENC_RC_VBR:
        if (!kbps)
            kbps = 20000;
        if (maxk < kbps)
            maxk = kbps;
        p->rc.rateControlMode = X265_RC_ABR;
        p->rc.bitrate = (int)kbps;
        p->rc.vbvMaxBitrate = (int)(c->rc == VENC_RC_CBR ? kbps : maxk);
        p->rc.vbvBufferSize = (int)((uint64_t)p->rc.vbvMaxBitrate * vbv / 1000);
        break;
    default:
        p->rc.rateControlMode = X265_RC_CRF;
        p->rc.rfConstant = 28;
        break;
    }
}

static void destroy(void *v)
{
    struct enc *e = v;
    if (!e)
        return;
    if (e->x264)
        x264_encoder_close(e->x264);
    if (e->x265)
        x265_encoder_close(e->x265);
    if (e->pic265)
        x265_picture_free(e->pic265);
    if (e->p265)
        x265_param_free(e->p265);
    for (int i = 0; i < 3; i++)
        free(e->planes[i]);
    free(e);
}

static void *create(const struct venc_config *c)
{
    struct enc *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->cfg = *c;
    uint32_t fn = c->fps_num ? c->fps_num : 90, fd = c->fps_den ? c->fps_den : 1;
    if (c->codec == VENC_H264) {
        x264_param_t *p = &e->p264;
        if (x264_param_default_preset(p, "veryfast", "zerolatency") < 0)
            goto fail;
        p->i_width = (int)c->width;
        p->i_height = (int)c->height;
        p->i_csp = X264_CSP_NV12;
        p->i_fps_num = fn;
        p->i_fps_den = fd;
        p->i_keyint_max = X264_KEYINT_MAX_INFINITE;
        p->b_repeat_headers = 0;
        p->b_annexb = 1;
        p->b_aud = 0;
        p->i_log_level = X264_LOG_ERROR;
        p->b_cabac = c->profile_idc == 66 ? 0 : (int)c->cabac;
        p->i_threads = 1;                   /* one slice per picture, no frame threads */
        p->analyse.b_transform_8x8 = c->profile_idc >= 100 && c->transform_8x8;
        p->analyse.i_weighted_pred = X264_WEIGHTP_NONE;
        p->analyse.i_chroma_qp_offset = c->chroma_qp_offset;
        p->analyse.b_psy = 0;               /* psy would move the chroma QP offset */
        p->b_deblocking_filter = 1;
        p->i_deblocking_filter_alphac0 = 0;
        p->i_deblocking_filter_beta = 0;
        p->i_bframe = 0;
        rc264(p, c);
        if (x264_param_apply_profile(p, h264_profile(c->profile_idc)) < 0)
            goto fail;
        e->x264 = x264_encoder_open(p);
        if (!e->x264)
            goto fail;
    } else if (c->codec == VENC_H265) {
        e->p265 = x265_param_alloc();
        if (!e->p265 || x265_param_default_preset(e->p265, "ultrafast", "zerolatency") < 0)
            goto fail;
        x265_param *p = e->p265;
        p->sourceWidth = (int)c->width;
        p->sourceHeight = (int)c->height;
        p->internalCsp = X265_CSP_I420;
        p->fpsNum = fn;
        p->fpsDenom = fd;
        p->keyframeMax = -1;                /* infinite */
        p->bRepeatHeaders = 0;
        p->bAnnexB = 1;
        p->bEnableAccessUnitDelimiters = 0;
        p->bEmitInfoSEI = 0;
        p->bEmitHRDSEI = 0;
        p->bframes = 0;
        p->logLevel = X265_LOG_ERROR;
        p->frameNumThreads = 1;
        p->bEnableWavefront = 0;
        rc265(p, c);
        if (x265_param_apply_profile(p, "main") < 0)
            goto fail;
        e->x265 = x265_encoder_open(p);
        e->pic265 = x265_picture_alloc();
        if (!e->x265 || !e->pic265)
            goto fail;
        x265_picture_init(p, e->pic265);
        size_t cw = (c->width + 1) / 2, ch = (c->height + 1) / 2;
        e->planes[1] = malloc(cw * ch);
        e->planes[2] = malloc(cw * ch);
        if (!e->planes[1] || !e->planes[2])
            goto fail;
    } else {
        goto fail;
    }
    return e;
fail:
    destroy(e);
    return NULL;
}

static int headers(void *v, uint8_t *out, size_t *inout)
{
    struct enc *e = v;
    uint8_t tmp[4096];
    size_t n = 0;
    if (e->x264) {
        x264_nal_t *nal;
        int nn;
        if (x264_encoder_headers(e->x264, &nal, &nn) < 0)
            return -EIO;
        for (int i = 0; i < nn; i++) {
            if (n + (size_t)nal[i].i_payload > sizeof(tmp))
                return -ENOSPC;
            memcpy(tmp + n, nal[i].p_payload, (size_t)nal[i].i_payload);
            n += (size_t)nal[i].i_payload;
        }
    } else {
        x265_nal *nal;
        uint32_t nn;
        if (x265_encoder_headers(e->x265, &nal, &nn) < 0)
            return -EIO;
        for (uint32_t i = 0; i < nn; i++) {
            if (n + nal[i].sizeBytes > sizeof(tmp))
                return -ENOSPC;
            memcpy(tmp + n, nal[i].payload, nal[i].sizeBytes);
            n += nal[i].sizeBytes;
        }
    }
    long r = venc_nal_filter(e->cfg.codec, tmp, n, 0, out, *inout, keep_params);
    if (r < 0)
        return (int)r;
    *inout = (size_t)r;
    return 0;
}

static long encode(void *v, const struct venc_picture *pic, uint8_t *out, size_t out_size, int *is_idr)
{
    struct enc *e = v;
    long total = 0;
    *is_idr = 0;
    if (e->x264) {
        x264_picture_t in, o;
        x264_nal_t *nal;
        int nn;
        x264_picture_init(&in);
        in.img.i_csp = X264_CSP_NV12;
        in.img.i_plane = 2;
        in.img.plane[0] = (uint8_t *)pic->luma;
        in.img.plane[1] = (uint8_t *)pic->chroma;
        in.img.i_stride[0] = (int)pic->luma_stride;
        in.img.i_stride[1] = (int)pic->chroma_stride;
        in.i_type = pic->force_idr ? X264_TYPE_IDR : X264_TYPE_AUTO;
        in.i_pts = (int64_t)e->frames++;
        int sz = x264_encoder_encode(e->x264, &nal, &nn, &in, &o);
        if (sz < 0)
            return -EIO;
        for (int i = 0; i < nn; i++) {
            if (!venc_nal_is_slice(VENC_H264, (uint32_t)nal[i].i_type))
                continue;
            long r = venc_nal_filter(VENC_H264, nal[i].p_payload, (size_t)nal[i].i_payload, 0,
                                     out + total, out_size - (size_t)total, keep_slices);
            if (r < 0)
                return r;
            total += r;
        }
        *is_idr = o.b_keyframe;
        return total;
    }
    /* x265: de-interleave NV12 chroma into I420 */
    uint32_t cw = (pic->width + 1) / 2, ch = (pic->height + 1) / 2;
    for (uint32_t y = 0; y < ch; y++) {
        const uint8_t *s = pic->chroma + (size_t)y * pic->chroma_stride;
        uint8_t *u = e->planes[1] + (size_t)y * cw, *w = e->planes[2] + (size_t)y * cw;
        for (uint32_t x = 0; x < cw; x++) {
            u[x] = s[2 * x];
            w[x] = s[2 * x + 1];
        }
    }
    x265_picture *in = e->pic265;
    in->planes[0] = (void *)pic->luma;
    in->planes[1] = e->planes[1];
    in->planes[2] = e->planes[2];
    in->stride[0] = (int)pic->luma_stride;
    in->stride[1] = in->stride[2] = (int)cw;
    in->sliceType = pic->force_idr ? X265_TYPE_IDR : X265_TYPE_AUTO;
    in->pts = (int64_t)e->frames++;
    x265_nal *nal;
    uint32_t nn = 0;
    x265_picture o;
    x265_picture_init(e->p265, &o);
    int r = x265_encoder_encode(e->x265, &nal, &nn, in, &o);
    if (r < 0)
        return -EIO;
    for (uint32_t i = 0; i < nn; i++) {
        if (!venc_nal_is_slice(VENC_H265, nal[i].type))
            continue;
        long w = venc_nal_filter(VENC_H265, nal[i].payload, nal[i].sizeBytes, 0, out + total,
                                 out_size - (size_t)total, keep_slices);
        if (w < 0)
            return w;
        total += w;
    }
    *is_idr = o.sliceType == X265_TYPE_IDR;
    return total;
}

static int reconfigure(void *v, const struct venc_config *c)
{
    struct enc *e = v;
    e->cfg.rc = c->rc;
    e->cfg.avg_bitrate = c->avg_bitrate;
    e->cfg.max_bitrate = c->max_bitrate;
    e->cfg.vbv_ms = c->vbv_ms;
    e->cfg.qp = c->qp;
    if (e->x264) {
        x264_param_t p;
        x264_encoder_parameters(e->x264, &p);
        rc264(&p, &e->cfg);
        return x264_encoder_reconfig(e->x264, &p) < 0 ? -EINVAL : 0;
    }
    x265_param *p = x265_param_alloc();
    if (!p)
        return -ENOMEM;
    x265_encoder_parameters(e->x265, p);
    rc265(p, &e->cfg);
    int r = x265_encoder_reconfig(e->x265, p);
    x265_param_free(p);
    return r < 0 ? -EINVAL : 0;
}

static const struct venc_backend_ops ops = {
    .abi = VENC_ABI,
    .name = "x26x",
    .query = query,
    .create = create,
    .headers = headers,
    .encode = encode,
    .reconfigure = reconfigure,
    .destroy = destroy,
    .import_memory = NULL,
    .release_memory = NULL,
};

__attribute__((visibility("default"))) const struct venc_backend_ops *venc_backend_get(void) { return &ops; }

/* SPDX-License-Identifier: Apache-2.0
 * VideoToolbox side of the guest's Vulkan Video encode layer (guest/venc: layer + backend "vt", venc_proto.h).
 *
 * SteamVR's Steam Frame streaming driver (vrlink) encodes with Vulkan Video inside the VM; the guest layer gives
 * Venus a video-encode queue and sends every picture here. Pictures and bitstreams stay in guest memory that the
 * guest imported into its wmtr context: Venus host-visible memory is POSIX shm on this host, attached to the
 * executor by virtio-gpu resource id (wmtr_vr_shm.h). A picture is copied once, into an IOSurface-backed pixel
 * buffer of the compression session's pool; the bitstream is written straight into the guest's staging memory.
 *
 * One synchronous message per call (WMTR_CTL_VR_QUERY, routed here from wmtr_vr_query): the encode of a picture
 * completes before the reply (RealTime, no frame reordering, VTCompressionSessionCompleteFrames). Sessions are
 * process-wide and live until the guest destroys them or the executor exits. */
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "steamac_vr_guest.h"
#include "venc_proto.h"
#include "wmtr_venc.h"
#include "wmtr_vr_shm.h"

_Static_assert(SVR_VENC == VENC_PROTO_OP, "guest VR protocol op of the encoder");

#define VENC_SESSIONS 32

struct vt_session {
  VTCompressionSessionRef vt;
  struct venc_config cfg;
  uint32_t codec;
  uint8_t headers[VENC_PROTO_DATA];
  size_t headers_len;
  int force_next_idr; /* after a probe picture made for the headers */
  uint64_t frames;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vt_session *g_sessions[VENC_SESSIONS];

static CMVideoCodecType
codec_type(uint32_t codec) {
  return codec == VENC_H265 ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
}

static OSType
pixel_format(const struct venc_config *c) {
  return c->bit_depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
}

static void
set_num(VTCompressionSessionRef s, CFStringRef key, double v) {
  CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &v);
  VTSessionSetProperty(s, key, n);
  CFRelease(n);
}

static void
apply_rate_control(VTCompressionSessionRef s, const struct venc_config *c) {
  double fps = c->fps_den ? (double)c->fps_num / c->fps_den : 90.0;
  set_num(s, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
  switch (c->rc) {
  case VENC_RC_CQP: {
    double qp = c->qp ? c->qp : 26;
    set_num(s, kVTCompressionPropertyKey_MinAllowedFrameQP, qp);
    set_num(s, kVTCompressionPropertyKey_MaxAllowedFrameQP, qp);
    break;
  }
  case VENC_RC_CBR:
    if (c->avg_bitrate) {
      OSStatus st = -1;
      if (@available(macOS 13.0, *)) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, &c->avg_bitrate);
        st = VTSessionSetProperty(s, kVTCompressionPropertyKey_ConstantBitRate, n);
        CFRelease(n);
      }
      if (st != noErr) set_num(s, kVTCompressionPropertyKey_AverageBitRate, (double)c->avg_bitrate);
    }
    break;
  case VENC_RC_VBR:
  default:
    if (c->avg_bitrate) {
      set_num(s, kVTCompressionPropertyKey_AverageBitRate, (double)c->avg_bitrate);
      uint64_t maxr = c->max_bitrate > c->avg_bitrate ? c->max_bitrate : c->avg_bitrate;
      double window = c->vbv_ms ? c->vbv_ms / 1000.0 : 1.0;
      NSArray *limits = @[ @((double)maxr / 8.0 * window), @(window) ]; /* bytes per window, seconds */
      VTSessionSetProperty(s, kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)limits);
    }
    break;
  }
}

static void
session_free(struct vt_session *s) {
  if (!s) return;
  if (s->vt) {
    VTCompressionSessionInvalidate(s->vt);
    CFRelease(s->vt);
  }
  free(s);
}

static int
session_create(const struct venc_config *c, struct vt_session **out) {
  struct vt_session *s = calloc(1, sizeof *s);
  if (!s) return -ENOMEM;
  s->cfg = *c;
  s->codec = c->codec;
  NSDictionary *src = @{
    (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey : @(pixel_format(c)),
    (__bridge NSString *)kCVPixelBufferWidthKey : @(c->width),
    (__bridge NSString *)kCVPixelBufferHeightKey : @(c->height),
    (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  /* low-latency rate control first (needs a bitrate); not offered for every codec and size: then without it */
  OSStatus st = -1;
  for (int low_latency = c->rc == VENC_RC_CBR || c->rc == VENC_RC_VBR; low_latency >= 0 && st != noErr; low_latency--) {
    NSDictionary *spec = @{
      (__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES,
      (__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES,
      (__bridge NSString *)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @(low_latency != 0),
    };
    st = VTCompressionSessionCreate(NULL, (int32_t)c->width, (int32_t)c->height, codec_type(c->codec),
                                    (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)src, NULL, NULL, NULL,
                                    &s->vt);
  }
  if (st != noErr) {
    NSLog(@"wmtr venc: VTCompressionSessionCreate %ux%u codec %u: %d", c->width, c->height, c->codec, (int)st);
    free(s);
    return -ENOTSUP;
  }
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
  set_num(s->vt, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);
  set_num(s->vt, kVTCompressionPropertyKey_MaxKeyFrameInterval, (double)INT32_MAX); /* IDR only when asked */
  set_num(s->vt, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 0);
  CFStringRef profile;
  if (c->codec == VENC_H265) {
    profile = c->bit_depth == 10 ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel;
  } else {
    profile = c->profile_idc == 66 ? kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel
            : c->profile_idc == 77 ? kVTProfileLevel_H264_Main_AutoLevel
                                   : kVTProfileLevel_H264_High_AutoLevel;
    VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_H264EntropyMode,
                         c->cabac && c->profile_idc != 66 ? kVTH264EntropyMode_CABAC : kVTH264EntropyMode_CAVLC);
  }
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_ProfileLevel, profile);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
  apply_rate_control(s->vt, c);
  VTCompressionSessionPrepareToEncodeFrames(s->vt);
  *out = s;
  return 0;
}

/* Annex B parameter sets of a format description: H.264 SPS PPS, HEVC VPS SPS PPS. Returns bytes or -errno. */
static long
parameter_sets(CMFormatDescriptionRef fd, uint32_t codec, uint8_t *out, size_t cap, int *nal_len) {
  size_t count = 0, at = 0;
  int hl = 4;
  OSStatus st = codec == VENC_H265
                    ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, NULL, NULL, &count, &hl)
                    : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, NULL, NULL, &count, &hl);
  if (st != noErr) return -EIO;
  if (nal_len) *nal_len = hl;
  for (size_t i = 0; i < count; i++) {
    const uint8_t *p = NULL;
    size_t n = 0;
    st = codec == VENC_H265 ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, i, &p, &n, NULL, NULL)
                            : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &p, &n, NULL, NULL);
    if (st != noErr) return -EIO;
    if (at + 4 + n > cap) return -ENOSPC;
    out[at] = 0, out[at + 1] = 0, out[at + 2] = 0, out[at + 3] = 1;
    memcpy(out + at + 4, p, n);
    at += 4 + n;
  }
  return (long)at;
}

static int
keep_slices(uint32_t codec, uint32_t t) {
  return venc_nal_is_slice(codec, t);
}

/* Encodes one picture (planes already in `pb`); the slices go to out (Annex B), the parameter sets to the
 * session's cache. Returns bytes or -errno. */
static long
encode_pb(struct vt_session *s, CVPixelBufferRef pb, uint64_t pts, int force_idr, uint8_t *out, size_t cap,
          int *is_idr) {
  __block CMSampleBufferRef got = NULL;
  __block OSStatus cb_status = noErr;
  NSDictionary *props = force_idr ? @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES} : nil;
  double fps = s->cfg.fps_den ? (double)s->cfg.fps_num / s->cfg.fps_den : 90.0;
  CMTime t = CMTimeMake((int64_t)pts * 1000, (int32_t)(fps * 1000 + 0.5));
  OSStatus st = VTCompressionSessionEncodeFrameWithOutputHandler(
      s->vt, pb, t, kCMTimeInvalid, (__bridge CFDictionaryRef)props, NULL,
      ^(OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
        (void)flags;
        cb_status = status;
        if (sb && !got) got = (CMSampleBufferRef)CFRetain(sb);
      });
  if (st == noErr) st = VTCompressionSessionCompleteFrames(s->vt, kCMTimeInvalid);
  if (st != noErr || cb_status != noErr || !got) {
    if (got) CFRelease(got);
    return -EIO;
  }
  long r;
  CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(got);
  int nal_len = 4;
  long hn = fd ? parameter_sets(fd, s->codec, s->headers, sizeof s->headers, &nal_len) : -EIO;
  if (hn > 0) s->headers_len = (size_t)hn;
  CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(got, false);
  int sync = 1;
  if (att && CFArrayGetCount(att) > 0) {
    CFDictionaryRef a = CFArrayGetValueAtIndex(att, 0);
    sync = !CFDictionaryContainsKey(a, kCMSampleAttachmentKey_NotSync);
  }
  *is_idr = sync;
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(got);
  size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
  uint8_t *tmp = len ? malloc(len) : NULL;
  if (!tmp || CMBlockBufferCopyDataBytes(bb, 0, len, tmp) != kCMBlockBufferNoErr) {
    r = -EIO;
  } else {
    r = venc_nal_filter(s->codec, tmp, len, nal_len, out, cap, keep_slices);
  }
  free(tmp);
  CFRelease(got);
  s->frames++;
  return r;
}

static CVPixelBufferRef
pool_buffer(struct vt_session *s) {
  CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(s->vt);
  CVPixelBufferRef pb = NULL;
  if (!pool || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess) return NULL;
  return pb;
}

/* rows of one plane into a locked pixel buffer plane */
static void
copy_plane(CVPixelBufferRef pb, size_t plane, const uint8_t *src, size_t src_stride, size_t row_bytes, size_t rows) {
  uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(pb, plane);
  size_t ds = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
  size_t h = CVPixelBufferGetHeightOfPlane(pb, plane);
  if (rows > h) rows = h;
  if (row_bytes > ds) row_bytes = ds;
  for (size_t y = 0; y < rows; y++) memcpy(dst + y * ds, src + y * src_stride, row_bytes);
}

/* a picture made for the parameter sets when the guest asks before encoding anything */
static int
probe_headers(struct vt_session *s) {
  CVPixelBufferRef pb = pool_buffer(s);
  if (!pb) return -ENOMEM;
  CVPixelBufferLockBaseAddress(pb, 0);
  for (size_t p = 0; p < 2; p++) {
    uint8_t *d = CVPixelBufferGetBaseAddressOfPlane(pb, p);
    memset(d, p ? 0x80 : 0x10, CVPixelBufferGetBytesPerRowOfPlane(pb, p) * CVPixelBufferGetHeightOfPlane(pb, p));
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
  uint8_t *scratch = malloc(8 << 20);
  int idr = 0;
  long r = scratch ? encode_pb(s, pb, s->frames, 1, scratch, 8 << 20, &idr) : -ENOMEM;
  free(scratch);
  CVPixelBufferRelease(pb);
  s->force_next_idr = 1;
  return r < 0 ? (int)r : (s->headers_len ? 0 : -EIO);
}

static struct vt_session *
session_get(uint32_t id) {
  return id && id < VENC_SESSIONS ? g_sessions[id] : NULL;
}

int
wmtr_venc_query(void *buf, uint32_t size) {
  if (size < sizeof(struct venc_msg)) return -EINVAL;
  struct venc_msg *m = buf;
  if (m->version != VENC_PROTO_VERSION) {
    m->status = -EPROTO;
    return 0;
  }
  @autoreleasepool {
    pthread_mutex_lock(&g_lock);
    struct vt_session *s = session_get(m->session);
    int r = 0;
    switch (m->cmd) {
    case VENC_CMD_QUERY: {
      memset(&m->limits, 0, sizeof m->limits);
      struct venc_config c = {.codec = m->codec, .width = 256, .height = 256, .bit_depth = 8, .profile_idc = 0,
                              .cabac = 1, .fps_num = 90, .fps_den = 1};
      struct vt_session *t = NULL;
      if (m->codec != VENC_H264 && m->codec != VENC_H265) {
        r = -ENOTSUP;
      } else if ((r = session_create(&c, &t)) == 0) {
        session_free(t);
        m->limits.min_width = m->limits.min_height = 64;
        /* Apple silicon media engines: H.264 to 4096 wide, HEVC to 8192 (sessions past a chip's limit fail at
         * creation, and the guest then reports the error per session) */
        m->limits.max_width = m->limits.max_height = m->codec == VENC_H265 ? 8192 : 4096;
        m->limits.max_bitrate = 1000000000ull;
        m->limits.max_level_idc = m->codec == VENC_H265 ? 186 : 52;
        struct vt_session *t10 = NULL;
        struct venc_config c10 = c;
        c10.bit_depth = 10;
        m->limits.ten_bit = m->codec == VENC_H265 && session_create(&c10, &t10) == 0;
        session_free(t10);
      }
      break;
    }
    case VENC_CMD_CREATE: {
      uint32_t id = 1;
      while (id < VENC_SESSIONS && g_sessions[id]) id++;
      if (id == VENC_SESSIONS) {
        r = -EMFILE;
        break;
      }
      struct vt_session *n = NULL;
      r = session_create(&m->config, &n);
      if (r == 0) {
        g_sessions[id] = n;
        m->session = id;
      }
      break;
    }
    case VENC_CMD_HEADERS:
      if (!s) {
        r = -EINVAL;
        break;
      }
      if (!s->headers_len) r = probe_headers(s);
      if (r == 0) {
        memcpy(m->data, s->headers, s->headers_len);
        m->bytes = s->headers_len;
      }
      break;
    case VENC_CMD_ENCODE: {
      if (!s) {
        r = -EINVAL;
        break;
      }
      uint64_t in_len = 0, out_len = 0;
      const uint8_t *in = wmtr_shm_acquire(m->in_res, &in_len);
      uint8_t *out = wmtr_shm_acquire_rw(m->out_res, &out_len);
      uint32_t bpe = s->cfg.bit_depth == 10 ? 2 : 1;
      uint64_t luma_bytes = (uint64_t)m->luma_stride * m->height;
      uint64_t chroma_bytes = (uint64_t)m->chroma_stride * ((m->height + 1) / 2);
      if (!in || !out) {
        r = -ENOENT;
      } else if (m->luma_offset > in_len || luma_bytes > in_len - m->luma_offset || m->chroma_offset > in_len ||
                 chroma_bytes > in_len - m->chroma_offset || m->out_offset > out_len ||
                 m->out_capacity > out_len - m->out_offset || m->width * bpe > m->luma_stride) {
        r = -EFAULT;
      } else {
        CVPixelBufferRef pb = pool_buffer(s);
        if (!pb) {
          r = -ENOMEM;
        } else {
          CVPixelBufferLockBaseAddress(pb, 0);
          copy_plane(pb, 0, in + m->luma_offset, m->luma_stride, (size_t)m->width * bpe, m->height);
          copy_plane(pb, 1, in + m->chroma_offset, m->chroma_stride, (size_t)((m->width + 1) / 2) * 2 * bpe,
                     (m->height + 1) / 2);
          CVPixelBufferUnlockBaseAddress(pb, 0);
          int idr = 0;
          long n = encode_pb(s, pb, m->pts, m->force_idr || s->force_next_idr, out + m->out_offset,
                             (size_t)m->out_capacity, &idr);
          CVPixelBufferRelease(pb);
          if (n < 0) {
            r = (int)n;
          } else {
            s->force_next_idr = 0;
            m->bytes = (uint64_t)n;
            m->is_idr = (uint32_t)idr;
          }
        }
      }
      if (in) wmtr_shm_release(m->in_res);
      if (out) wmtr_shm_release(m->out_res);
      break;
    }
    case VENC_CMD_RECONFIG:
      if (!s) {
        r = -EINVAL;
        break;
      }
      s->cfg.rc = m->config.rc;
      s->cfg.avg_bitrate = m->config.avg_bitrate;
      s->cfg.max_bitrate = m->config.max_bitrate;
      s->cfg.vbv_ms = m->config.vbv_ms;
      s->cfg.qp = m->config.qp;
      apply_rate_control(s->vt, &s->cfg);
      break;
    case VENC_CMD_DESTROY:
      if (s) {
        g_sessions[m->session] = NULL;
        session_free(s);
      }
      break;
    default:
      r = -ENOSYS;
    }
    pthread_mutex_unlock(&g_lock);
    m->status = r;
  }
  return 0;
}

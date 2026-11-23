/* SPDX-License-Identifier: Apache-2.0
 * wmtr_venc.m against VideoToolbox: the guest encode layer's messages (venc_proto.h) with pictures and bitstreams
 * in shared memory attached by resource id, like the VM's attach hook does.
 *
 *   wmtr_venc_test [out-dir]    writes <out-dir>/venc_{h264,h265}.{264,265} (parameter sets + every picture)
 *
 * Checks: QUERY limits, CREATE, HEADERS before the first picture (a probe encode), the first picture an IDR, later
 * ones not, a forced IDR when asked, RECONFIG, Annex B slices only (no parameter sets or SEI in the pictures),
 * -EFAULT for a picture outside its memory and -ENOENT for an unattached resource. Without a hardware encoder
 * (virtual Macs) the codec is reported as skipped when WMTR_VENC_ALLOW_SKIP=1, else that is a failure. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "venc_proto.h"
#include "wmtr_venc.h"
#include "wmtr_vr_shm.h"

static int failures;
#define CHECK(c, ...)                                                                                                  \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
      fprintf(stderr, __VA_ARGS__);                                                                                    \
      fprintf(stderr, "\n");                                                                                           \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static void *
shm_region(uint32_t res, size_t size) {
  char name[64];
  snprintf(name, sizeof name, "/wmtr_venc_test.%d.%u", (int)getpid(), res);
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return NULL;
  shm_unlink(name);
  if (ftruncate(fd, (off_t)size) != 0) return NULL;
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  int dupfd = dup(fd);
  close(fd);
  if (p == MAP_FAILED || wmtr_shm_attach(res, dupfd, size) != 0) return NULL;
  return p;
}

static void
msg_init(struct venc_msg *m, uint32_t cmd, uint32_t session) {
  memset(m, 0, sizeof *m);
  m->op = VENC_PROTO_OP;
  m->size = sizeof *m;
  m->version = VENC_PROTO_VERSION;
  m->cmd = cmd;
  m->session = session;
}

/* start code + NAL type of every NAL unit in an Annex B buffer; returns the count */
static int
nal_types(uint32_t codec, const uint8_t *p, size_t n, int *types, int max) {
  int count = 0;
  for (size_t i = 0; i + 3 < n && count < max; i++) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      uint8_t h = p[i + 3];
      types[count++] = codec == VENC_H265 ? (h >> 1) & 0x3f : h & 0x1f;
      i += 2;
    }
  }
  return count;
}

static int
is_slice(uint32_t codec, int t) {
  return codec == VENC_H265 ? t <= 21 : (t >= 1 && t <= 5);
}

static void
run_codec(uint32_t codec, const char *dir) {
  const char *name = codec == VENC_H265 ? "h265" : "h264";
  const uint32_t W = 1920, H = 1080;
  struct venc_msg m;

  msg_init(&m, VENC_CMD_QUERY, 0);
  m.codec = codec;
  CHECK(wmtr_venc_query(&m, sizeof m) == 0, "query call");
  if (m.status == -ENOTSUP && getenv("WMTR_VENC_ALLOW_SKIP")) {
    printf("%s: SKIP (no hardware encoder on this Mac)\n", name);
    return;
  }
  CHECK(m.status == 0, "%s query: %d", name, m.status);
  if (m.status) return;
  CHECK(m.limits.max_width >= 3840 && m.limits.min_width > 0, "%s limits %u..%u", name, m.limits.min_width,
        m.limits.max_width);
  printf("%s: max %ux%u, level %u, 10-bit %u\n", name, m.limits.max_width, m.limits.max_height,
         m.limits.max_level_idc, m.limits.ten_bit);

  msg_init(&m, VENC_CMD_CREATE, 0);
  m.config = (struct venc_config){.codec = codec, .width = W, .height = H, .bit_depth = 8, .cabac = 1,
                                  .transform_8x8 = 1, .fps_num = 90, .fps_den = 1, .rc = VENC_RC_CBR,
                                  .avg_bitrate = 60000000, .max_bitrate = 60000000};
  wmtr_venc_query(&m, sizeof m);
  CHECK(m.status == 0 && m.session, "%s create: %d", name, m.status);
  if (m.status) return;
  uint32_t session = m.session;

  char path[1024];
  snprintf(path, sizeof path, "%s/venc_%s.%s", dir, name, codec == VENC_H265 ? "265" : "264");
  FILE *f = fopen(path, "wb");
  CHECK(f != NULL, "open %s", path);

  msg_init(&m, VENC_CMD_HEADERS, session);
  wmtr_venc_query(&m, sizeof m);
  CHECK(m.status == 0 && m.bytes > 8, "%s headers: %d, %llu bytes", name, m.status, (unsigned long long)m.bytes);
  int types[64];
  int n = nal_types(codec, m.data, m.bytes, types, 64);
  CHECK(n == (codec == VENC_H265 ? 3 : 2), "%s headers: %d NAL units", name, n);
  if (f) fwrite(m.data, 1, m.bytes, f);

  uint32_t in_res = 0x7001 + codec * 2, out_res = in_res + 1;
  size_t luma = (size_t)W * H, in_size = luma + luma / 2 + 4096, out_size = 8u << 20;
  uint8_t *in = shm_region(in_res, in_size);
  uint8_t *out = shm_region(out_res, out_size);
  CHECK(in && out, "shared memory");
  if (!in || !out) return;

  size_t total = 0;
  const int frames = 90;
  for (int i = 0; i < frames; i++) {
    /* a moving gradient with a bar: content that changes but predicts well */
    for (uint32_t y = 0; y < H; y++)
      for (uint32_t x = 0; x < W; x++)
        in[4096 + (size_t)y * W + x] = (uint8_t)(16 + ((x + y + i * 8) & 0x7f) + ((x / 64 + i) % 30 == 0 ? 80 : 0));
    for (uint32_t y = 0; y < H / 2; y++)
      for (uint32_t x = 0; x < W; x += 2) {
        in[4096 + luma + (size_t)y * W + x] = (uint8_t)(96 + ((y + i) & 63));
        in[4096 + luma + (size_t)y * W + x + 1] = (uint8_t)(160 - ((x / 2 + i) & 63));
      }
    if (i == 45) {
      msg_init(&m, VENC_CMD_RECONFIG, session);
      m.config = (struct venc_config){.codec = codec, .width = W, .height = H, .fps_num = 90, .fps_den = 1,
                                      .rc = VENC_RC_VBR, .avg_bitrate = 30000000, .max_bitrate = 45000000};
      wmtr_venc_query(&m, sizeof m);
      CHECK(m.status == 0, "%s reconfig: %d", name, m.status);
    }
    msg_init(&m, VENC_CMD_ENCODE, session);
    m.in_res = in_res, m.out_res = out_res;
    m.luma_offset = 4096, m.chroma_offset = 4096 + luma;
    m.luma_stride = m.chroma_stride = W;
    m.width = W, m.height = H;
    m.out_offset = 256, m.out_capacity = out_size - 256;
    m.force_idr = i == 60;
    m.pts = (uint64_t)i;
    wmtr_venc_query(&m, sizeof m);
    CHECK(m.status == 0 && m.bytes > 0, "%s frame %d: %d", name, i, m.status);
    if (m.status) break;
    int want_idr = i == 0 || i == 60;
    CHECK((int)m.is_idr == want_idr, "%s frame %d: idr %u", name, i, m.is_idr);
    n = nal_types(codec, out + 256, m.bytes, types, 64);
    CHECK(n > 0, "%s frame %d: no NAL units", name, i);
    for (int k = 0; k < n; k++) CHECK(is_slice(codec, types[k]), "%s frame %d: NAL type %d", name, i, types[k]);
    if (want_idr) {
      int idr_type = codec == VENC_H265 ? (types[0] == 19 || types[0] == 20) : types[0] == 5;
      CHECK(idr_type, "%s frame %d: first NAL type %d is not IDR", name, i, types[0]);
    }
    if (f) fwrite(out + 256, 1, m.bytes, f);
    total += m.bytes;
  }
  printf("%s: %d frames 1080p, %.1f KiB per frame -> %s\n", name, frames, total / 1024.0 / frames, path);
  if (f) fclose(f);

  /* a picture past the end of its memory, and memory that is not attached */
  msg_init(&m, VENC_CMD_ENCODE, session);
  m.in_res = in_res, m.out_res = out_res;
  m.luma_offset = in_size - 100, m.chroma_offset = 4096 + luma;
  m.luma_stride = m.chroma_stride = W;
  m.width = W, m.height = H;
  m.out_capacity = out_size;
  wmtr_venc_query(&m, sizeof m);
  CHECK(m.status == -EFAULT, "%s out-of-range picture: %d", name, m.status);
  m.status = 0;
  m.in_res = 0x6fff;
  m.luma_offset = 4096;
  wmtr_venc_query(&m, sizeof m);
  CHECK(m.status == -ENOENT, "%s unattached memory: %d", name, m.status);

  msg_init(&m, VENC_CMD_DESTROY, session);
  wmtr_venc_query(&m, sizeof m);
  CHECK(m.status == 0, "%s destroy: %d", name, m.status);
  wmtr_shm_detach(in_res);
  wmtr_shm_detach(out_res);
  munmap(in, in_size);
  munmap(out, out_size);
}

int
main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  struct venc_msg m;
  msg_init(&m, VENC_CMD_QUERY, 0);
  m.version = 99;
  wmtr_venc_query(&m, sizeof m);
  CHECK(m.status == -EPROTO, "wrong version: %d", m.status);
  CHECK(wmtr_venc_query(&m, 16) == -EINVAL, "short buffer");
  msg_init(&m, VENC_CMD_HEADERS, 5);
  wmtr_venc_query(&m, sizeof m);
  CHECK(m.status == -EINVAL, "unknown session: %d", m.status);

  run_codec(VENC_H264, dir);
  run_codec(VENC_H265, dir);
  if (failures) {
    fprintf(stderr, "wmtr_venc_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("wmtr_venc_test: ok\n");
  return 0;
}

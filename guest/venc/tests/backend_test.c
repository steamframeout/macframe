/* Encodes a moving NV12 test pattern with a backend (headers + slices) into an Annex B file.
 *   backend_test <backend.so> <h264|h265> <w> <h> <frames> <out> [idr_every]   */
#include "venc_backend.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    const struct venc_backend_ops *ops = ((venc_backend_get_fn)dlsym(h, "venc_backend_get"))();
    struct venc_config c = {0};
    c.codec = strcmp(argv[2], "h265") ? VENC_H264 : VENC_H265;
    c.width = atoi(argv[3]); c.height = atoi(argv[4]);
    c.profile_idc = c.codec == VENC_H264 ? 100 : 1; c.bit_depth = 8; c.cabac = 1; c.fps_num = 30; c.fps_den = 1;
    c.rc = VENC_RC_CQP; c.qp = 22;
    int frames = atoi(argv[5]), idr_every = argc > 7 ? atoi(argv[7]) : 0;
    void *e = ops->create(&c);
    if (!e) { fprintf(stderr, "create failed\n"); return 1; }
    FILE *f = fopen(argv[6], "wb");
    uint8_t hdr[4096]; size_t hn = sizeof(hdr);
    if (ops->headers(e, hdr, &hn)) { fprintf(stderr, "headers failed\n"); return 1; }
    fwrite(hdr, 1, hn, f);
    uint32_t w = c.width, ht = c.height;
    uint8_t *y = malloc(w * ht), *uv = malloc(w * ((ht + 1) / 2));
    uint8_t *out = malloc(8 << 20);
    for (int i = 0; i < frames; i++) {
        for (uint32_t r = 0; r < ht; r++) for (uint32_t x = 0; x < w; x++) y[r * w + x] = (uint8_t)((x + r + 4 * i) & 0xff);
        for (uint32_t r = 0; r < (ht + 1) / 2; r++) for (uint32_t x = 0; x < w; x++) uv[r * w + x] = (uint8_t)(x & 1 ? 128 + (r & 63) : 128 + ((x / 2 + i) & 63));
        struct venc_picture p = {.luma = y, .chroma = uv, .luma_stride = w, .chroma_stride = w, .width = w, .height = ht,
                                 .force_idr = idr_every && i % idr_every == 0, .pts = (uint64_t)i};
        int idr;
        long n = ops->encode(e, &p, out, 8 << 20, &idr);
        if (n < 0) { fprintf(stderr, "encode failed %ld\n", n); return 1; }
        if (i == 0 && !idr) { fprintf(stderr, "first picture is not IDR\n"); return 1; }
        if (idr && i > 0) fwrite(hdr, 1, hn, f);   /* repeat parameter sets before every IDR */
        fwrite(out, 1, (size_t)n, f);
    }
    fclose(f);
    ops->destroy(e);
    printf("ok %d frames\n", frames);
    return 0;
}

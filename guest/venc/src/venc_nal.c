/* NAL unit helpers shared by the encoder backends. */
#include "venc_backend.h"

#include <errno.h>
#include <string.h>

uint32_t venc_nal_type(uint32_t codec, uint8_t b)
{
    return codec == VENC_H265 ? (uint32_t)(b >> 1) & 0x3f : (uint32_t)b & 0x1f;
}

int venc_nal_is_slice(uint32_t codec, uint32_t t)
{
    if (codec == VENC_H265)
        return t <= 21;          /* VCL: TRAIL..CRA, reserved IRAP */
    return t >= 1 && t <= 5;     /* non-IDR, partitions A-C, IDR */
}

int venc_nal_is_param_set(uint32_t codec, uint32_t t)
{
    if (codec == VENC_H265)
        return t == 32 || t == 33 || t == 34;   /* VPS SPS PPS */
    return t == 7 || t == 8;                     /* SPS PPS */
}

static long put(uint8_t *out, size_t out_size, size_t at, const uint8_t *nal, size_t n)
{
    if (at + 4 + n > out_size)
        return -ENOSPC;
    out[at] = 0; out[at + 1] = 0; out[at + 2] = 0; out[at + 3] = 1;
    memcpy(out + at + 4, nal, n);
    return (long)(at + 4 + n);
}

long venc_nal_filter(uint32_t codec, const uint8_t *in, size_t in_size, int length_size,
                     uint8_t *out, size_t out_size, int (*keep)(uint32_t codec, uint32_t type))
{
    size_t at = 0, i = 0;
    if (length_size > 0) {
        while (i + (size_t)length_size <= in_size) {
            size_t n = 0;
            for (int k = 0; k < length_size; k++)
                n = (n << 8) | in[i + k];
            i += (size_t)length_size;
            if (n == 0 || n > in_size - i)
                break;
            if (keep(codec, venc_nal_type(codec, in[i]))) {
                long r = put(out, out_size, at, in + i, n);
                if (r < 0)
                    return r;
                at = (size_t)r;
            }
            i += n;
        }
        return (long)at;
    }
    /* Annex B: find start codes (00 00 01, optionally preceded by a zero byte) */
    size_t start = (size_t)-1;
    while (i + 3 <= in_size) {
        if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) {
            if (start != (size_t)-1) {
                size_t end = i;
                while (end > start && in[end - 1] == 0)   /* trailing zero / next 4-byte start code */
                    end--;
                if (end > start && keep(codec, venc_nal_type(codec, in[start]))) {
                    long r = put(out, out_size, at, in + start, end - start);
                    if (r < 0)
                        return r;
                    at = (size_t)r;
                }
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != (size_t)-1 && start < in_size && keep(codec, venc_nal_type(codec, in[start]))) {
        long r = put(out, out_size, at, in + start, in_size - start);
        if (r < 0)
            return r;
        at = (size_t)r;
    }
    return (long)at;
}

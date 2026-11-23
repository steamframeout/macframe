/* SPDX-License-Identifier: Apache-2.0
 * VideoToolbox encoder for the guest's Vulkan Video encode layer (wmtr_venc.m, protocol guest/venc/include/venc_proto.h). */
#ifndef WMTR_VENC_H
#define WMTR_VENC_H
#include <stdint.h>

/* one SVR_VENC message (struct venc_msg) in `buf`, overwritten with the reply. 0, or -EINVAL for a short buffer. */
int wmtr_venc_query(void *buf, uint32_t size);

#endif

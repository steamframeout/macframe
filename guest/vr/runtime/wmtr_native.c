/* SPDX-License-Identifier: Apache-2.0
 * Native wmtr link of the guest VR runtime: see wmtr_native.h. The record format and ring discipline are the wmtr
 * client's (wmtr/src/wmtr_client.c send_built_flags) for a control request without relocations or outputs. */
#include "wmtr_native.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

#include "wmtr_proto.h"
#include "wmtr_spin.h"
#include "wmtr_transport.h"
#include "wmtr_wire.h"

/* one reply slot: the runtime's VR queries are serialized (the transport is called under one lock) */
#define NATIVE_SLOT 0u

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct wmtr_transport *T;
static struct wmtr_ring *R;
static uint8_t *ring_data;
static struct wmtr_slot *slots;
static int state; /* 0 = not tried, 1 = connected, -errno = failed (not retried) */

int
svr_wmtr_native_open(void) {
  pthread_mutex_lock(&lock);
  if (!state) {
    T = wmtr_transport_create();
    uint8_t *base = T ? T->open_ring(T, WMTR_RING_BLOB_BYTES) : NULL;
    if (!base) {
      state = -ENODEV;
    } else {
      R = (struct wmtr_ring *)base;
      ring_data = base + WMTR_RING_DATA_OFFSET;
      slots = (struct wmtr_slot *)(base + WMTR_SLOTS_OFFSET);
      state = 1;
    }
  }
  int r = state > 0 ? 0 : state;
  pthread_mutex_unlock(&lock);
  return r;
}

static uint64_t
ring_space(void) {
  return WMTR_RING_BYTES - (R->head - __atomic_load_n(&R->tail, __ATOMIC_ACQUIRE));
}

int
svr_wmtr_native_call(const void *req, uint32_t req_size, void *rep, uint32_t rep_cap) {
  if (svr_wmtr_native_open()) return -ENODEV;
  /* the executor overwrites the params with the reply: params_size covers both */
  uint32_t psize = WMTR_ALIGN8(req_size > rep_cap ? req_size : rep_cap);
  if (!req || !rep || req_size < 8 || psize > WMTR_SLOT_BYTES || psize > 4096) return -EINVAL;
  struct wmtr_req_ext hdr;
  memset(&hdr, 0, sizeof hdr);
  hdr.h.call = WMTR_CTL_VR_QUERY;
  hdr.h.flags = WMTR_F_SYNC | WMTR_F_CONTROL;
  hdr.h.slot = NATIVE_SLOT;
  hdr.h.params_size = psize;
  hdr.payload_size = psize;
  uint32_t poff = wmtr_req_payload_offset(&hdr);
  uint32_t total = WMTR_ALIGN8(poff + psize);
  hdr.h.size = total;

  pthread_mutex_lock(&lock);
  struct wmtr_slot *s = &slots[NATIVE_SLOT];
  uint64_t seq = s->done_seq + 1;
  hdr.h.seq = seq;
  uint64_t head = R->head;
  uint32_t pos = (uint32_t)(head % WMTR_RING_BYTES);
  uint32_t to_end = WMTR_RING_BYTES - pos;
  uint64_t need = total + (to_end < total ? to_end : 0);
  for (int spins = 0; ring_space() < need; spins++) {
    if (wmtr_claim_kick(&R->host_idle)) T->kick(T);
    if (spins > 64) sched_yield();
  }
  if (to_end < total) {
    if (to_end >= sizeof(struct wmtr_req)) {
      struct wmtr_req w;
      memset(&w, 0, sizeof w);
      w.size = to_end;
      w.flags = WMTR_F_WRAP;
      memcpy(ring_data + pos, &w, sizeof w);
    }
    head += to_end;
    pos = 0;
  }
  uint8_t *dst = ring_data + pos;
  memcpy(dst, &hdr, sizeof hdr);
  memset(dst + poff, 0, psize);
  memcpy(dst + poff, req, req_size);
  __atomic_store_n(&R->head, head + total, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (wmtr_claim_kick(&R->host_idle)) T->kick(T);

  if (__atomic_load_n(&s->done_seq, __ATOMIC_ACQUIRE) < seq) {
    __atomic_store_n(&s->waiting, 1, __ATOMIC_SEQ_CST);
    T->wait(T, &s->done_seq, seq, NATIVE_SLOT, wmtr_spin_budget_ns(1 /* WMTR_CLS_S */));
    __atomic_store_n(&s->waiting, 0, __ATOMIC_RELAXED);
  }
  int r = 0;
  if (s->status) {
    r = -EIO;
  } else {
    uint32_t got = ((const uint32_t *)(const void *)s->data)[1]; /* reply header: op, size */
    if (got < 8 || got > rep_cap) r = -EPROTO;
    else memcpy(rep, (const void *)s->data, got);
  }
  pthread_mutex_unlock(&lock);
  return r;
}

int
svr_wmtr_native_import_fd(int fd, uint32_t *bo, uint32_t *res_id) {
  if (svr_wmtr_native_open()) return -ENODEV;
  if (!T->import_fd) return -ENOTSUP;
  pthread_mutex_lock(&lock);
  int r = T->import_fd(T, fd, bo, res_id);
  pthread_mutex_unlock(&lock);
  return r;
}

void
svr_wmtr_native_close_bo(uint32_t bo) {
  if (state <= 0 || !T->close_bo) return;
  pthread_mutex_lock(&lock);
  T->close_bo(T, bo);
  pthread_mutex_unlock(&lock);
}

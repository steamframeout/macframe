/* Host memory of guest Vulkan eye buffers: see wmtr_vr_shm.h. */
#include "wmtr_vr_shm.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct shm_entry {
  uint32_t res_id;
  uint32_t attaches; /* attach hooks that saw it (one per wmtr context) */
  uint32_t uses;     /* registered eye buffers on it */
  void *addr;
  uint64_t len;
  int writable;      /* mapped read-write */
};

#define SHM_MAX 64 /* eye buffers: 2 eyes x 3 slots per game, plus re-allocations in flight */
static struct shm_entry tab[SHM_MAX];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct shm_entry *
find(uint32_t res_id) {
  for (int i = 0; i < SHM_MAX; i++)
    if (tab[i].addr && tab[i].res_id == res_id) return &tab[i];
  return NULL;
}

static void
drop_if_unused(struct shm_entry *e) {
  if (e->attaches || e->uses) return;
  munmap(e->addr, e->len);
  e->addr = NULL;
  e->len = 0;
  e->res_id = 0;
}

int
wmtr_shm_attach(uint32_t res_id, int fd, uint64_t size) {
  if (fd < 0) return -EBADF;
  if (!res_id || !size) { close(fd); return -EINVAL; }
  pthread_mutex_lock(&lock);
  struct shm_entry *e = find(res_id);
  if (e) { /* attached to another context too: one mapping */
    e->attaches++;
    pthread_mutex_unlock(&lock);
    close(fd);
    return 0;
  }
  for (int i = 0; i < SHM_MAX && !e; i++)
    if (!tab[i].addr) e = &tab[i];
  if (!e) { pthread_mutex_unlock(&lock); close(fd); return -ENOSPC; }
  /* whole host pages, never past the object (a short object would fault the GPU's reads) */
  struct stat st;
  uint64_t len = size;
  if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size < len) len = (uint64_t)st.st_size;
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) page = 16384;
  len &= ~(uint64_t)(page - 1);
  /* read-write when the fd allows it: Metal wraps the pages without a copy (newBufferWithBytesNoCopy) and wants
   * ordinary writable memory, though the executor only reads */
  void *a = len ? mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
  int writable = a != MAP_FAILED;
  if (a == MAP_FAILED && len) a = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (a == MAP_FAILED) { pthread_mutex_unlock(&lock); return -ENOMEM; }
  e->res_id = res_id;
  e->attaches = 1;
  e->uses = 0;
  e->addr = a;
  e->len = len;
  e->writable = writable;
  pthread_mutex_unlock(&lock);
  return 0;
}

void
wmtr_shm_detach(uint32_t res_id) {
  pthread_mutex_lock(&lock);
  struct shm_entry *e = find(res_id);
  if (e && e->attaches) {
    e->attaches--;
    drop_if_unused(e);
  }
  pthread_mutex_unlock(&lock);
}

const void *
wmtr_shm_acquire(uint32_t res_id, uint64_t *len) {
  pthread_mutex_lock(&lock);
  struct shm_entry *e = find(res_id);
  const void *a = NULL;
  if (e && e->attaches) {
    e->uses++;
    a = e->addr;
    if (len) *len = e->len;
  }
  pthread_mutex_unlock(&lock);
  return a;
}

void *
wmtr_shm_acquire_rw(uint32_t res_id, uint64_t *len) {
  pthread_mutex_lock(&lock);
  struct shm_entry *e = find(res_id);
  void *a = NULL;
  if (e && e->attaches && e->writable) {
    e->uses++;
    a = e->addr;
    if (len) *len = e->len;
  }
  pthread_mutex_unlock(&lock);
  return a;
}

void
wmtr_shm_release(uint32_t res_id) {
  pthread_mutex_lock(&lock);
  struct shm_entry *e = find(res_id);
  if (e && e->uses) {
    e->uses--;
    drop_if_unused(e);
  }
  pthread_mutex_unlock(&lock);
}

uint32_t
wmtr_shm_count(void) {
  uint32_t n = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < SHM_MAX; i++) n += tab[i].addr != NULL;
  pthread_mutex_unlock(&lock);
  return n;
}

int
wmtr_vr_buf_eye_check(const struct svr_buf_eye *e, uint64_t len, uint32_t *mtl_format, uint32_t *bpe) {
  uint32_t f;
  switch (e->vk_format) {
  case SVR_VK_FORMAT_R8G8B8A8_UNORM: f = WMTR_MTL_RGBA8; break;
  case SVR_VK_FORMAT_R8G8B8A8_SRGB: f = WMTR_MTL_RGBA8_SRGB; break;
  case SVR_VK_FORMAT_B8G8R8A8_UNORM: f = WMTR_MTL_BGRA8; break;
  case SVR_VK_FORMAT_B8G8R8A8_SRGB: f = WMTR_MTL_BGRA8_SRGB; break;
  case SVR_VK_FORMAT_R16G16B16A16_SFLOAT: f = WMTR_MTL_RGBA16F; break;
  default: return -ENOTSUP;
  }
  uint32_t b = svr_vk_format_bytes(e->vk_format);
  if (!e->width || !e->height || e->width > 16384 || e->height > 16384) return -EINVAL;
  /* Metal's buffer-to-texture copy: pitch and offset in whole texels, rows at least as wide as the rectangle */
  if (e->row_bytes % b || (uint64_t)e->row_bytes < (uint64_t)e->width * b || e->offset % b) return -EINVAL;
  uint64_t end = e->offset + (uint64_t)e->row_bytes * (e->height - 1) + (uint64_t)e->width * b;
  if (e->offset > len || end > len) return -EINVAL;
  *mtl_format = f;
  *bpe = b;
  return 0;
}

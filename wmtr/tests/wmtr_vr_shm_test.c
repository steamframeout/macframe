/* Tests of wmtr/src/wmtr_vr_shm.c (host memory of guest Vulkan eye buffers). Plain C: runs on macOS and Linux.
 *   cc -Iinclude -I../guest/vr/include tests/wmtr_vr_shm_test.c src/wmtr_vr_shm.c -o shm_test -lpthread && ./shm_test */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wmtr_vr_shm.h"

static int failures;
#define CHECK(c)                                                                \
  do {                                                                          \
    if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); failures++; } \
  } while (0)

/* a "Venus" shm object of `size` bytes filled with `byte`, and a writable mapping of it (the guest's view) */
static int
make_shm(uint64_t size, uint8_t byte, uint8_t **guest) {
  char name[64];
  snprintf(name, sizeof name, "/wmtr-shm-test.%d.%u", (int)getpid(), (unsigned)rand());
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  shm_unlink(name);
  if (fd < 0 || ftruncate(fd, (off_t)size)) { perror("shm"); exit(1); }
  *guest = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  memset(*guest, byte, size);
  return fd;
}

int
main(void) {
  long page = sysconf(_SC_PAGESIZE);
  uint64_t size = 4 * (uint64_t)page;
  uint8_t *guest;

  /* attach, use, guest writes are visible, mapping outlives the detach while used */
  int fd = make_shm(size, 0xab, &guest);
  CHECK(wmtr_shm_attach(7, fd, size) == 0);
  CHECK(wmtr_shm_count() == 1);
  uint64_t len = 0;
  const uint8_t *p = wmtr_shm_acquire(7, &len);
  CHECK(p && len == size && p[0] == 0xab && p[size - 1] == 0xab);
  guest[123] = 0x42; /* the GPU copy lands in the guest's buffer... */
  CHECK(p[123] == 0x42); /* ...and the host reads it in place */
  wmtr_shm_detach(7);
  CHECK(wmtr_shm_count() == 1); /* still used */
  CHECK(wmtr_shm_acquire(7, NULL) == NULL); /* but no new uses once detached */
  wmtr_shm_release(7);
  CHECK(wmtr_shm_count() == 0);

  /* two contexts attach the same resource: one mapping, gone after both detach */
  fd = make_shm(size, 1, &guest);
  int fd2 = dup(fd);
  CHECK(wmtr_shm_attach(9, fd, size) == 0);
  CHECK(wmtr_shm_attach(9, fd2, size) == 0);
  CHECK(wmtr_shm_count() == 1);
  wmtr_shm_detach(9);
  CHECK(wmtr_shm_acquire(9, &len) != NULL);
  wmtr_shm_release(9);
  wmtr_shm_detach(9);
  CHECK(wmtr_shm_count() == 0);
  CHECK(wmtr_shm_acquire(9, &len) == NULL);

  /* the mapping never runs past a short object; partial pages are not mapped */
  fd = make_shm(2 * (uint64_t)page, 3, &guest);
  CHECK(wmtr_shm_attach(11, fd, 8 * (uint64_t)page) == 0);
  CHECK(wmtr_shm_acquire(11, &len) && len == 2 * (uint64_t)page);
  wmtr_shm_release(11);
  wmtr_shm_detach(11);
  fd = make_shm((uint64_t)page / 2, 3, &guest);
  CHECK(wmtr_shm_attach(12, fd, (uint64_t)page / 2) == -ENOMEM); /* less than one page: nothing to map */
  CHECK(wmtr_shm_count() == 0);

  /* bad arguments; detach/release of unknown ids are harmless */
  CHECK(wmtr_shm_attach(0, make_shm(size, 0, &guest), size) == -EINVAL);
  CHECK(wmtr_shm_attach(5, -1, size) == -EBADF);
  wmtr_shm_detach(999);
  wmtr_shm_release(999);
  CHECK(wmtr_shm_count() == 0);

  /* eye descriptions */
  uint32_t f, b;
  struct svr_buf_eye e = {1, SVR_VK_FORMAT_R8G8B8A8_SRGB, 0, 1832 * 4, 1832, 1920, 0};
  uint64_t blen = (uint64_t)1832 * 4 * 1920;
  CHECK(wmtr_vr_buf_eye_check(&e, blen, &f, &b) == 0 && f == WMTR_MTL_RGBA8_SRGB && b == 4);
  CHECK(wmtr_vr_buf_eye_check(&e, blen - 1, &f, &b) == -EINVAL); /* one byte short */
  e.vk_format = SVR_VK_FORMAT_B8G8R8A8_UNORM;
  CHECK(wmtr_vr_buf_eye_check(&e, blen, &f, &b) == 0 && f == WMTR_MTL_BGRA8);
  e.vk_format = SVR_VK_FORMAT_R16G16B16A16_SFLOAT;
  CHECK(wmtr_vr_buf_eye_check(&e, blen, &f, &b) == -EINVAL); /* 8-byte texels: rows too narrow */
  e.row_bytes = 1832 * 8;
  CHECK(wmtr_vr_buf_eye_check(&e, blen * 2, &f, &b) == 0 && f == WMTR_MTL_RGBA16F && b == 8);
  e.vk_format = 64; /* A2B10G10R10 */
  CHECK(wmtr_vr_buf_eye_check(&e, blen * 2, &f, &b) == -ENOTSUP);
  struct svr_buf_eye o = {1, SVR_VK_FORMAT_R8G8B8A8_UNORM, 6, 400, 100, 10, 0};
  CHECK(wmtr_vr_buf_eye_check(&o, 1 << 20, &f, &b) == -EINVAL); /* offset not a whole texel */
  o.offset = 8, o.row_bytes = 402;
  CHECK(wmtr_vr_buf_eye_check(&o, 1 << 20, &f, &b) == -EINVAL); /* pitch not a whole texel */
  o.row_bytes = 404;
  CHECK(wmtr_vr_buf_eye_check(&o, 8 + 404 * 9 + 400, &f, &b) == 0); /* last row may end before the pitch */
  CHECK(wmtr_vr_buf_eye_check(&o, 8 + 404 * 9 + 399, &f, &b) == -EINVAL);
  o.offset = ~0ull - 3;
  CHECK(wmtr_vr_buf_eye_check(&o, 1 << 20, &f, &b) == -EINVAL); /* no wrap-around */
  o.offset = 0, o.width = 0;
  CHECK(wmtr_vr_buf_eye_check(&o, 1 << 20, &f, &b) == -EINVAL);

  /* writable mappings (the video encoder writes bitstreams into guest memory) */
  {
    uint8_t *g2;
    int wfd = make_shm(size, 0, &g2);
    CHECK(wmtr_shm_attach(21, wfd, size) == 0);
    uint64_t wl = 0;
    uint8_t *w = wmtr_shm_acquire_rw(21, &wl);
    CHECK(w && wl == size);
    if (w) {
      memcpy(w + 100, "bitstream", 9);
      CHECK(memcmp(g2 + 100, "bitstream", 9) == 0); /* the guest sees the host's write */
      wmtr_shm_release(21);
    }
    wmtr_shm_detach(21);
    CHECK(wmtr_shm_acquire_rw(21, NULL) == NULL);
    /* an fd that only allows reading: readable, never handed out for writing */
    char name[64];
    snprintf(name, sizeof name, "/wmtr-shm-test-ro.%d", (int)getpid());
    int rw = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(rw >= 0 && ftruncate(rw, (off_t)size) == 0);
    int ro = shm_open(name, O_RDONLY, 0);
    shm_unlink(name);
    close(rw);
    CHECK(wmtr_shm_attach(22, ro, size) == 0);
    CHECK(wmtr_shm_acquire_rw(22, NULL) == NULL);
    CHECK(wmtr_shm_acquire(22, NULL) != NULL);
    wmtr_shm_release(22);
    wmtr_shm_detach(22);
    munmap(g2, size);
  }

  printf(failures ? "wmtr_vr_shm_test: %d FAILED\n" : "wmtr_vr_shm_test: ok\n", failures);
  return failures != 0;
}

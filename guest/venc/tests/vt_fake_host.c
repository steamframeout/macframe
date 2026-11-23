/* Stand-in for the Mac side of the "vt" backend (venc_proto.h) on Linux: serves one connection on a unix socket,
 * maps the memory fds the guest backend sends, and encodes with another backend (x26x) in place of VideoToolbox.
 * Lets the guest backend, the protocol and the layer's shared staging memory be tested without a Mac.
 *
 *   vt_fake_host <socket path> <backend.so>
 *
 * lavapipe's exportable memory is a memfd with a 4 KiB private header in front of the allocation's bytes
 * (FAKE_HOST_OFFSET overrides). On the Mac the executor maps the Venus resource's shm directly. */
#include "venc_backend.h"
#include "venc_proto.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAXMEM 64
#define MAXSESS 64
static struct { uint8_t *map; size_t map_len; uint8_t *base; uint64_t size; } mem[MAXMEM];
static void *sess[MAXSESS];
static const struct venc_backend_ops *be;

static uint8_t *mem_at(uint32_t res, uint64_t off, uint64_t len)
{
    if (res == 0 || res >= MAXMEM || !mem[res].base || off > mem[res].size || len > mem[res].size - off)
        return NULL;
    return mem[res].base + off;
}

static void serve(struct venc_msg *m, int fd)
{
    m->status = 0;
    switch (m->cmd) {
    case VENC_CMD_MEMORY: {
        struct stat st;
        uint32_t r = 1;
        while (r < MAXMEM && mem[r].map)
            r++;
        if (fd < 0 || fstat(fd, &st) || r == MAXMEM || (uint64_t)st.st_size < m->mem_size) {
            m->status = -EINVAL;
            break;
        }
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            m->status = -errno;
            break;
        }
        fprintf(stderr, "fake host: memory res %u: file %lld bytes, allocation %llu\n", r, (long long)st.st_size, (unsigned long long)m->mem_size);
        mem[r].map = p;
        mem[r].map_len = (size_t)st.st_size;
        /* where the allocation's bytes start in the fd: FAKE_HOST_OFFSET (bytes), else lavapipe's layout */
        const char *fo = getenv("FAKE_HOST_OFFSET");
        uint64_t off = fo ? strtoull(fo, NULL, 0) : 4096;   /* lavapipe: a 4 KiB header, then the allocation */
        mem[r].base = (uint8_t *)p + off;
        mem[r].size = m->mem_size;
        m->session = r;
        break;
    }
    case VENC_CMD_QUERY:
        m->status = be->query(m->codec, &m->limits);
        break;
    case VENC_CMD_CREATE: {
        uint32_t s = 1;
        while (s < MAXSESS && sess[s])
            s++;
        void *e = s < MAXSESS ? be->create(&m->config) : NULL;
        if (!e) {
            m->status = -EINVAL;
            break;
        }
        sess[s] = e;
        m->session = s;
        break;
    }
    case VENC_CMD_HEADERS: {
        size_t n = VENC_PROTO_DATA;
        if (m->session >= MAXSESS || !sess[m->session]) {
            m->status = -EINVAL;
            break;
        }
        m->status = be->headers(sess[m->session], m->data, &n);
        m->bytes = n;
        break;
    }
    case VENC_CMD_ENCODE: {
        if (m->session >= MAXSESS || !sess[m->session]) {
            m->status = -EINVAL;
            break;
        }
        uint8_t *y = mem_at(m->in_res, m->luma_offset, (uint64_t)m->luma_stride * m->height);
        uint8_t *uv = mem_at(m->in_res, m->chroma_offset, (uint64_t)m->chroma_stride * ((m->height + 1) / 2));
        uint8_t *out = mem_at(m->out_res, m->out_offset, m->out_capacity);
        if (!y || !uv || !out) {
            m->status = -EFAULT;
            break;
        }
        struct venc_picture p = {.luma = y, .chroma = uv, .luma_stride = m->luma_stride, .chroma_stride = m->chroma_stride,
                                 .width = m->width, .height = m->height, .force_idr = (int)m->force_idr, .pts = m->pts};
        int idr = 0;
        long n = be->encode(sess[m->session], &p, out, m->out_capacity, &idr);
        if (n < 0) {
            m->status = (int32_t)n;
            break;
        }
        m->bytes = (uint64_t)n;
        m->is_idr = (uint32_t)idr;
        break;
    }
    case VENC_CMD_RECONFIG:
        m->status = m->session < MAXSESS && sess[m->session] ? be->reconfigure(sess[m->session], &m->config) : -EINVAL;
        break;
    case VENC_CMD_DESTROY:
        if (m->session < MAXSESS && sess[m->session]) {
            be->destroy(sess[m->session]);
            sess[m->session] = NULL;
        }
        break;
    default:
        m->status = -ENOSYS;
    }
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: vt_fake_host <socket> <backend.so>\n");
        return 2;
    }
    void *h = dlopen(argv[2], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    be = ((venc_backend_get_fn)dlsym(h, "venc_backend_get"))();
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", argv[1]);
    unlink(argv[1]);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 4)) {
        perror("bind");
        return 1;
    }
    printf("listening\n");
    fflush(stdout);
    unsigned long msgs = 0, encodes = 0;
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0)
            continue;
        struct venc_msg m;
        for (;;) {
            size_t got = 0;
            int fd = -1;
            while (got < sizeof(m)) {
                struct iovec iov = {(uint8_t *)&m + got, sizeof(m) - got};
                char ctl[CMSG_SPACE(sizeof(int))];
                struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof(ctl)};
                ssize_t r = recvmsg(c, &mh, 0);
                if (r <= 0)
                    goto next;
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
                    if (cm->cmsg_type == SCM_RIGHTS)
                        memcpy(&fd, CMSG_DATA(cm), sizeof(int));
                got += (size_t)r;
            }
            if (m.op != VENC_PROTO_OP || m.version != VENC_PROTO_VERSION) {
                m.status = -EPROTO;
            } else {
                serve(&m, fd);
                msgs++;
                encodes += m.cmd == VENC_CMD_ENCODE;
            }
            if (send(c, &m, sizeof(m), MSG_NOSIGNAL) != (ssize_t)sizeof(m))
                goto next;
        }
    next:
        close(c);
        fprintf(stderr, "fake host: connection closed after %lu messages (%lu encodes)\n", msgs, encodes);
    }
}

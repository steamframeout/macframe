/* Encoder backend "vt": the Mac's VideoToolbox, reached from the guest (venc_proto.h).
 *
 * Pictures and bitstreams stay in the layer's staging memory, which this backend imports into its wmtr context
 * (import_memory): the executor maps the same POSIX shm and encodes without a copy across the VM boundary.
 *
 * Transport: the steamac guest VR link (svr_wmtr_native_*, built with -DVENC_VT_NATIVE), or for tests a unix
 * socket named by VENC_VT_SOCKET (memory fds passed with SCM_RIGHTS). */
#include "venc_backend.h"
#include "venc_proto.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef VENC_VT_NATIVE
#include "wmtr_native.h"
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_sock = -1;
static int g_mode;   /* 0 unknown, 1 socket, 2 native, -1 unavailable */

static int connect_transport(void)
{
    if (g_mode)
        return g_mode > 0 ? 0 : -ENODEV;
    const char *path = getenv("VENC_VT_SOCKET");
    if (path && *path) {
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
        if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            g_sock = s;
            g_mode = 1;
            return 0;
        }
        if (s >= 0)
            close(s);
        g_mode = -1;
        return -ENODEV;
    }
#ifdef VENC_VT_NATIVE
    if (svr_wmtr_native_open() == 0) {
        g_mode = 2;
        return 0;
    }
#endif
    g_mode = -1;
    return -ENODEV;
}

static int io_all(int s, void *buf, size_t n, int out, int fd)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r;
        if (out) {
            struct iovec iov = {p, n};
            struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
            char ctl[CMSG_SPACE(sizeof(int))];
            if (fd >= 0) {
                memset(ctl, 0, sizeof(ctl));
                mh.msg_control = ctl;
                mh.msg_controllen = sizeof(ctl);
                struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
                c->cmsg_level = SOL_SOCKET;
                c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(c), &fd, sizeof(int));
                fd = -1;
            }
            r = sendmsg(s, &mh, MSG_NOSIGNAL);
        } else {
            r = recv(s, p, n, 0);
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -EIO;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* one request/reply; fd (or -1) travels with a socket request */
static int call(struct venc_msg *m, int fd)
{
    m->op = VENC_PROTO_OP;
    m->size = sizeof(*m);
    m->version = VENC_PROTO_VERSION;
    pthread_mutex_lock(&g_lock);
    int r = connect_transport();
    if (r == 0) {
        if (g_mode == 1) {
            r = io_all(g_sock, m, sizeof(*m), 1, fd);
            if (r == 0)
                r = io_all(g_sock, m, sizeof(*m), 0, -1);
        }
#ifdef VENC_VT_NATIVE
        else
            r = svr_wmtr_native_call(m, sizeof(*m), m, sizeof(*m));
#endif
    }
    pthread_mutex_unlock(&g_lock);
    if (r)
        return r;
    return m->status;
}

static int query(uint32_t codec, struct venc_limits *l)
{
    struct venc_msg *m = calloc(1, sizeof(*m));
    if (!m)
        return -ENOMEM;
    m->cmd = VENC_CMD_QUERY;
    m->codec = codec;
    int r = call(m, -1);
    if (r == 0)
        *l = m->limits;
    free(m);
    return r;
}

struct enc {
    uint32_t session;
    struct venc_config cfg;
};

static void *create(const struct venc_config *c)
{
    struct venc_msg *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->cmd = VENC_CMD_CREATE;
    m->config = *c;
    struct enc *e = NULL;
    if (call(m, -1) == 0 && m->session) {
        e = calloc(1, sizeof(*e));
        if (e) {
            e->session = m->session;
            e->cfg = *c;
        }
    }
    free(m);
    return e;
}

static int headers(void *v, uint8_t *out, size_t *inout)
{
    struct enc *e = v;
    struct venc_msg *m = calloc(1, sizeof(*m));
    if (!m)
        return -ENOMEM;
    m->cmd = VENC_CMD_HEADERS;
    m->session = e->session;
    int r = call(m, -1);
    if (r == 0) {
        if (m->bytes > *inout || m->bytes > VENC_PROTO_DATA) {
            r = -ENOSPC;
        } else {
            memcpy(out, m->data, m->bytes);
            *inout = m->bytes;
        }
    }
    free(m);
    return r;
}

static long encode(void *v, const struct venc_picture *pic, uint8_t *out, size_t out_size, int *is_idr)
{
    struct enc *e = v;
    (void)out;
    if (!pic->mem || !pic->out_mem)
        return -ENOTSUP;   /* the host only sees imported memory */
    struct venc_msg *m = calloc(1, sizeof(*m));
    if (!m)
        return -ENOMEM;
    m->cmd = VENC_CMD_ENCODE;
    m->session = e->session;
    m->in_res = (uint32_t)pic->mem;
    m->luma_offset = pic->luma_offset;
    m->chroma_offset = pic->chroma_offset;
    m->luma_stride = pic->luma_stride;
    m->chroma_stride = pic->chroma_stride;
    m->width = pic->width;
    m->height = pic->height;
    m->force_idr = (uint32_t)pic->force_idr;
    m->pts = pic->pts;
    m->out_res = (uint32_t)pic->out_mem;
    m->out_offset = pic->out_offset;
    m->out_capacity = out_size;
    long r = call(m, -1);
    if (r == 0) {
        *is_idr = (int)m->is_idr;
        r = (long)m->bytes;
    }
    free(m);
    return r;
}

static int reconfigure(void *v, const struct venc_config *c)
{
    struct enc *e = v;
    struct venc_msg *m = calloc(1, sizeof(*m));
    if (!m)
        return -ENOMEM;
    m->cmd = VENC_CMD_RECONFIG;
    m->session = e->session;
    m->config = *c;
    int r = call(m, -1);
    free(m);
    return r;
}

static void destroy(void *v)
{
    struct enc *e = v;
    if (!e)
        return;
    struct venc_msg *m = calloc(1, sizeof(*m));
    if (m) {
        m->cmd = VENC_CMD_DESTROY;
        m->session = e->session;
        call(m, -1);
        free(m);
    }
    free(e);
}

/* imported memory: res_id in the low 32 bits, the native transport's buffer object in the high 32 */
static int import_memory(int fd, uint64_t size, int dmabuf, uint64_t *handle)
{
    pthread_mutex_lock(&g_lock);
    int r = connect_transport();
    int mode = g_mode;
    pthread_mutex_unlock(&g_lock);
    if (r)
        return r;
    if (mode == 1) {
        struct venc_msg *m = calloc(1, sizeof(*m));
        if (!m)
            return -ENOMEM;
        m->cmd = VENC_CMD_MEMORY;
        m->mem_size = size;
        m->is_idr = (uint32_t)dmabuf;   /* reused field: dma-buf or opaque */
        r = call(m, fd);
        if (r == 0)
            *handle = m->session;
        free(m);
        return r;
    }
#ifdef VENC_VT_NATIVE
    if (!dmabuf)
        return -ENOTSUP;   /* the host maps dma-bufs (Venus shm resources) */
    uint32_t bo = 0, res = 0;
    r = svr_wmtr_native_import_fd(fd, &bo, &res);
    if (r)
        return r;
    *handle = ((uint64_t)bo << 32) | res;
    return 0;
#else
    (void)size;
    (void)dmabuf;
    return -ENOTSUP;
#endif
}

static void release_memory(uint64_t handle)
{
#ifdef VENC_VT_NATIVE
    if (g_mode == 2 && (handle >> 32))
        svr_wmtr_native_close_bo((uint32_t)(handle >> 32));
#else
    (void)handle;
#endif
}

static const struct venc_backend_ops ops = {
    .abi = VENC_ABI,
    .name = "vt",
    .query = query,
    .create = create,
    .headers = headers,
    .encode = encode,
    .reconfigure = reconfigure,
    .destroy = destroy,
    .import_memory = import_memory,
    .release_memory = release_memory,
};

__attribute__((visibility("default"))) const struct venc_backend_ops *venc_backend_get(void) { return &ops; }

/*
 * userspace/libfs/userfs.c — chapter 140 user-space filesystem
 * boilerplate.  See userfs.h for the public API.
 *
 * The serve loop is single-threaded: read one request fully,
 * dispatch it, write the reply fully, repeat.  v1 deliberately
 * does no pipelining — the kernel side serialises requests
 * per-channel anyway.
 */

#include "userfs.h"
#include "../libc/syscall.h"
#include "../libc/printf.h"

/* ENOSYS matches userspace/libc/errno.h conventions (38).  We
 * inline the constant rather than pull in another header. */
#ifndef ENOSYS
#define ENOSYS 38
#endif

/* Read exactly `n` bytes off `fd` or fail.  Returns 0 on full
 * read, -1 on EOF / error. */
static int read_full(int fd, void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < n) {
        long r = read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return 0;
}

/* Write exactly `n` bytes to `fd` or fail.  Returns 0 on full
 * write, -1 on broken-pipe / error. */
static int write_full(int fd, const void *buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t put = 0;
    while (put < n) {
        long w = write(fd, p + put, n - put);
        if (w <= 0) return -1;
        put += (uint32_t)w;
    }
    return 0;
}

/* Drain `n` bytes off `fd` and discard them.  Used when an
 * incoming request has a payload bigger than our scratch
 * buffer can hold (path > P9_MAX_PAYLOAD — should never
 * happen since kernel caps it, but defensive against a
 * deranged kernel). */
static int drain(int fd, uint32_t n)
{
    uint8_t scratch[64];
    while (n > 0) {
        uint32_t want = n < sizeof scratch ? n : (uint32_t)sizeof scratch;
        long r = read(fd, scratch, want);
        if (r <= 0) return -1;
        n -= (uint32_t)r;
    }
    return 0;
}

/* Send a reply with `op` (P9_REPLY already ORed by us),
 * `status`, and optional payload. */
static int send_reply(int rsp_fd, uint32_t op, uint32_t tag,
                      uint32_t handle, uint32_t flags,
                      int32_t status, const void *payload, uint32_t len)
{
    struct p9_msg rsp;
    rsp.op     = op | P9_REPLY;
    rsp.tag    = tag;
    rsp.handle = handle;
    rsp.flags  = flags;
    rsp.offset = 0;
    rsp.length = len;
    rsp.status = status;
    if (write_full(rsp_fd, &rsp, sizeof rsp) < 0) return -1;
    if (len > 0 && payload &&
        write_full(rsp_fd, payload, len) < 0) return -1;
    return 0;
}

int userfs_serve(const char *prefix, const struct userfs_handler *h)
{
    return userfs_serve_flags(prefix, h, 0);
}

int userfs_serve_flags(const char *prefix,
                       const struct userfs_handler *h,
                       unsigned long flags)
{
    if (!prefix || !h) return -22;   /* -EINVAL */

    int fds[2];
    long mid = mount_kernel(prefix, fds, flags);
    if (mid < 0) return (int)mid;
    int req_fd = fds[0];
    int rsp_fd = fds[1];

    printf("[libfs] %s mounted as id %ld (req=%d rsp=%d)\n",
           prefix, mid, req_fd, rsp_fd);

    /* Shared scratch for request payloads — path or write data.
     * Caps at P9_MAX_PAYLOAD + 1 so we can NUL-terminate path
     * payloads in place before handing them to callbacks. */
    static uint8_t req_payload[P9_MAX_PAYLOAD + 1];
    /* Read-side scratch (for on_read fills, on_listdir name). */
    static uint8_t rd_payload[P9_MAX_PAYLOAD];

    for (;;) {
        struct p9_msg req;
        if (read_full(req_fd, &req, sizeof req) < 0) break;

        if (req.length > P9_MAX_PAYLOAD) {
            (void)drain(req_fd, req.length);
            (void)send_reply(rsp_fd, req.op, req.tag, 0, 0, -22, NULL, 0);
            continue;
        }
        if (req.length > 0) {
            if (read_full(req_fd, req_payload, req.length) < 0) break;
        }
        req_payload[req.length] = 0;
        /* vfs_resolve hands the kernel side a rel that keeps its
         * leading slash for any path past the mount root
         * (/echo/hello → "/hello"; /echo exactly → "").  Daemons
         * are easier to write if they see bare child names, so
         * strip a single leading '/' here. */
        const char *path = (const char *)req_payload;
        if (path[0] == '/') path++;

        switch (req.op) {
        case P9_OP_OPEN: {
            if (!h->on_open) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, -ENOSYS, NULL, 0);
                break;
            }
            uint32_t hid = 0;
            int r = h->on_open(h->userdata, path, (int)req.flags, &hid);
            send_reply(rsp_fd, req.op, req.tag,
                       r == 0 ? hid : 0, 0, r, NULL, 0);
            break;
        }
        case P9_OP_READ: {
            if (!h->on_read) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, -ENOSYS, NULL, 0);
                break;
            }
            /* The desired byte count rides in `flags`; see the
             * matching comment in kernel/core/userfs.c's
             * `userfs_op_read`.  Clamp to our scratch size. */
            uint32_t cap = req.flags;
            if (cap == 0 || cap > P9_MAX_PAYLOAD) cap = P9_MAX_PAYLOAD;
            int got = h->on_read(h->userdata, req.handle, req.offset,
                                 rd_payload, cap);
            if (got < 0) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, got, NULL, 0);
            } else {
                send_reply(rsp_fd, req.op, req.tag, req.handle, 0, 0,
                           rd_payload, (uint32_t)got);
            }
            break;
        }
        case P9_OP_WRITE: {
            if (!h->on_write) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, -ENOSYS, NULL, 0);
                break;
            }
            int w = h->on_write(h->userdata, req.handle, req.offset,
                                req_payload, req.length);
            if (w < 0) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, w, NULL, 0);
            } else {
                /* Carry the accepted byte count in `flags`, not
                 * `length` — a non-zero `length` would make the
                 * kernel try to drain that many payload bytes
                 * from rsp_pipe after the header, hanging the
                 * call (the WRITE reply has no payload). */
                struct p9_msg rsp;
                rsp.op     = req.op | P9_REPLY;
                rsp.tag    = req.tag;
                rsp.handle = req.handle;
                rsp.flags  = (uint32_t)w;
                rsp.offset = 0;
                rsp.length = 0;
                rsp.status = 0;
                if (write_full(rsp_fd, &rsp, sizeof rsp) < 0) goto done;
            }
            break;
        }
        case P9_OP_CLOSE: {
            if (h->on_close) (void)h->on_close(h->userdata, req.handle);
            send_reply(rsp_fd, req.op, req.tag, 0, 0, 0, NULL, 0);
            break;
        }
        case P9_OP_LISTDIR: {
            if (!h->on_listdir) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, -ENOSYS, NULL, 0);
                break;
            }
            uint32_t type = 0;
            int n = h->on_listdir(h->userdata, path, (int)req.flags,
                                  (char *)rd_payload, P9_MAX_PAYLOAD, &type);
            if (n < 0) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, n, NULL, 0);
            } else {
                send_reply(rsp_fd, req.op, req.tag, 0, type, 0,
                           rd_payload, (uint32_t)n);
            }
            break;
        }
        case P9_OP_UNLINK: {
            int r = h->on_unlink ? h->on_unlink(h->userdata, path) : -ENOSYS;
            send_reply(rsp_fd, req.op, req.tag, 0, 0, r, NULL, 0);
            break;
        }
        case P9_OP_MKDIR: {
            int r = h->on_mkdir ? h->on_mkdir(h->userdata, path) : -ENOSYS;
            send_reply(rsp_fd, req.op, req.tag, 0, 0, r, NULL, 0);
            break;
        }
        case P9_OP_IS_DIR: {
            int r = h->on_is_dir ? h->on_is_dir(h->userdata, path) : -ENOSYS;
            if (r < 0) {
                send_reply(rsp_fd, req.op, req.tag, 0, 0, r, NULL, 0);
            } else {
                send_reply(rsp_fd, req.op, req.tag, 0, (uint32_t)r, 0, NULL, 0);
            }
            break;
        }
        case P9_OP_LOAD: {
            send_reply(rsp_fd, req.op, req.tag, 0, 0, -ENOSYS, NULL, 0);
            break;
        }
        default:
            send_reply(rsp_fd, req.op, req.tag, 0, 0, -ENOSYS, NULL, 0);
            break;
        }
    }

done:
    printf("[libfs] %s channel closed; exiting\n", prefix);
    /* Best-effort umount; the kernel may have already removed
     * the mount table entry if it tore down the channel first. */
    (void)umount_kernel((int)mid);
    close(req_fd);
    close(rsp_fd);
    return 0;
}

/*
 * userspace/libc/clipboard.h — chapter 108 client library.
 *
 * The clipboard is NOT a kernel feature.  It lives in a userspace
 * daemon (/bin/clipboardd) bound to /srv/clipboard via the
 * chapter-107 named-IPC bus.  This header is the protocol
 * definition AND the thin client.  Daemon and clients both
 * include it; the on-wire bytes are exactly the layout below.
 *
 * Why a separate header?  Because we need exactly one definition
 * of `struct clip_msg` and the operation codes, shared by every
 * party that speaks the protocol.  Putting it in libc keeps it
 * reachable from any userspace program without having to publish
 * a kernel header (the kernel knows nothing about MIME types or
 * generation counters — to the kernel, every clip message is just
 * a length-prefixed datagram on a /srv conn).
 *
 * On-wire framing:
 *
 *   +------------------- struct clip_msg ---------------------+
 *   | op  | gen | len | flags | mime[CLIP_MIME_MAX]           |
 *   +---------------------------------------------------------+
 *   | data[len] (variable, 0..CLIP_DATA_MAX bytes)            |
 *   +---------------------------------------------------------+
 *
 * One IPC message = one header + (optional) payload.  Total
 * size is bounded by chapter 107's SRV_MSG_MAX (64 KiB); we
 * cap CLIP_DATA_MAX at 32 KiB to leave comfortable headroom.
 *
 * Operations:
 *
 *   CLIP_OP_SET    set the clipboard payload (writer).
 *                  request:  op, mime, len, data
 *                  reply:    op=SET, gen=new generation, flags
 *                            (bit 0 = truncated if data > CLIP_DATA_MAX)
 *
 *   CLIP_OP_GET    read the current payload (reader).
 *                  request:  op, len=0
 *                  reply:    op=GET, gen, mime, len, data
 *                            (len=0 if clipboard empty)
 *
 *   CLIP_OP_GEN    cheap "has the clipboard changed?" probe.
 *                  request:  op, len=0
 *                  reply:    op=GEN, gen
 *                            (no payload — use this to decide
 *                            whether to bother with a full GET)
 *
 *   CLIP_OP_CLEAR  wipe the clipboard (privacy / test fixtures).
 *                  request:  op, len=0
 *                  reply:    op=CLEAR, gen=new generation
 *
 *   CLIP_OP_ERR    server-side rejection.  reply only;
 *                  flags carries a small error code (CLIP_ERR_*).
 *
 * The four client helpers (clip_set/clip_get/clip_generation/
 * clip_clear) open a fresh /srv/clipboard connection for each
 * call.  A long-lived process that does a lot of clipboard
 * work can call clip_open() once and keep the fd; the helpers
 * are convenience wrappers around the one-shot pattern.
 */
#ifndef USER_CLIPBOARD_H
#define USER_CLIPBOARD_H

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"

#define CLIP_SOCK_PATH   "/srv/clipboard"

/* Payload ceiling.  Sized so header + data + slop fits well
 * within chapter 107's SRV_MSG_MAX (64 KiB).  Anything larger
 * is silently truncated to this length and CLIP_FLAG_TRUNC
 * is set in the SET reply. */
#define CLIP_DATA_MAX    32768

/* MIME type tag.  Fixed-width to keep the framing simple.
 * Caller-meaningful, never interpreted by the daemon. */
#define CLIP_MIME_MAX    32

/* Conventional MIME strings the chapter-108 clients agree on.
 * The daemon doesn't enforce these — anything that fits in
 * CLIP_MIME_MAX is fine.  Listed here so callers can agree. */
#define CLIP_MIME_TEXT   "text/plain"
#define CLIP_MIME_URI    "text/uri-list"
#define CLIP_MIME_BIN    "application/octet-stream"

/* On-wire operation codes.  Request and reply share the same
 * enum: the server echoes the request op on success, or sends
 * CLIP_OP_ERR with a code in flags on rejection. */
enum {
    CLIP_OP_SET   = 1,
    CLIP_OP_GET   = 2,
    CLIP_OP_GEN   = 3,
    CLIP_OP_CLEAR = 4,
    CLIP_OP_ERR   = 0xEE,
};

/* Reply flag bits. */
#define CLIP_FLAG_TRUNC  0x1u   /* SET reply: data was truncated to CLIP_DATA_MAX */

/* CLIP_OP_ERR codes (carried in the err reply's flags field). */
#define CLIP_ERR_PROTO   1u   /* unrecognised op or short message  */
#define CLIP_ERR_TOOBIG  2u   /* SET payload over the wire was > SRV_MSG_MAX */

/* Fixed 48-byte header.  Packed so the layout is identical on
 * both sides of the IPC; we never use any structure padding
 * tricks.  Always written/read as one byte-for-byte chunk. */
struct clip_msg {
    uint32_t op;                   /* CLIP_OP_*                    */
    uint32_t gen;                  /* generation, server-assigned  */
    uint32_t len;                  /* payload byte count (≤ CLIP_DATA_MAX) */
    uint32_t flags;                /* CLIP_FLAG_* or CLIP_ERR_*    */
    char     mime[CLIP_MIME_MAX];  /* null-terminated; "" = none   */
    /* uint8_t data[len] follows on the wire (not part of header). */
};

/* Total bytes a SET request occupies on the wire. */
static inline size_t clip_wire_size(uint32_t len)
{
    return sizeof(struct clip_msg) + (size_t)len;
}

/* Open a fresh connection to the daemon.  Returns an fd suitable
 * for read/write/close, or a negative errno (-ENOENT_VFS if
 * /bin/clipboardd hasn't bound /srv/clipboard yet — usually
 * means init's supervisor is still bringing up services). */
static inline int clip_open(void)
{
    return srv_connect(CLIP_SOCK_PATH);
}

/* Read a fixed-size header (with one short-read retry tolerance).
 * Returns 0 on success, -1 on EOF or error. */
static inline int _clip_read_header(int fd, struct clip_msg *hdr)
{
    uint8_t *p = (uint8_t *)hdr;
    size_t need = sizeof(*hdr);
    /* The /srv message boundary lines up with one read() call --
     * chapter 107 returns whole datagrams or -EMSGSIZE.  But we
     * code defensively against any future stream-shaped peer. */
    while (need > 0) {
        long n = read(fd, p, need);
        if (n <= 0) return -1;
        p += n;
        need -= (size_t)n;
    }
    return 0;
}

/* SET the clipboard.  `mime` may be NULL → empty MIME string.
 * `data`/`len` may be NULL/0 to set an empty payload (acts like
 * a length-preserving CLEAR — generation still bumps).
 *
 * Returns the new generation on success, or a negative errno.
 * The `out_truncated` (if non-NULL) is set to 1 when the daemon
 * had to truncate the payload (len > CLIP_DATA_MAX). */
static inline int clip_set(const char *mime, const void *data, uint32_t len,
                           int *out_truncated)
{
    if (out_truncated) *out_truncated = 0;

    /* The wire layout caps payload at CLIP_DATA_MAX; sending more
     * would just be discarded by the daemon and is almost always
     * a caller bug.  Cap on this side so the wire stays sane. */
    uint32_t send_len = (len > CLIP_DATA_MAX) ? CLIP_DATA_MAX : len;
    int caller_truncated = (len > CLIP_DATA_MAX) ? 1 : 0;

    int fd = clip_open();
    if (fd < 0) return fd;

    /* Build header + payload as one contiguous buffer so we can
     * write() it in a single call -- chapter 107 enforces one
     * datagram per write(). */
    static char buf[sizeof(struct clip_msg) + CLIP_DATA_MAX];
    struct clip_msg *hdr = (struct clip_msg *)buf;
    hdr->op    = CLIP_OP_SET;
    hdr->gen   = 0;
    hdr->len   = send_len;
    hdr->flags = 0;
    /* mime: bounded copy; pad rest with zeros. */
    for (int i = 0; i < CLIP_MIME_MAX; i++) hdr->mime[i] = 0;
    if (mime) {
        for (int i = 0; i < CLIP_MIME_MAX - 1 && mime[i]; i++)
            hdr->mime[i] = mime[i];
    }
    if (send_len > 0 && data) {
        const uint8_t *src = (const uint8_t *)data;
        uint8_t *dst = (uint8_t *)(buf + sizeof(struct clip_msg));
        for (uint32_t i = 0; i < send_len; i++) dst[i] = src[i];
    }
    long w = write(fd, buf, sizeof(struct clip_msg) + send_len);
    if (w < 0) { close(fd); return (int)w; }

    struct clip_msg reply;
    if (_clip_read_header(fd, &reply) < 0) { close(fd); return -1; }
    close(fd);

    if (reply.op == CLIP_OP_ERR) return -(int)reply.flags;
    if (out_truncated) {
        *out_truncated = caller_truncated || (reply.flags & CLIP_FLAG_TRUNC) ? 1 : 0;
    }
    return (int)reply.gen;
}

/* GET the current payload.  Writes up to `cap` bytes into `out`
 * and writes the actual size (clamped to cap) into *out_len.
 * mime_out (if non-NULL, sized >= CLIP_MIME_MAX) receives the
 * MIME string.  Returns the generation on success, or a
 * negative errno.
 *
 * If the on-wire payload is bigger than `cap`, only `cap` bytes
 * land in `out` -- callers that care should size at least
 * CLIP_DATA_MAX. */
static inline int clip_get(void *out, uint32_t cap, uint32_t *out_len,
                           char *mime_out)
{
    if (out_len) *out_len = 0;
    int fd = clip_open();
    if (fd < 0) return fd;

    struct clip_msg req;
    for (int i = 0; i < (int)sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.op = CLIP_OP_GET;
    long w = write(fd, &req, sizeof(req));
    if (w < 0) { close(fd); return (int)w; }

    /* Reply = header followed by `reply.len` data bytes.
     * Chapter 107 delivers them as ONE datagram; we have to
     * read the whole thing or chapter 107 returns -EMSGSIZE. */
    static char buf[sizeof(struct clip_msg) + CLIP_DATA_MAX];
    long n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < (long)sizeof(struct clip_msg)) return -1;

    struct clip_msg *reply = (struct clip_msg *)buf;
    if (reply->op == CLIP_OP_ERR) return -(int)reply->flags;
    if (reply->op != CLIP_OP_GET) return -1;

    if (mime_out) {
        for (int i = 0; i < CLIP_MIME_MAX; i++) mime_out[i] = reply->mime[i];
    }

    uint32_t plen = reply->len;
    if (plen > CLIP_DATA_MAX) plen = CLIP_DATA_MAX;
    uint32_t copy = (plen < cap) ? plen : cap;
    if (out && copy > 0) {
        const uint8_t *src = (const uint8_t *)(buf + sizeof(struct clip_msg));
        uint8_t *dst = (uint8_t *)out;
        for (uint32_t i = 0; i < copy; i++) dst[i] = src[i];
    }
    if (out_len) *out_len = plen;
    return (int)reply->gen;
}

/* Cheap "has the clipboard changed?" probe.  Returns the current
 * generation or a negative errno.  Roundtrips one 48-byte
 * datagram each way -- much cheaper than GET for a paste-menu
 * "should this be enabled?" refresh. */
static inline int clip_generation(void)
{
    int fd = clip_open();
    if (fd < 0) return fd;
    struct clip_msg req;
    for (int i = 0; i < (int)sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.op = CLIP_OP_GEN;
    long w = write(fd, &req, sizeof(req));
    if (w < 0) { close(fd); return (int)w; }
    struct clip_msg reply;
    if (_clip_read_header(fd, &reply) < 0) { close(fd); return -1; }
    close(fd);
    if (reply.op == CLIP_OP_ERR) return -(int)reply.flags;
    return (int)reply.gen;
}

/* Wipe the clipboard.  Returns the new generation on success
 * (or a negative errno).  Generation bumps even though the
 * payload is now empty -- listeners polling GEN need to be
 * able to notice "the world changed" without holding the old
 * payload to compare against. */
static inline int clip_clear(void)
{
    int fd = clip_open();
    if (fd < 0) return fd;
    struct clip_msg req;
    for (int i = 0; i < (int)sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.op = CLIP_OP_CLEAR;
    long w = write(fd, &req, sizeof(req));
    if (w < 0) { close(fd); return (int)w; }
    struct clip_msg reply;
    if (_clip_read_header(fd, &reply) < 0) { close(fd); return -1; }
    close(fd);
    if (reply.op == CLIP_OP_ERR) return -(int)reply.flags;
    return (int)reply.gen;
}

#endif

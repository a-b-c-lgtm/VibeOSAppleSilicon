/*
 * userspace/clipboardd/clipboardd.c — chapter 108 service.
 *
 * The system clipboard, as a userspace daemon.  Binds
 * /srv/clipboard via the chapter-107 named-IPC bus, then
 * answers one connection at a time:
 *
 *     for (;;) {
 *         int cfd = srv_accept(lfd);
 *         handle_one_message(cfd);
 *         close(cfd);
 *     }
 *
 * One message per connection -- the four operations
 * (SET / GET / GEN / CLEAR) are all stateless from the
 * client's point of view, so there's no reason to keep
 * the conn open across calls.  The serve loop is
 * synchronous on purpose: SETs are quick (one heap copy),
 * GETs are quick (one heap copy back), the clipboard is
 * not a high-throughput service.  If two clients race for
 * "the current value", the generation counter serialises
 * them and the loser has to re-read.
 *
 * State is one heap-allocated payload plus a monotonic
 * generation counter and a MIME tag:
 *
 *     g_gen      -- bumps on every SET and CLEAR
 *     g_mime     -- last SET's MIME tag (or "" if empty)
 *     g_data     -- last SET's payload bytes (or NULL)
 *     g_len      -- length of g_data (0 if empty)
 *
 * The whole thing fits in ~250 lines.  No selection model,
 * no MIME negotiation, no SO_PEERCRED-shaped permission
 * check -- those are deferred (see book chapter 108).
 *
 * The supervisor (init) respawns us if we crash; the
 * generation resets to 1 on respawn, payload is lost --
 * which matches every other clipboard daemon ever
 * written (X11 selection lifetime, macOS pasteboardd
 * pre-Mojave, etc.).
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/clipboard.h"

/* ---------------- state ---------------- */

static uint32_t g_gen  = 0;            /* bumps on every SET/CLEAR; 1 after first set */
static char     g_mime[CLIP_MIME_MAX]; /* current MIME tag                            */
static uint8_t  g_data[CLIP_DATA_MAX]; /* current payload, statically reserved        */
static uint32_t g_len  = 0;            /* live length of g_data                       */

/* ---------------- tiny helpers ---------------- */

static void zero_bytes(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;
}
static void copy_bytes(void *d, const void *s, size_t n)
{
    uint8_t *b = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    while (n--) *b++ = *q++;
}

/* Pre-fill a reply header with the daemon's current state.
 * The caller may overwrite op/flags before sending. */
static void init_reply(struct clip_msg *r, uint32_t op)
{
    zero_bytes(r, sizeof(*r));
    r->op    = op;
    r->gen   = g_gen;
    r->len   = 0;
    r->flags = 0;
}

/* Send a fixed-header reply (no payload). */
static void send_reply_hdr(int fd, struct clip_msg *r)
{
    long w = write(fd, r, sizeof(*r));
    if (w != (long)sizeof(*r)) {
        /* Peer disconnected mid-reply.  Not fatal -- we'll just
         * close the fd in the caller.  Common when a test fires
         * a request and exits without reading. */
    }
}

/* Send the current clipboard (header + g_data[0..g_len]) on a
 * GET reply.  One write() = one IPC datagram. */
static void send_reply_get(int fd)
{
    /* Combined buffer.  The static-locals discipline keeps the
     * stack tiny -- daemon stacks are 4 pages by default. */
    static uint8_t buf[sizeof(struct clip_msg) + CLIP_DATA_MAX];
    struct clip_msg *r = (struct clip_msg *)buf;
    init_reply(r, CLIP_OP_GET);
    r->len = g_len;
    copy_bytes(r->mime, g_mime, CLIP_MIME_MAX);
    if (g_len > 0)
        copy_bytes(buf + sizeof(*r), g_data, g_len);
    long w = write(fd, buf, sizeof(*r) + g_len);
    (void)w;  /* same disconnect-tolerance as send_reply_hdr */
}

/* ---------------- per-op handlers ---------------- */

static void handle_set(int fd, const struct clip_msg *req,
                       const uint8_t *payload, uint32_t payload_len)
{
    /* Cap on the receive side too -- a misbehaving client that
     * lied about its `len` field can't poison g_data past the
     * static array.  In practice clip.h already caps senders. */
    uint32_t store = (req->len < payload_len) ? req->len : payload_len;
    if (store > CLIP_DATA_MAX) store = CLIP_DATA_MAX;
    int truncated = (req->len > CLIP_DATA_MAX) ? 1 : 0;

    /* Copy MIME tag (bounded; always null-terminate the last slot). */
    copy_bytes(g_mime, req->mime, CLIP_MIME_MAX);
    g_mime[CLIP_MIME_MAX - 1] = '\0';

    if (store > 0) copy_bytes(g_data, payload, store);
    g_len = store;
    g_gen++;

    printf("[clipboardd] SET gen=%u mime=%s len=%u%s\n",
           (unsigned)g_gen, g_mime[0] ? g_mime : "(none)",
           (unsigned)g_len, truncated ? " (truncated)" : "");

    struct clip_msg r;
    init_reply(&r, CLIP_OP_SET);
    if (truncated) r.flags |= CLIP_FLAG_TRUNC;
    send_reply_hdr(fd, &r);
}

static void handle_get(int fd)
{
    printf("[clipboardd] GET -> gen=%u len=%u\n",
           (unsigned)g_gen, (unsigned)g_len);
    send_reply_get(fd);
}

static void handle_gen(int fd)
{
    struct clip_msg r;
    init_reply(&r, CLIP_OP_GEN);
    printf("[clipboardd] GEN -> gen=%u\n", (unsigned)g_gen);
    send_reply_hdr(fd, &r);
}

static void handle_clear(int fd)
{
    g_len = 0;
    g_mime[0] = '\0';
    g_gen++;
    printf("[clipboardd] CLEAR gen=%u\n", (unsigned)g_gen);
    struct clip_msg r;
    init_reply(&r, CLIP_OP_CLEAR);
    send_reply_hdr(fd, &r);
}

static void handle_err(int fd, uint32_t code)
{
    struct clip_msg r;
    init_reply(&r, CLIP_OP_ERR);
    r.flags = code;
    printf("[clipboardd] ERR code=%u\n", (unsigned)code);
    send_reply_hdr(fd, &r);
}

/* ---------------- per-connection driver ---------------- */

/* One read = one datagram per chapter 107.  The daemon serves
 * exactly one request per accepted connection and then closes
 * the fd.  This keeps the protocol stateless and removes any
 * need for the daemon to track per-connection cursors. */
static void serve_one(int cfd)
{
    static uint8_t buf[sizeof(struct clip_msg) + CLIP_DATA_MAX];
    long n = read(cfd, buf, sizeof(buf));
    if (n < (long)sizeof(struct clip_msg)) {
        handle_err(cfd, CLIP_ERR_PROTO);
        return;
    }
    struct clip_msg *req = (struct clip_msg *)buf;
    const uint8_t *payload = buf + sizeof(struct clip_msg);
    uint32_t payload_len = (uint32_t)(n - (long)sizeof(struct clip_msg));

    switch (req->op) {
    case CLIP_OP_SET:   handle_set(cfd, req, payload, payload_len); break;
    case CLIP_OP_GET:   handle_get(cfd);                            break;
    case CLIP_OP_GEN:   handle_gen(cfd);                            break;
    case CLIP_OP_CLEAR: handle_clear(cfd);                          break;
    default:            handle_err(cfd, CLIP_ERR_PROTO);            break;
    }
}

/* ---------------- main loop ---------------- */

int main(void)
{
    /* Zero state explicitly -- BSS is already zero but being
     * explicit makes the respawn behaviour readable (after a
     * crash, init respawns us; we start with gen=0 and an
     * empty payload, same as boot.  This matches the X11 /
     * pasteboardd convention that clipboard contents do NOT
     * survive the daemon dying). */
    g_gen = 0;
    g_len = 0;
    zero_bytes(g_mime, sizeof(g_mime));

    int lfd = srv_bind(CLIP_SOCK_PATH);
    if (lfd < 0) {
        printf("[clipboardd] srv_bind(%s) failed: %d\n",
               CLIP_SOCK_PATH, lfd);
        return 1;
    }
    printf("[clipboardd] ready on %s (lfd=%d)\n",
           CLIP_SOCK_PATH, lfd);

    for (;;) {
        int cfd = srv_accept(lfd);
        if (cfd < 0) {
            /* EINTR is fine -- a signal could break us out of
             * accept; just resume.  Anything else is a kernel
             * bug (no other failure modes are spec'd). */
            if (cfd == -4 /*EINTR*/) continue;
            printf("[clipboardd] accept failed: %d\n", cfd);
            close(lfd);
            return 1;
        }
        serve_one(cfd);
        close(cfd);
    }
}

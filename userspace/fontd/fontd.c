/*
 * userspace/fontd/fontd.c — chapter 108b font server.
 *
 * Binds /srv/font (chapter 107 named IPC) and answers
 * FONT_OP_GLYPH / FONT_OP_METRICS requests using the TTF
 * rasteriser moved out of the kernel.  One process, one
 * embedded face (DejaVu Sans).
 *
 * Connection shape: PERSISTENT.  Each accepted conn handles
 * many requests until the client closes (read returns 0).
 * This matches kernel/core/wm_font.c, which caches the conn
 * for the kernel's lifetime to avoid an srv_connect per
 * glyph.  An earlier draft of this file closed cfd after the
 * first request, mirroring clipboardd's shape — but the
 * clipboard does one round-trip per user action, whereas the
 * WM does one per glyph, and that mismatch turned every
 * cache miss after the first into srv_write -> -EPIPE,
 * forcing a bitmap fallback and a reconnect.  Visible
 * symptom: M, L, S, P in <h1> text rendered bitmap on the
 * first paint of a browser window, and a layout shift on
 * the first launcher hover-repaint.
 *
 * The pre-rasterised ASCII range at the default size (16 px)
 * is warmed at startup so the first WM-side gui_draw_text
 * after boot doesn't pay a cold-cache penalty per glyph.
 *
 * If we die, init's supervisor (chapter 108) respawns us.
 * The cache is rebuilt from scratch on respawn.  Glyph
 * render is pure and idempotent, so cache-loss has no
 * user-visible effect.
 *
 * Concurrency (chapter 108c)
 * --------------------------
 *
 * Until chapter 108c the daemon served one client at a time:
 * the accept loop blocked inside serve_conn() until the client
 * closed, so the next srv_accept didn't run until the current
 * client went away.  That was fine when the kernel WM was the
 * only client.  Chapter 108c gives every GUI app its own
 * persistent fontd connection (via libgui/draw.c), which means
 * one held-open client (kernel WM) would otherwise starve all
 * the apps waiting in srv_connect's pending queue.
 *
 * Fix: spawn one worker thread per accepted connection using
 * thread_spawn_files (CLONE_FILES, so the worker can read/write
 * the cfd main accepted).  The main thread accepts and reaps
 * zombies; workers serve until their client EOFs, then close
 * the conn and exit.  The TTF rasteriser cache inside
 * ttf_get_glyph mutates on first miss for a given (cp, size),
 * so all calls into it from workers are serialised behind
 * g_face_lock.  Steady state (cache filled by ttf_warm_ascii)
 * is uncontended.
 */

#include "../libc/syscall.h"
#include "../libc/thread.h"
#include "../libc/printf.h"
#include "../libc/font_proto.h"
#include "ttf.h"

/* Symbols emitted by Makefile's objcopy -I binary rule for
 * the DejaVu Sans blob now linked into fontd (chapter 108b
 * moved this out of the kernel image). */
extern const uint8_t _binary_DejaVuSans_ttf_start[];
extern const uint8_t _binary_DejaVuSans_ttf_end[];

/* The face for our one embedded font (DejaVu Sans).  Allocated
 * in main() before we bind /srv/font so a connecting client
 * can never see a half-initialised face. */
static struct ttf_face *g_face;

/* Statistics, exposed via FONT_OP_HEALTH.  No /proc/fontd
 * today; the per-process count of served requests is just for
 * the test harness to observe respawn behaviour.  Wrapped in
 * the lock too — atomic increment isn't enough because the
 * stats reply reads the value and we don't want a torn read. */
static uint64_t g_served = 0;

/* Single mutex around everything touching g_face's internal
 * caches and g_served.  Held only across the actual
 * rasteriser call, which is microseconds; the IPC read/write
 * happens OUTSIDE the lock so a slow client can't pin the
 * face. */
static mutex_t g_face_lock = MUTEX_INIT;

/* Largest message we ever send: header + max bitmap.
 * The rasteriser caps glyph bitmaps at 256x256 in theory, but
 * at the default 16 px size the worst real glyph is ~32x32
 * (= 1024 B + header).  Workers allocate REPLY_BUF_BYTES on
 * the stack (each worker has its own 64 KiB stack), so we
 * cap at 8 KiB to keep the stack frame small.  If a future
 * chapter adds bigger sizes we'll need a malloc'd buffer per
 * worker; today the static cap is fine.
 * Chapter 108c removed the file-scope static buffer that
 * predated multi-client service. */
#define REPLY_BUF_BYTES  8192u
/* Defensive cap on the bitmap payload: send_reply will refuse
 * to encode anything bigger so we don't trample the worker's
 * stack. */
#define MAX_BITMAP_BYTES (REPLY_BUF_BYTES - sizeof(struct font_msg))

static void zero_bytes(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;
}
static void copy_bytes(void *d, const void *s, size_t n)
{
    uint8_t *b = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    while (n--) *b++ = *q++;
}

/* Send a reply with the given header and optional payload.
 * Tolerates peer disconnect mid-reply (next accept just
 * picks up a fresh client).  Caller supplies the scratch
 * buffer so workers don't share state across threads. */
static void send_reply(int cfd, uint8_t *scratch,
                       const struct font_msg *hdr,
                       const uint8_t *payload, uint32_t payload_len)
{
    /* Copy into scratch so we issue one write() = one IPC
     * datagram with header and payload contiguous. */
    copy_bytes(scratch, hdr, sizeof(*hdr));
    if (payload_len > 0 && payload) {
        copy_bytes(scratch + sizeof(*hdr), payload, payload_len);
    }
    long w = write(cfd, scratch, sizeof(*hdr) + payload_len);
    (void)w;
}

static void send_err(int cfd, uint8_t *scratch,
                     const struct font_msg *req, int16_t code)
{
    struct font_msg r;
    zero_bytes(&r, sizeof(r));
    r.op        = FONT_OP_ERR;
    r.flags     = 0;
    r.font_id   = req ? req->font_id   : 0;
    r.codepoint = req ? req->codepoint : 0;
    r.size_px   = req ? req->size_px   : 0;
    r.status    = code;
    send_reply(cfd, scratch, &r, NULL, 0);
}

/* ---------------- per-op handlers ---------------- */

static void handle_glyph(int cfd, uint8_t *scratch,
                         const struct font_msg *req, int want_bitmap)
{
    if (req->font_id != FONT_ID_DEFAULT) {
        send_err(cfd, scratch, req, FONT_ERR_NOFONT);
        return;
    }
    uint16_t size = req->size_px ? req->size_px : FONT_SIZE_DEFAULT;

    struct font_glyph g;
    int rc;
    /* The rasteriser mutates a per-(face, size) cache on first
     * miss for a given cp, so multiple workers calling it
     * concurrently could corrupt that linked-list/cache.
     * Serialise the call.  Steady state (cache hot) is
     * uncontended; cold misses serialise across workers, which
     * is fine — ttf_warm_ascii has already pre-filled the
     * common range at startup. */
    mutex_lock(&g_face_lock);
    if (want_bitmap) rc = ttf_get_glyph(g_face, req->codepoint, size, &g);
    else             rc = ttf_get_metrics(g_face, req->codepoint, size, &g);
    if (rc == 0) g_served++;
    mutex_unlock(&g_face_lock);
    if (rc != 0) {
        send_err(cfd, scratch, req, FONT_ERR_NOGLYPH);
        return;
    }

    struct font_msg r;
    zero_bytes(&r, sizeof(r));
    r.op           = want_bitmap ? FONT_OP_GLYPH : FONT_OP_METRICS;
    r.font_id      = req->font_id;
    r.codepoint    = req->codepoint;
    r.size_px      = size;
    r.status       = FONT_OK;
    r.left_bearing = g.left_bearing;
    r.top_bearing  = g.top_bearing;
    r.advance      = g.advance;
    r.bmp_w        = want_bitmap ? g.bitmap_w : 0;
    r.bmp_h        = want_bitmap ? g.bitmap_h : 0;

    uint32_t payload_len = (uint32_t)r.bmp_w * (uint32_t)r.bmp_h;
    if (payload_len > MAX_BITMAP_BYTES) {
        /* Shouldn't happen at today's sizes — defensive bound
         * to keep us from trampling the worker's stack scratch
         * buffer.  Degrade to metrics-only so the client at
         * least gets advance to lay out with. */
        r.op = FONT_OP_METRICS;
        r.bmp_w = r.bmp_h = 0;
        payload_len = 0;
    }
    send_reply(cfd, scratch, &r, g.pixels, payload_len);
}

static void handle_health(int cfd, uint8_t *scratch,
                          const struct font_msg *req)
{
    struct font_msg r;
    zero_bytes(&r, sizeof(r));
    r.op        = FONT_OP_HEALTH;
    r.font_id   = req ? req->font_id : 0;
    /* Repurpose advance as the served-count low-32 (tests just
     * check >= 0).  Negative status would say "ill". */
    r.status    = FONT_OK;
    mutex_lock(&g_face_lock);
    uint64_t served = g_served;
    mutex_unlock(&g_face_lock);
    r.advance   = (uint16_t)(served & 0xFFFFu);
    send_reply(cfd, scratch, &r, NULL, 0);
}

/* ---------------- per-connection driver ---------------- */

/* Serve requests on cfd until the client closes (read returns
 * 0) or sends a malformed header.  A protocol error replies
 * once and tears the conn down so we don't loop forever on a
 * stream of junk — the client will reconnect on its next
 * glyph miss.
 *
 * Runs on a dedicated worker thread (chapter 108c) so other
 * clients aren't starved behind this conn.  `scratch` is the
 * per-worker reply buffer, stack-allocated by serve_thread. */
static void serve_conn(int cfd, uint8_t *scratch)
{
    struct font_msg req;
    for (;;) {
        long n = read(cfd, &req, sizeof(req));
        if (n == 0) return;                         /* clean EOF */
        if (n < (long)sizeof(req)) {
            send_err(cfd, scratch, NULL, FONT_ERR_PROTO);
            return;
        }
        switch (req.op) {
        case FONT_OP_GLYPH:   handle_glyph(cfd, scratch, &req, 1); break;
        case FONT_OP_METRICS: handle_glyph(cfd, scratch, &req, 0); break;
        case FONT_OP_HEALTH:  handle_health(cfd, scratch, &req);   break;
        default:
            send_err(cfd, scratch, &req, FONT_ERR_PROTO);
            return;
        }
    }
}

/* Worker thread entry.  Allocates its own scratch buffer on
 * its stack (per-thread so workers can't trample each other's
 * pending writes), serves the conn until the client EOFs,
 * closes the conn and exits.  The main thread reaps the
 * zombie via waitpid(WNOHANG) on the next accept iteration. */
static void serve_thread(void *arg)
{
    int cfd = (int)(long)arg;
    uint8_t scratch[REPLY_BUF_BYTES];
    serve_conn(cfd, scratch);
    close(cfd);
    exit(0);
}

/* ---------------- main loop ---------------- */

int main(void)
{
    uint32_t blob_size = (uint32_t)(
        _binary_DejaVuSans_ttf_end - _binary_DejaVuSans_ttf_start);
    g_face = ttf_init_face(_binary_DejaVuSans_ttf_start, blob_size);
    if (!g_face) {
        printf("[fontd] ttf_init_face failed (blob_size=%u)\n",
               (unsigned)blob_size);
        return 1;
    }
    /* Warm the ASCII printable range at the default size so the
     * first WM-side gui_draw_text after init doesn't pay a
     * per-glyph rasterise cost.  ~95 glyphs at 16 px takes a
     * negligible fraction of a second on our reference hardware.
     * Done BEFORE we accept any connection so g_face's cache is
     * dense by the time the first worker thread reads it,
     * which keeps the per-call mutex critical section short. */
    ttf_warm_ascii(g_face, FONT_SIZE_DEFAULT);

    int lfd = srv_bind(FONT_SOCK_PATH);
    if (lfd < 0) {
        printf("[fontd] srv_bind(%s) failed: %d\n", FONT_SOCK_PATH, lfd);
        return 1;
    }
    printf("[fontd] ready on %s (lfd=%d)\n", FONT_SOCK_PATH, lfd);

    for (;;) {
        /* Reap any worker threads that have exited.  Polling
         * once per accept iteration is enough — workers exit
         * only when their client disconnects, which is rare
         * enough that we don't need a SIGCHLD path. */
        while (waitpid(-1, NULL, WNOHANG) > 0) { /* nothing */ }

        int cfd = srv_accept(lfd);
        if (cfd < 0) {
            if (cfd == -4 /*EINTR*/) continue;
            printf("[fontd] accept failed: %d\n", cfd);
            close(lfd);
            return 1;
        }
        /* Spawn a worker thread per accepted connection so the
         * main thread can keep accepting.  CLONE_FILES so the
         * worker sees the cfd we just got back from accept.
         * cpu_id == -1 lets the scheduler pick — workers are
         * I/O-bound, no need to pin. */
        int tid = thread_spawn_files(serve_thread,
                                     (void *)(long)cfd, -1);
        if (tid < 0) {
            /* Couldn't spawn — fall back to in-line service
             * so the client doesn't hang forever.  Other
             * clients block behind us until this one EOFs,
             * but at least no request is lost. */
            uint8_t scratch[REPLY_BUF_BYTES];
            serve_conn(cfd, scratch);
            close(cfd);
        }
    }
}

/*
 * userspace/libgui/draw.c — chapter 116 fontd client + text
 * rendering.
 *
 * Shape of this file
 * ------------------
 *
 * Per-process state (file-local statics):
 *
 *   g_conn       persistent srv_connect fd to /srv/font; -1
 *                when never connected or when a previous I/O
 *                failed and we dropped it.
 *   g_down       sticky "fontd is unreachable" flag.  Set on
 *                the first srv_connect failure; cleared only
 *                when a manual draw_font_reset() is called
 *                (no caller does today — the desktop is dead
 *                without fontd anyway).
 *   g_cache[N]   flat per-codepoint glyph cache.  N = 256
 *                covers ASCII + Latin-1 Supplement, which is
 *                everything our userspace renders.
 *
 * Why persistent connection (not per-request)
 * -------------------------------------------
 *
 * Chapter 115 learned the hard way that opening + closing a
 * srv_connect per glyph is the wrong shape: every cache miss
 * costs an srv_connect + handshake + close cycle.  For the
 * kernel WM that meant a measurable per-frame stall whenever
 * a window's title bar changed glyphs.  Userspace apps have
 * roughly the same access pattern (a paint frame touches a
 * handful of codepoints; the next frame touches the same
 * codepoints again), so we hold the conn open from the first
 * draw_text call until the process exits.
 *
 * The trade-off: while g_conn is held open, fontd's accept
 * loop is parked in `read(g_conn)` on this client (chapter
 * 108b's single-connection model).  Chapter 116 upgrades
 * fontd to spawn a worker thread per accepted conn so the
 * other clients (kernel WM + every GUI app) aren't serialised
 * behind each other.  See userspace/fontd/fontd.c.
 *
 * Drop-and-retry on I/O failure
 * -----------------------------
 *
 * If srv_write or srv_read on g_conn ever returns short or
 * negative — fontd died and respawned, or chapter-107 IPC
 * tore the conn down for back-pressure — we close g_conn and
 * try one srv_connect on the very next call.  If THAT fails,
 * g_down latches; future calls return immediately with
 * fallback values until process exit.  This mirrors the
 * kernel WM's wm_font.c reconnect policy.
 *
 * Cache shape and memory cost
 * ---------------------------
 *
 * 256 entries × (struct + per-glyph alpha buffer).  Worst-
 * case alpha buffer for the 16 px size is ~16×16 = 256 B; in
 * practice most glyphs occupy ~10×14 = 140 B.  Filling all
 * 95 printable ASCII glyphs costs ~16 KB of heap per app, in
 * exchange for "zero IPC per text-draw frame in steady
 * state."  Acceptable for the ~half-dozen GUI apps that ever
 * call draw_text concurrently.
 *
 * Thread safety
 * -------------
 *
 * NOT thread-safe.  Apps that share an address space across
 * threads and call draw_text from more than one thread
 * (browser parser thread?) need to either (a) serialise
 * draw_text behind a mutex_t or (b) draw from a single
 * thread.  Today no app does both; if that changes, the
 * cleanest fix is one mutex around fetch_glyph (the cache
 * write path) — reads can stay lock-free because the publish
 * order in fetch_glyph keeps `have=0` until the entry is
 * fully populated.
 */

#include "draw.h"
#include "../libc/syscall.h"
#include "../libc/malloc.h"
#include "../libc/font_proto.h"

#include <stddef.h>
#include <stdint.h>

/* GCC's optimiser may pattern-match the `(struct) { 0 }` init
 * below into a call to memset; freestanding userspace has no
 * libc-memset.  Provide a private one.  Same trap from the
 * early printf chapter (and documented in
 * /memories/freestanding-c-memset-trap.md). */
static __attribute__((used)) void *memset(void *dst, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

/* ── Per-codepoint glyph cache ───────────────────────────── */

struct draw_glyph {
    uint8_t  have;          /* 1 = entry valid (success OR negative) */
    uint8_t  negative;      /* 1 = fontd had no glyph — render as space */
    int16_t  left_bearing;  /* px from pen origin to bitmap left */
    int16_t  top_bearing;   /* px from baseline up to bitmap top */
    uint16_t advance;       /* px to advance pen after this glyph */
    uint16_t bmp_w;
    uint16_t bmp_h;
    uint8_t *alpha;         /* malloc'd bmp_w * bmp_h, or NULL */
};

#define DRAW_CACHE_N 0x100u

static struct draw_glyph g_cache[DRAW_CACHE_N];

/* ── Persistent fontd connection ─────────────────────────── */

static int g_conn = -1;     /* -1 = no conn */
static int g_down = 0;      /* 1 = fontd unreachable, fall back */

static void drop_conn(void)
{
    if (g_conn >= 0) {
        close(g_conn);
        g_conn = -1;
    }
}

/* Try to (re)open the connection.  Returns 0 on success, -1
 * on failure (and sets g_down to keep us from re-trying every
 * single call). */
static int ensure_conn(void)
{
    if (g_conn >= 0) return 0;
    if (g_down)     return -1;
    int fd = srv_connect(FONT_SOCK_PATH);
    if (fd < 0) {
        g_down = 1;
        return -1;
    }
    g_conn = fd;
    return 0;
}

/* Largest header + payload for a default-size glyph.  16 px
 * caps the bitmap at ~32x32 = 1 KB; we keep 4 KB of slack so
 * future sizes (24 / 32 px section headings) don't need a
 * second buffer.  Static so each draw_text call doesn't grow
 * its own stack frame past the 8 KiB userspace default. */
#define DRAW_REPLY_BUF_BYTES 4096u
static uint8_t g_reply_buf[DRAW_REPLY_BUF_BYTES];

/* Issue one FONT_OP_GLYPH and fill *out from the reply.
 * Returns 0 on success (including the "no such glyph"
 * negative-cache case, where out->negative is set), -1 on
 * I/O failure (caller falls back). */
static int fetch_glyph(uint32_t cp, struct draw_glyph *out)
{
    if (ensure_conn() < 0) return -1;

    struct font_msg req;
    memset(&req, 0, sizeof(req));
    req.op        = FONT_OP_GLYPH;
    req.font_id   = FONT_ID_DEFAULT;
    req.codepoint = cp;
    req.size_px   = FONT_SIZE_DEFAULT;

    long w = write(g_conn, &req, sizeof(req));
    if (w != (long)sizeof(req)) {
        drop_conn();
        g_down = 1;
        return -1;
    }

    long n = read(g_conn, g_reply_buf, DRAW_REPLY_BUF_BYTES);
    if (n < (long)sizeof(struct font_msg)) {
        drop_conn();
        g_down = 1;
        return -1;
    }
    struct font_msg *r = (struct font_msg *)g_reply_buf;

    /* FONT_OP_ERR with FONT_ERR_NOGLYPH is a negative cache
     * entry — perfectly normal for codepoints fontd doesn't
     * have (control chars, missing glyphs).  Don't tear the
     * conn down; just record "skip this cp". */
    if (r->status != FONT_OK) {
        memset(out, 0, sizeof(*out));
        out->have     = 1;
        out->negative = 1;
        /* Still give it a synthetic advance so calling code's
         * layout doesn't collapse at unknown chars.  Half the
         * cell height matches the fallback in wm_font.c. */
        out->advance  = DRAW_TEXT_CELL_H / 2;
        return 0;
    }

    uint32_t bytes = (uint32_t)r->bmp_w * (uint32_t)r->bmp_h;
    if (n < (long)(sizeof(struct font_msg) + bytes)) {
        drop_conn();
        g_down = 1;
        return -1;
    }

    out->have         = 1;
    out->negative     = 0;
    out->left_bearing = r->left_bearing;
    out->top_bearing  = r->top_bearing;
    out->advance      = r->advance ? r->advance : DRAW_TEXT_CELL_H / 2;
    out->bmp_w        = r->bmp_w;
    out->bmp_h        = r->bmp_h;
    out->alpha        = NULL;

    if (bytes > 0) {
        uint8_t *buf = (uint8_t *)malloc(bytes);
        if (!buf) {
            /* Couldn't store the bitmap — treat as negative
             * cache (advance only) so the line still lays out. */
            out->negative = 1;
            out->bmp_w = out->bmp_h = 0;
            return 0;
        }
        for (uint32_t i = 0; i < bytes; i++)
            buf[i] = g_reply_buf[sizeof(struct font_msg) + i];
        out->alpha = buf;
    }
    return 0;
}

/* Return a pointer to the cached glyph for `cp`, fetching it
 * on first use.  Returns NULL only when both the cache and
 * fontd are unavailable (cp out of range, or fontd down and
 * cache empty). */
static struct draw_glyph *get_glyph(uint32_t cp)
{
    if (cp >= DRAW_CACHE_N) return NULL;
    struct draw_glyph *e = &g_cache[cp];
    if (e->have) return e;
    if (fetch_glyph(cp, e) < 0) return NULL;
    return e;
}

/* ── Pixel blend (mirrors wm_blend_pixel in kernel/core/wm.c) ── */

static inline void blend_pixel(uint32_t *p, uint32_t fg, uint8_t a)
{
    if (a == 0) return;
    if (a == 0xFF) { *p = fg; return; }
    uint32_t dst = *p;
    uint8_t fr = (uint8_t)((fg >> 16) & 0xFF);
    uint8_t fgn = (uint8_t)((fg >> 8) & 0xFF);
    uint8_t fb = (uint8_t)(fg & 0xFF);
    uint8_t dr = (uint8_t)((dst >> 16) & 0xFF);
    uint8_t dg = (uint8_t)((dst >> 8) & 0xFF);
    uint8_t db = (uint8_t)(dst & 0xFF);
    uint16_t inv = (uint16_t)(255 - a);
    uint8_t orr = (uint8_t)(((uint16_t)fr  * a + (uint16_t)dr * inv) / 255);
    uint8_t org = (uint8_t)(((uint16_t)fgn * a + (uint16_t)dg * inv) / 255);
    uint8_t orb = (uint8_t)(((uint16_t)fb  * a + (uint16_t)db * inv) / 255);
    *p = ((uint32_t)0xFFu << 24) | ((uint32_t)orr << 16)
       | ((uint32_t)org  <<  8) |  (uint32_t)orb;
}

/* ── Public draw_text / draw_text_clipped / draw_measure_text  */

void draw_text_clipped(struct gui_fb *fb,
                       int32_t x, int32_t y,
                       const char *s,
                       uint32_t fg_bgra, uint32_t bg_bgra,
                       int transparent,
                       int32_t cl_x, int32_t cl_y,
                       int32_t cl_w, int32_t cl_h)
{
    if (!fb || !fb->pixels || !s) return;

    /* Clamp the clip rect to the framebuffer.  After this the
     * clip rect is the intersection of the caller's rect and
     * (0, 0, fb->w, fb->h); per-pixel writes test against
     * (cx0..cx1, cy0..cy1) and need no separate fb bounds
     * check. */
    int32_t cx0 = cl_x;
    int32_t cy0 = cl_y;
    int32_t cx1 = cl_x + cl_w;
    int32_t cy1 = cl_y + cl_h;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > (int32_t)fb->w) cx1 = (int32_t)fb->w;
    if (cy1 > (int32_t)fb->h) cy1 = (int32_t)fb->h;
    if (cx1 <= cx0 || cy1 <= cy0) return;

    int32_t cx = x;
    uint32_t line_step = DRAW_TEXT_CELL_H + 2u;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char ch = *p;
        if (ch == '\n') {
            y += (int32_t)line_step;
            cx = x;
            continue;
        }
        struct draw_glyph *g = get_glyph((uint32_t)ch);
        uint16_t adv;
        if (!g) {
            /* fontd unreachable, no cache, no synth — just
             * advance an estimated half-cell so the line
             * doesn't degenerate. */
            adv = DRAW_TEXT_CELL_H / 2;
            cx += adv;
            continue;
        }
        adv = g->advance;

        /* Wrap if this glyph won't fit in the framebuffer.
         * Wrapping is governed by fb->w (the real surface
         * width) not the clip rect — a clipped paint of a
         * normal-length title should NOT spuriously wrap just
         * because the clip happens to be narrow. */
        if (cx + (int32_t)adv > (int32_t)fb->w) {
            cx = x;
            y += (int32_t)line_step;
        }
        if (y + (int32_t)DRAW_TEXT_CELL_H > (int32_t)fb->h) return;
        if (y + (int32_t)DRAW_TEXT_CELL_H <= cy0) {
            cx += (int32_t)adv;
            continue;
        }
        if (y >= cy1) return;

        if (!g->negative && g->alpha) {
            int32_t bx = cx + g->left_bearing;
            int32_t by = y + (int32_t)DRAW_TEXT_BASELINE - g->top_bearing;
            for (int row = 0; row < g->bmp_h; row++) {
                int32_t py = by + row;
                if (py < cy0 || py >= cy1) continue;
                uint32_t *line = (uint32_t *)(fb->pixels
                                              + (size_t)py * fb->stride);
                for (int col = 0; col < g->bmp_w; col++) {
                    int32_t px = bx + col;
                    if (px < cx0 || px >= cx1) continue;
                    uint8_t a = g->alpha[row * g->bmp_w + col];
                    uint32_t *slot = &line[px];
                    if (a == 0) {
                        if (!transparent) *slot = bg_bgra;
                        continue;
                    }
                    if (transparent) {
                        blend_pixel(slot, fg_bgra, a);
                    } else {
                        uint32_t tmp = bg_bgra;
                        blend_pixel(&tmp, fg_bgra, a);
                        *slot = tmp;
                    }
                }
            }
        } else if (!transparent) {
            /* Negative cache: still clear the glyph cell so
             * the bg doesn't have a stale rectangle of old
             * pixels under what would have been a missing
             * glyph. */
            for (uint32_t row = 0; row < DRAW_TEXT_CELL_H; row++) {
                int32_t py = y + (int32_t)row;
                if (py < cy0 || py >= cy1) continue;
                uint32_t *line = (uint32_t *)(fb->pixels
                                              + (size_t)py * fb->stride);
                for (uint16_t col = 0; col < adv; col++) {
                    int32_t px = cx + (int32_t)col;
                    if (px < cx0 || px >= cx1) continue;
                    line[px] = bg_bgra;
                }
            }
        }

        cx += (int32_t)adv;
    }
}

void draw_text(struct gui_fb *fb,
               int32_t x, int32_t y,
               const char *s,
               uint32_t fg_bgra, uint32_t bg_bgra,
               int transparent)
{
    if (!fb) return;
    draw_text_clipped(fb, x, y, s, fg_bgra, bg_bgra, transparent,
                      0, 0, (int32_t)fb->w, (int32_t)fb->h);
}

int draw_measure_text(const char *s)
{
    if (!s) return 0;
    uint32_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char ch = *p;
        if (ch == '\n') break;
        struct draw_glyph *g = get_glyph((uint32_t)ch);
        w += g ? g->advance : (DRAW_TEXT_CELL_H / 2);
    }
    return (int)w;
}

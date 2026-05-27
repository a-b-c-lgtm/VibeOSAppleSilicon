/*
 * kernel/core/wm_font.c — chapter 115 WM-side font client.
 *
 * Implementation notes — see wm_font.h for the contract.
 *
 * IPC shape:
 *   - One persistent struct srv_conn to /srv/font, allocated
 *     by the in-kernel srv module on first cache miss.
 *   - Per-request: write FONT_OP_GLYPH header, read reply,
 *     copy bitmap bytes into cache.
 *   - On EPIPE / NOENT: drop the conn, mark fontd "down".
 *     Next caller retries srv_connect.  If still down, return
 *     -1 so wm_draw_text falls back to the bitmap font.
 *
 * Cache shape:
 *   - 256-entry flat array indexed by codepoint.  Matches the
 *     CACHE_FLAT_N choice in the daemon's rasteriser; covers
 *     ASCII + Latin-1 Supplement, which is everything our
 *     userspace currently renders.  Higher codepoints fall
 *     back to the bitmap font.
 *   - Each cached entry owns a small kmalloc'd alpha buffer
 *     plus a `struct glyph_info` written once and never
 *     mutated; readers don't need the lock.
 *
 * Locking:
 *   - g_lock_busy protects (a) the connection state and
 *     (b) cache writes.  Cache reads are unlocked — a partial
 *     entry would mean we'd return zeros for `pixels`, and
 *     callers tolerate that (they branch on `pixels != NULL`
 *     before blitting).  The publish order in fill_cache_entry
 *     keeps pixels = NULL until the alpha buffer is fully
 *     populated.
 *   - lock is acquired with the standard spin-yield idiom
 *     (the kernel doesn't ship a blocking mutex; futex-style
 *     thread_block_on_held is overkill for a path this cold
 *     and would tangle with the IPC blocking the same conn).
 */

#include "wm_font.h"
#include "srv.h"
#include "thread.h"
#include "heap.h"
#include "serial.h"
#include "../device/font.h"
#include "../../userspace/libc/font_proto.h"

#include <stddef.h>
#include <stdint.h>

#define WM_FONT_CACHE_N    0x100u
#define WM_FONT_SIZE_PX    FONT_SIZE_DEFAULT
/* DejaVu Sans @ 16 px: ascent ~13, descent ~3 — matches the
 * chapter-102 cell height of 16 and baseline of 12 the WM
 * was using already.  Hardcoded for chapter 115; chapter 116
 * generalises this when libgui callers ask for other sizes. */
#define WM_FONT_CELL_HEIGHT 16u
#define WM_FONT_BASELINE    12u

/* Per-cp glyph cache slot. ------------------------------------- */
struct wm_font_entry {
    int                in_use;     /* slot has been filled or attempted */
    int                negative;   /* fontd rejected this cp (use bitmap fb) */
    struct glyph_info  gi;
};

static struct wm_font_entry g_cache[WM_FONT_CACHE_N];

static struct srv_conn *g_conn   = NULL;
static int              g_down   = 0;  /* fontd known unreachable, retry on next call */

/* Coarse-grained yielding lock --- see header for rationale.
 * The kernel ships a spinlock_t but its acquire path spins
 * without yielding, which would burn a core (or livelock on
 * UP) while we hold the lock across an IPC round-trip that
 * blocks on srv_read.  Inline LL/SC + yield() lets a waiter
 * surrender the CPU until the holder releases.  Same algebra
 * as spin_lock in arch/spinlock.h, just with the back-off
 * branch jumping to a yield instead of spinning straight back
 * into the load. */
static volatile uint32_t g_lock_busy = 0;

static inline uint32_t wmfont_try_acquire(void)
{
    uint32_t prev, scratch;
    __asm__ volatile(
        "1: ldaxr   %w0, [%2]   \n"   /* prev = *busy */
        "   cbnz    %w0, 2f      \n"   /* if held, return 1 */
        "   mov     %w1, #1      \n"
        "   stxr    %w0, %w1, [%2] \n" /* try to store 1 */
        "   cbnz    %w0, 1b      \n"   /* SC failure, retry LL */
        "   mov     %w0, #0      \n"   /* success */
        "2:                       \n"
        : "=&r"(prev), "=&r"(scratch)
        : "r"(&g_lock_busy)
        : "memory");
    return prev;          /* 0 = acquired, non-zero = was already held */
}

static void wmfont_lock(void)
{
    while (wmfont_try_acquire() != 0) {
        yield();
    }
}

static void wmfont_unlock(void)
{
    __asm__ volatile("stlr wzr, [%0]" :: "r"(&g_lock_busy) : "memory");
}

/* Acquire/release semantics for cache-entry publish/read.  The
 * cache is written under the lock, but the FAST PATH reads it
 * unlocked and uses ldar to observe `in_use` with acquire
 * ordering -- so once a reader sees `in_use == 1` it's
 * guaranteed to see all earlier-stored fields too. */
static inline int wmfont_load_in_use_acq(const int *p)
{
    int v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline void wmfont_store_in_use_rel(int *p, int v)
{
    __asm__ volatile("stlr %w0, [%1]" :: "r"(v), "r"(p) : "memory");
}

static inline void wmfont_store_ptr_rel(const uint8_t **p, const uint8_t *v)
{
    __asm__ volatile("stlr %0, [%1]" :: "r"(v), "r"(p) : "memory");
}

/* Drop the current conn and mark fontd down.  Subsequent
 * callers will retry srv_connect on entry. */
static void disconnect_locked(void)
{
    if (g_conn) {
        srv_unref_conn(g_conn, /*is_service_end=*/0);
        g_conn = NULL;
    }
    g_down = 1;
}

/* Open the conn if we don't have one.  Returns 0 on success,
 * -1 if fontd isn't bound yet (caller falls back).  Must hold
 * the lock. */
static int ensure_connected_locked(void)
{
    if (g_conn) return 0;
    int err = 0;
    g_conn = srv_connect(FONT_SOCK_PATH, &err);
    if (!g_conn) {
        g_conn = NULL;
        g_down = 1;
        return -1;
    }
    g_down = 0;
    return 0;
}

/* One request/reply cycle.  Returns 0 on success and copies
 * the reply into *out_reply (header) + *out_pixels (alpha,
 * kmalloc'd here; caller takes ownership).  Returns -1 on
 * any IPC or protocol error; on -1 the conn is torn down.
 * Must hold the lock. */
static int rpc_glyph_locked(uint32_t cp,
                            struct font_msg *out_reply,
                            uint8_t **out_pixels)
{
    *out_pixels = NULL;

    /* Build request header. */
    struct font_msg req;
    for (size_t i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.op        = FONT_OP_GLYPH;
    req.font_id   = FONT_ID_DEFAULT;
    req.codepoint = cp;
    req.size_px   = WM_FONT_SIZE_PX;

    long w = srv_write(g_conn, /*is_service_end=*/0, &req, sizeof(req));
    if (w != (long)sizeof(req)) {
        disconnect_locked();
        return -1;
    }

    /* Receive reply.  Allocate a max-size buffer up front
     * (32 KiB is the worst-case glyph bitmap; in practice we
     * see ~200 B per glyph).  Free it before returning. */
    const size_t max_reply = sizeof(struct font_msg) + 64u * 1024u;
    uint8_t *rbuf = (uint8_t *)kmalloc(max_reply);
    if (!rbuf) {
        disconnect_locked();
        return -1;
    }
    long n = srv_read(g_conn, /*is_service_end=*/0, rbuf, max_reply);
    if (n < (long)sizeof(struct font_msg)) {
        kfree(rbuf);
        disconnect_locked();
        return -1;
    }
    struct font_msg *r = (struct font_msg *)rbuf;
    if (r->op == FONT_OP_ERR || r->status != FONT_OK) {
        /* Fontd answered but rejected this glyph.  Don't
         * tear down the conn; just signal negative. */
        *out_reply = *r;
        kfree(rbuf);
        return -1;
    }
    if (r->op != FONT_OP_GLYPH) {
        kfree(rbuf);
        disconnect_locked();
        return -1;
    }

    *out_reply = *r;
    uint32_t pix_bytes = (uint32_t)r->bmp_w * (uint32_t)r->bmp_h;
    if (pix_bytes == 0) {
        /* Whitespace / empty glyph — no bitmap. */
        kfree(rbuf);
        return 0;
    }
    if ((long)pix_bytes > n - (long)sizeof(struct font_msg)) {
        /* Reply was truncated.  Treat as protocol error. */
        kfree(rbuf);
        disconnect_locked();
        return -1;
    }
    uint8_t *alpha = (uint8_t *)kmalloc(pix_bytes);
    if (!alpha) {
        kfree(rbuf);
        return -1;
    }
    const uint8_t *src = rbuf + sizeof(struct font_msg);
    for (uint32_t i = 0; i < pix_bytes; i++) alpha[i] = src[i];
    kfree(rbuf);
    *out_pixels = alpha;
    return 0;
}

/* Move a fetched reply into the cache slot.  Publish order:
 * write the bitmap pointer LAST so a racy reader either sees
 * (in_use=0, pixels=NULL) or the fully populated entry. */
static void publish_entry(uint32_t cp, struct wm_font_entry *e,
                          const struct font_msg *r, uint8_t *pixels)
{
    e->gi.bitmap_w     = r->bmp_w;
    e->gi.bitmap_h     = r->bmp_h;
    e->gi.left_bearing = r->left_bearing;
    e->gi.top_bearing  = r->top_bearing;
    e->gi.advance      = r->advance ? r->advance : 1;
    wmfont_store_ptr_rel((const uint8_t **)&e->gi.pixels, pixels);
    wmfont_store_in_use_rel(&e->in_use, 1);
    (void)cp;
}

int wm_font_get_glyph(uint32_t cp, struct glyph_info *out)
{
    if (!out) return -1;
    if (cp >= WM_FONT_CACHE_N) return -1;     /* fall back to bitmap */

    /* Fast path: cache hit, no lock. */
    struct wm_font_entry *e = &g_cache[cp];
    if (wmfont_load_in_use_acq(&e->in_use)) {
        if (e->negative) return -1;
        *out = e->gi;
        return 0;
    }

    /* Cache miss — serialise IPC. */
    wmfont_lock();

    /* Recheck after acquiring the lock — another thread may
     * have populated while we waited. */
    if (wmfont_load_in_use_acq(&e->in_use)) {
        int neg = e->negative;
        struct glyph_info gi = e->gi;
        wmfont_unlock();
        if (neg) return -1;
        *out = gi;
        return 0;
    }

    /* Try to (re)connect, then do one RPC. */
    if (ensure_connected_locked() != 0) {
        wmfont_unlock();
        return -1;
    }

    struct font_msg reply;
    /* Default reply state so a transport-level failure (rpc
     * returns -1 without filling reply) isn't mistaken for an
     * explicit NOGLYPH; only the latter should poison the
     * cache slot.  Also silences -Wmaybe-uninitialized: GCC
     * can't see through rpc_glyph_locked's contract that
     * reply is filled on the *_ERR path. */
    for (size_t k = 0; k < sizeof(reply); k++) ((uint8_t *)&reply)[k] = 0;
    uint8_t        *pixels = NULL;
    int rc = rpc_glyph_locked(cp, &reply, &pixels);
    if (rc != 0) {
        /* Persistent negative for this cp if fontd answered
         * with FONT_ERR_NOGLYPH; transient miss otherwise.
         * We mark the slot in_use+negative only on explicit
         * NOGLYPH so a transient disconnect doesn't poison
         * the cache forever. */
        if (reply.op == FONT_OP_ERR && reply.status == FONT_ERR_NOGLYPH) {
            e->negative = 1;
            e->gi.pixels = NULL;
            e->gi.bitmap_w = 0;
            e->gi.bitmap_h = 0;
            e->gi.advance  = 0;
            wmfont_store_in_use_rel(&e->in_use, 1);
        }
        wmfont_unlock();
        return -1;
    }

    publish_entry(cp, e, &reply, pixels);
    struct glyph_info gi = e->gi;
    wmfont_unlock();
    *out = gi;
    return 0;
}

uint32_t wm_font_cell_height(void)
{
    return WM_FONT_CELL_HEIGHT;
}

uint32_t wm_font_baseline_offset(void)
{
    return WM_FONT_BASELINE;
}

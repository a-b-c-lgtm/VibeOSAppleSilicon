/*
 * userspace/wsd/wsd.c — chapter 117, window-server daemon.
 *
 * Chapter 117 built this daemon in stages: the initial
 * scanout claim (SYS_FB_MAP_SCANOUT) and /srv/wm bus setup,
 * then per-window framebuffer allocation (SYS_WIN_FB_ALLOC +
 * WM_WIN_MAP_FB), then damage-driven composition
 * (WM_WIN_DAMAGE + compose_all), then position tracking and
 * WM_WIN_MOVE, and finally the full app cutover — all GUI
 * apps ported to wmclient (/srv/wm), the kernel compositor
 * retired.  Chapter 118 layered decorations, resize, and
 * the compose-based cursor model on top.
 *
 * Concurrency model
 * -----------------
 *
 * Still single-threaded.  All ops are non-blocking and
 * complete in microseconds.  The window table is global
 * (`g_windows[]`) but since the only writer is the
 * single accept thread, there's no locking needed.  The
 * fontd worker-per-conn pattern lifts in cleanly when
 * Phase D adds the blocking WM_EVENT_PULL.
 *
 * Ownership and cleanup
 * ---------------------
 *
 * Each window remembers the cfd of the conn that created
 * it.  When a conn EOFs (clean or otherwise), wsd walks
 * `g_windows[]` and destroys every entry tagged with that
 * cfd.  This is the daemon's contribution to crash-
 * recovery: a client that dies mid-session leaves no
 * stale window state behind.  init's supervisor handles
 * the wsd-side equivalent (the kernel auto-tears down
 * /srv/wm on wsd exit, the supervisor respawns wsd, the
 * new instance starts with an empty `g_windows[]`).
 *
 * Lifecycle
 * ---------
 *
 * Supervised by init (userspace/init/init.c) the same way
 * fontd and clipboardd are.  If we crash, init respawns us.
 * The kernel's wsd_fb_release_owner() runs on our thread
 * exit and clears the FB owner slot, so the respawn's call
 * to fb_map_scanout returns 0 rather than -EBUSY.  Chapter
 * 107's srv_bind likewise tears down the /srv/wm endpoint
 * automatically on our exit, so a respawn's bind sees a
 * clean slate.
 *
 * Reliability note
 * -----------------
 *
 * We deliberately do NOT exit on any failure of
 * fb_map_scanout.  Boot-time races are possible: if init
 * brings wsd up before fb_init has finished (early-boot
 * scheduling order is not guaranteed), the call returns
 * -EAGAIN.  Looping with yield() is cheap and lets us claim
 * the FB the moment it becomes available, without forcing a
 * supervisor-restart churn.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/thread.h"
#include "../libc/wm_proto.h"
#include "../libgui/draw.h"

/* chapter 118 -- decoration geometry.  Title bar above each
 * decorated window, no left/right/bottom border for now (a 1-px
 * border looked gappy at the launcher's gray background and would
 * just be more pixels for the same UX).
 *
 * chapter 118 -- two title-bar buttons, both inset 2 px from
 * the bar edges:
 *   - close    (rightmost)  : red X, kills the window via
 *                             GUI_EVENT_CLOSE delivery.
 *   - minimize (to its left): blue underscore, hides the
 *                             window from compose + hit-test
 *                             (taskbar click restores).
 *
 * Body origin (the pixel where the client's FB starts on the
 * scanout) is (w->x, w->y + deco_top_h(w)).  An undecorated
 * window has deco_top_h == 0, so wsd-without-decoration
 * behaviour is byte-identical to chapter 117 for
 * panels (taskbar, desktop wallpaper, notify popups). */
#define WSD_TITLE_H        24u
#define WSD_CLOSE_BTN_W    20u
#define WSD_MIN_BTN_W      20u
#define WSD_BTN_GAP         2u   /* gap between minimize and close */
#define WSD_BTN_INSET       2u

/* chapter 118 -- resize.  The grip is a 12x12 square in the
 * bottom-right corner of decorated, RESIZABLE-flagged
 * windows.  Painted as three diagonal hairlines so it's
 * visually distinct from the close/minimize buttons up top.
 * The hard minima below cap how small the user can drag a
 * window: title bar must still fit two buttons + title text.
 * The maxima are NOT capped here -- wsd clamps to the
 * original FB allocation in resize_apply (the FB never
 * grows past its create-time size). */
#define WSD_GRIP_SIZE      12u
#define WSD_GRIP_FG        0xffd0d0d0u  /* light gray */
#define WSD_RESIZE_MIN_W   80u
#define WSD_RESIZE_MIN_H   40u

/* Theme.  Two colours per "active vs idle" pair.  The values
 * are picked to read cleanly against the dark-navy wallpaper
 * (0xff112233) and to land high enough on the BGRA brightness
 * scale that the close-button glyph still stands out.  Future
 * Phase E themesel app will read these from /etc/wsd/theme. */
#define WSD_DECO_BG_ACTIVE     0xff3a6ea5u  /* steel blue   */
#define WSD_DECO_BG_IDLE       0xff556677u  /* dim gray-blue */
#define WSD_DECO_FG            0xffffffffu  /* white         */
#define WSD_CLOSE_BG_HOVER     0xffaa3030u  /* red-ish       */
#define WSD_CLOSE_BG_IDLE      0xff883333u  /* darker red    */
#define WSD_CLOSE_FG           0xffffffffu  /* white X       */
#define WSD_MIN_BG_IDLE        0xff336688u  /* navy-blue     */
#define WSD_MIN_FG             0xffffffffu  /* white "_"     */
#define WSD_BORDER_COLOR       0xff202020u  /* almost black  */

/* Returns the pixel height of decoration above the body for
 * window w.  0 for undecorated windows (panels), WSD_TITLE_H
 * for everything else.  Used in three places: blit_full_window,
 * handle_damage's coord translation, and the input-poller's
 * hit-test. */
static inline uint32_t deco_top_h(uint32_t flags)
{
    return (flags & WM_WF_NODECORATION) ? 0u : WSD_TITLE_H;
}

/* Walltime helper isn't necessary; we just yield until the FB
 * is up.  yield() is cheap and the kernel scheduler will pick
 * other runnable threads when we have nothing to do. */
static void wait_for_fb_then_map(struct fb_map_args *out)
{
    /* Cap the loop so a configuration bug (no GPU at all)
     * eventually surfaces as a serial message instead of a
     * silent hang.  At ~1ms per yield (idle CPU) this caps
     * the wait at roughly 30s, which is well past the longest
     * observed fb_init time in CI (single-digit ms even on
     * cold boot). */
    for (uint32_t tries = 0; tries < 30000; tries++) {
        int r = fb_map_scanout(out);
        if (r == 0) return;
        if (errno != EAGAIN) {
            /* Real failure (e.g. EBUSY: another process won
             * the race).  Report once, then keep retrying so
             * a future release_owner gives us a chance. */
            printf("[wsd] fb_map_scanout failed: %s try=%u\n",
                   strerror(errno), tries);
        }
        yield();
    }
    printf("[wsd] gave up waiting for framebuffer after 30000 tries\n");
}

/* Monotonic session counter.  Bumped per accept; passed back
 * to the client in WM_HELLO so future phases can use it as a
 * per-client handle (e.g. for input-event routing).  Wraps
 * at 2^32 in theory, never in practice (boot lifetime). */
static uint32_t g_next_session_id = 1u;

/* Chapter 117 — scanout state, cached after wait_for_fb_then_map.
 * Used by handle_damage as the destination of every blit.
 * g_scanout_va==0 means "FB not yet up" — any compose op
 * answered while in that state replies WM_ERR_NOTIMPL (a
 * conservative default; should never actually happen because
 * the bind-ready banner only prints AFTER FB-map succeeds). */
static uint64_t g_scanout_va     = 0;
static uint32_t g_scanout_w      = 0;
static uint32_t g_scanout_h      = 0;
static uint32_t g_scanout_stride = 0;

/* The window table.  Fixed-size array, slot-allocated.  Why
 * not a linked list: WM_MAX_WINDOWS=64 is tiny, an array
 * lets WM_LIST walk in cache-friendly order, and the
 * "first-free-slot" allocator collapses to a 64-iteration
 * loop that's trivially correct.  A linked list buys
 * nothing at this scale.
 *
 * Each entry's `in_use==0` slot can be re-used by the next
 * WM_WIN_CREATE; the global g_next_win_id (below) ensures
 * the same numeric id is never recycled, so a stale client
 * reference to a destroyed window gets WM_ERR_NOSUCHWIN
 * instead of touching a different window. */
struct wm_window {
    int      in_use;          /* slot allocated? */
    uint32_t id;              /* WM_WIN_* id, monotonic */
    int      owner_cfd;       /* cfd of the conn that created it */
    uint32_t owner_session;   /* session_id of that conn (for WM_LIST) */
    uint32_t w;               /* requested width  in pixels */
    uint32_t h;               /* requested height in pixels */
    uint32_t flags;           /* WM_WF_* bitmask */
    /* Per-window shareable FB (chapter 117).  Allocated at
     * CREATE, freed at DESTROY / gc.  fb_id == 0 means "no
     * backing yet"; the only reason that can be true today
     * is a WIN_FB_ALLOC failure during CREATE (we still
     * return the window so a client can DESTROY and retry,
     * but MAP_FB will return WM_ERR_NOMEM). */
    uint32_t fb_id;
    uint32_t fb_stride;       /* bytes per row (= fb_w*4) */
    uint32_t fb_size;         /* total mapped bytes (page-aligned) */
    uint64_t fb_va;           /* wsd-side VA (for future compose) */
    /* chapter 118 -- the FB is allocated ONCE at CREATE
     * for fb_w x fb_h pixels and never reallocated.  The
     * logical w/h above can shrink within that envelope on
     * resize and grow back up to (fb_w, fb_h) but never
     * past.  Apps that want a generous resize range should
     * request a larger window than they initially display
     * and shrink at startup -- the same trick we use for
     * the browser. */
    uint32_t fb_w;
    uint32_t fb_h;
    /* Scanout-relative origin (chapter 117).  Assigned at
     * CREATE via cascade; mutated by WM_WIN_MOVE.  Used by
     * handle_damage to translate window-local source
     * coords into scanout destination coords. */
    uint32_t x;
    uint32_t y;
    /* Caller-provided title for WM_LIST / taskbar.
     * 64 bytes (matches wm_win_desc.title).  Set
     * via WM_WIN_TITLE; empty string if the client never
     * called WM_WIN_TITLE.  NUL-terminated. */
    char     title[64];
    /* chapter 118 -- kernel-WM "input shadow" id, set by
     * WM_WIN_BIND_KERNEL.  -1 means "not bound" (no shadow
     * == no input routing, e.g. for output-only notify
     * popups).  Used by:
     *   - the input-poller drag path, to call
     *     SYS_GUI_MOVE_WINDOW so body hit-testing follows
     *     wsd's title-bar drag.
     *   - the close-button click path, to inject
     *     GUI_EVENT_CLOSE via SYS_GUI_DELIVER_EVENT.
     * No kernel-side ownership of this slot in wsd -- wsd
     * never destroys the shadow; the client's process exit
     * tears down the shadow via wm_destroy_owner. */
    int32_t  kernel_id;
    /* chapter 118 -- minimize state.  Set when the user
     * clicks the minimize button (or WM_WIN_RESTORE is
     * called with the opposite intent).  Hidden windows are
     * skipped from compose (no pixels), from hit-test (no
     * input), and from focus.  Still listed in WM_LIST so
     * the taskbar can show them and offer click-to-restore.
     * Restoration also calls gui_set_minimized(kid, 0) so
     * the kernel WM's focus tracking stays in sync. */
    int      hidden;
};
static struct wm_window g_windows[WM_MAX_WINDOWS];

/* chapter 118 -- explicit wsd-side z-order.  Each entry is a
 * slot index into g_windows[]; the array is bottom-to-top, so
 * g_z_order[0] is the backmost in-use window and
 * g_z_order[g_z_count - 1] is the topmost.  Maintained by:
 *
 *   - create_window_impl   : pushes the new slot at the top
 *   - destroy_window       : removes the slot
 *   - z_raise_slot         : moves the slot to the top
 *
 * Used by:
 *   - wsd_compose_all      : paints back-to-front, so the
 *                            topmost wsd window visually wins
 *   - hit_test_topmost     : iterates top-to-bottom
 *
 * Slot indices are stable for a window's lifetime (find_window
 * relies on g_windows[i].in_use); g_z_order just renumbers
 * THEM, not the slots.  Pinned-to-bottom windows (wallpaper)
 * always live at index 0, pinned-to-top (none today) would
 * always live at g_z_count - 1; this isn't enforced yet
 * because no wsd-managed window currently carries those flags
 * (the kernel WM still owns the wallpaper). */
static int      g_z_order[WM_MAX_WINDOWS];
static uint32_t g_z_count = 0;

/* Insert `slot` into g_z_order.  PIN_BOTTOM windows always
 * land at index 0 (bottom of stack); everything else lands
 * at the top.  PIN_BOTTOM is honoured here so the desktop's
 * wallpaper -- which is the first window created and would
 * otherwise sit at index 0 by accident of creation order --
 * stays at the bottom even after subsequent NO_DECORATION /
 * PIN_BOTTOM ordering changes. */
static void z_push(int slot)
{
    /* Defensive: don't double-insert. */
    for (uint32_t i = 0; i < g_z_count; i++)
        if (g_z_order[i] == slot) return;
    uint32_t flags = (slot >= 0 && slot < (int)WM_MAX_WINDOWS)
                     ? g_windows[slot].flags : 0u;
    if (flags & WM_WF_PIN_BOTTOM) {
        /* Shift everyone up by one, insert at index 0. */
        for (uint32_t i = g_z_count; i > 0; i--)
            g_z_order[i] = g_z_order[i - 1];
        g_z_order[0] = slot;
        g_z_count++;
        return;
    }
    g_z_order[g_z_count++] = slot;
}

static void z_remove(int slot)
{
    for (uint32_t i = 0; i < g_z_count; i++) {
        if (g_z_order[i] == slot) {
            for (uint32_t j = i + 1; j < g_z_count; j++)
                g_z_order[j - 1] = g_z_order[j];
            g_z_count--;
            return;
        }
    }
}

/* Move `slot` to the top of g_z_order.  No-op if already top
 * or not in the list.  Also no-op for PIN_BOTTOM windows --
 * the wallpaper must NEVER come above an app window, no
 * matter how it was clicked / focused.  Returns 1 only when
 * the z-order actually changed (so callers can avoid a
 * redundant kernel-side gui_raise_window IPC). */
static int z_raise(int slot)
{
    if (g_z_count == 0) return 0;
    if (slot >= 0 && slot < (int)WM_MAX_WINDOWS
        && (g_windows[slot].flags & WM_WF_PIN_BOTTOM)) {
        return 0;
    }
    if (g_z_order[g_z_count - 1] == slot) return 0;
    int found = 0;
    for (uint32_t i = 0; i < g_z_count; i++) {
        if (g_z_order[i] == slot) { found = 1; break; }
    }
    if (!found) return 0;
    z_remove(slot);
    z_push(slot);
    return 1;
}

/* Chapter 117 — one big mutex around every handler.  Now that
 * each accepted connection runs on its own worker thread
 * (via thread_spawn_files so the cfd is shared), every read/
 * write of g_windows[], the cascade counters, the scanout
 * cache, and compose_all() must serialise.  A single mutex
 * suffices because ops are microseconds and the contention
 * is low (one click, one damage rect at a time per app).
 *
 * Held by:
 *   - handle_* functions in their entirety
 *   - serve_conn around the dispatch switch
 *   - gc_conn_windows
 * NOT held by:
 *   - the accept loop (it only touches the listening fd)
 *   - the per-conn read() before dispatch (the syscall is
 *     itself the blocking point, not the handler) */
static mutex_t g_wsd_lock = MUTEX_INIT;

/* Chapter 117 — auto-position cascade.  Each CREATE picks up
 * the current (g_cascade_x, g_cascade_y) and advances by 40
 * px on each axis; if the next position would push the
 * incoming window off the scanout, the cascade wraps back
 * to the starting offset.  Cheap, predictable, and matches
 * mid-90s window-server behaviour (CDE, Motif).  Clients
 * that want a specific position issue WM_WIN_MOVE after
 * CREATE. */
#define WM_CASCADE_BASE_X   100u
#define WM_CASCADE_BASE_Y   100u
#define WM_CASCADE_STEP     40u
static uint32_t g_cascade_x = WM_CASCADE_BASE_X;
static uint32_t g_cascade_y = WM_CASCADE_BASE_Y;

/* Window-id allocator.  Strictly monotonic across the
 * lifetime of this wsd process.  Phase D's input router
 * will use ids as map keys; making them strictly monotonic
 * (rather than reusing slot indices) means a router cache
 * miss on a destroyed window is unambiguous. */
static uint32_t g_next_win_id = 1u;

/* Per-connection state passed down to handlers.  Holds the
 * cfd (for write() target) and the session id assigned by
 * the conn's WM_HELLO.  session_id stays 0 until HELLO
 * succeeds; CREATE before HELLO is treated as a protocol
 * error so a future ACL layer (chapter 132+) has somewhere
 * to hook in. */
struct wm_conn {
    int      cfd;
    uint32_t session_id;
};

/* Look up a slot by window id.  Returns NULL if not
 * found.  Caller is responsible for checking owner_cfd
 * before mutating. */
static struct wm_window *find_window(uint32_t id)
{
    if (id == 0) return NULL;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].id == id) {
            return &g_windows[i];
        }
    }
    return NULL;
}

/* Forward decls so handle_create / handle_destroy /
 * handle_move (defined before blit_to_scanout) can call the
 * compositor helpers (defined after blit_to_scanout).  We could
 * move blit_to_scanout up instead, but the file already reads
 * "small static helpers near the top, handlers in the middle,
 * compositor at the bottom"; keeping that shape and using a
 * forward decl is the smaller change. */
static void wsd_compose_all(void);
static void paint_wallpaper(void);
static void blit_full_window(const struct wm_window *w);

/* Garbage-collect windows owned by a connection that just
 * went away.  Called from serve_conn() right before we
 * close the cfd, so any stale window state can't outlive
 * the client.  Logs a one-liner if anything was reaped so
 * a leak (client forgot DESTROY before exit) is visible in
 * boot logs.  Also frees any per-window FB (chapter 117) so
 * the kernel's win_fb table doesn't accumulate orphans. */
static void gc_conn_windows(int cfd)
{
    int reaped = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].owner_cfd == cfd) {
            if (g_windows[i].fb_id != 0) {
                (void)win_fb_free(g_windows[i].fb_id);
                g_windows[i].fb_id = 0;
            }
            g_windows[i].in_use = 0;
            /* chapter 118 -- yank this slot from the z-order
             * so subsequent hit-tests + composes don't index it. */
            z_remove((int)i);
            reaped++;
        }
    }
    if (reaped > 0) {
        printf("[wsd] gc cfd=%d reaped %d window(s)\n", cfd, reaped);
        /* Repaint scanout so the reaped windows'
         * old pixels are replaced by wallpaper. */
        wsd_compose_all();
    }
}

/* Handle one WM_HELLO request.  Validates client_version
 * against WM_PROTO_VERSION; replies with the assigned
 * session_id and wsd's own version.  Stamps the per-conn
 * session_id so subsequent ops on this cfd carry it into
 * the window table. */
static void handle_hello(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_HELLO;

    /* Strict-equal check: any version mismatch is a bug worth
     * surfacing immediately.  Once a deployed client legitimately
     * lags wsd by a minor version, relax to
     * "client_version <= WM_PROTO_VERSION". */
    if (req->a != WM_PROTO_VERSION) {
        rep.status = WM_ERR_BADVER;
        printf("[wsd] WM_HELLO bad version: client=%u wsd=%u\n",
               (unsigned)req->a, (unsigned)WM_PROTO_VERSION);
    } else {
        rep.status = WM_OK;
        c->session_id = g_next_session_id++;
        rep.a = c->session_id;
        rep.b = WM_PROTO_VERSION;
    }
    write(c->cfd, &rep, sizeof(rep));
}

/* Handle one WM_LIST request.  Chapter 117: walks g_windows[]
 * and packs an entry per in-use slot directly after the
 * reply header in the same datagram.  Chapter-107 IPC is
 * datagram-oriented, so the whole reply (header + payload)
 * goes out in a single write().  Clients sized their read
 * buffer to WM_LIST_REPLY_MAX so the worst case fits. */
static void handle_list(struct wm_conn *c, const struct wm_msg *req)
{
    (void)req;
    /* Worst-case payload: header + WM_MAX_WINDOWS desc.
     * Stack-allocated; about 1.5 KiB. */
    uint8_t buf[sizeof(struct wm_msg)
                + WM_MAX_WINDOWS * sizeof(struct wm_win_desc)];
    struct wm_msg *rep = (struct wm_msg *)buf;
    struct wm_win_desc *descs =
        (struct wm_win_desc *)(buf + sizeof(struct wm_msg));

    /* Memset by hand — freestanding C, no libc memset. */
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_windows[i].in_use) continue;
        descs[n].win_id        = g_windows[i].id;
        descs[n].owner_session = g_windows[i].owner_session;
        descs[n].w             = g_windows[i].w;
        descs[n].h             = g_windows[i].h;
        /* chapter 118 -- expose the minimized state to
         * WM_LIST clients (notably the taskbar) by ORing in
         * GUI_WIN_FLAG_MINIMIZED.  The wsd-side hidden bit
         * IS the source of truth -- the kernel's bit (set via
         * gui_set_minimized) is downstream of this one. */
        descs[n].flags         = g_windows[i].flags
                               | (g_windows[i].hidden
                                  ? GUI_WIN_FLAG_MINIMIZED : 0u);
        descs[n].x             = g_windows[i].x;
        descs[n].y             = g_windows[i].y;
        /* Copy the (possibly empty) title.  Fixed
         * 64 bytes; trailing NUL is guaranteed by handle_title
         * (and by the zero-init at CREATE for windows that
         * never called it). */
        for (uint32_t t = 0; t < sizeof(descs[n].title); t++)
            descs[n].title[t] = g_windows[i].title[t];
        n++;
    }

    rep->op     = WM_LIST;
    rep->status = WM_OK;
    rep->a      = n;

    write(c->cfd, buf,
          sizeof(struct wm_msg) + n * sizeof(struct wm_win_desc));
}

/* Core helper — shared body of WM_WIN_CREATE
 * and WM_WIN_CREATE_AT.  If `use_pos` is non-zero, the
 * window lands at (px, py) and the cascade counter is left
 * alone; if zero, the cascade is consulted (and advanced).
 * Returns the wsd window id on success, 0 on failure (with
 * the appropriate WM_ERR_* already written into *rep). */
static uint32_t create_window_impl(struct wm_conn *c,
                                   uint32_t w, uint32_t h,
                                   uint32_t flags,
                                   int use_pos,
                                   uint32_t px, uint32_t py,
                                   struct wm_msg *rep)
{
    if (c->session_id == 0) {
        rep->status = WM_ERR_PROTO;
        return 0;
    }
    if (w == 0 || h == 0) {
        rep->status = WM_ERR_PROTO;
        return 0;
    }

    int slot = -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_windows[i].in_use) { slot = (int)i; break; }
    }
    if (slot < 0) {
        rep->status = WM_ERR_FULL;
        return 0;
    }

    g_windows[slot].in_use        = 1;
    g_windows[slot].id            = g_next_win_id++;
    g_windows[slot].owner_cfd     = c->cfd;
    g_windows[slot].owner_session = c->session_id;
    g_windows[slot].w             = w;
    g_windows[slot].h             = h;
    g_windows[slot].flags         = flags;
    g_windows[slot].fb_id         = 0;
    g_windows[slot].fb_stride     = 0;
    g_windows[slot].fb_size       = 0;
    g_windows[slot].fb_va         = 0;
    /* chapter 118 -- fb_w/fb_h are the immutable allocation
     * dimensions.  Set them to the create-time w/h up front
     * so resize can cap against them even if win_fb_alloc
     * below fails (it then stays a no-op cap because
     * fb_id==0 == "no backing, no resize possible"). */
    g_windows[slot].fb_w          = w;
    g_windows[slot].fb_h          = h;
    /* Zero the title so an unset title reads
     * as the empty string in WM_LIST.  No memset in
     * freestanding C; manual loop. */
    for (uint32_t t = 0; t < sizeof(g_windows[slot].title); t++)
        g_windows[slot].title[t] = 0;
    /* chapter 118 -- no kernel shadow bound until the client
     * calls WM_WIN_BIND_KERNEL.  Output-only popups (notify)
     * never bind, and that's fine; their close path stays
     * "client exits, gc reaps". */
    g_windows[slot].kernel_id     = -1;
    /* chapter 118 -- created visible.  Minimize button (or
     * an explicit WM_WIN_RESTORE with hide intent in the
     * future) sets this. */
    g_windows[slot].hidden        = 0;

    if (use_pos) {
        /* Caller-supplied position.  Cascade counter
         * untouched so subsequent cascade-positioned clients
         * keep their existing layout. */
        g_windows[slot].x = px;
        g_windows[slot].y = py;
    } else {
        if (g_scanout_w != 0
            && (g_cascade_x + w > g_scanout_w
             || g_cascade_y + h > g_scanout_h)) {
            g_cascade_x = WM_CASCADE_BASE_X;
            g_cascade_y = WM_CASCADE_BASE_Y;
        }
        g_windows[slot].x = g_cascade_x;
        g_windows[slot].y = g_cascade_y;
        g_cascade_x += WM_CASCADE_STEP;
        g_cascade_y += WM_CASCADE_STEP;
    }

    struct win_fb_alloc_args fa;
    int r = win_fb_alloc(w, h, &fa);
    if (r == 0) {
        g_windows[slot].fb_id     = fa.id;
        g_windows[slot].fb_stride = fa.stride;
        g_windows[slot].fb_size   = fa.size;
        g_windows[slot].fb_va     = fa.va;
        /* chapter 118 -- fb_w/fb_h track the CURRENT
         * allocation (not a fixed cap).  Resize grows them
         * via win_fb_resize. */
        g_windows[slot].fb_w      = w;
        g_windows[slot].fb_h      = h;
    } else {
        printf("[wsd] win_fb_alloc(w=%u h=%u) failed err=%d\n",
               (unsigned)w, (unsigned)h, r);
    }

    /* chapter 118 -- newly created windows go to the top of
     * the wsd z-order so they're visible above existing ones,
     * matching every WIMP convention.  Subsequent click-to-
     * raise reorders this further. */
    z_push(slot);

    rep->status = WM_OK;
    rep->a      = g_windows[slot].id;
    rep->b      = g_windows[slot].x;
    rep->c      = g_windows[slot].y;
    return g_windows[slot].id;
}

/* Handle one WM_WIN_CREATE.  Requires that the conn has
 * completed WM_HELLO (so we have a valid session_id to
 * stamp on the window).  w/h must both be non-zero; we
 * impose no upper bound yet beyond the framebuffer size
 * being uncheckable at this point (the kernel WM may
 * still grow buffers; Phase D will tighten this when wsd
 * starts mapping the buffer). */
static void handle_create(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_CREATE;

    if (create_window_impl(c, req->a, req->b, req->c,
                           0, 0, 0, &rep) == 0) {
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    write(c->cfd, &rep, sizeof(rep));

    /* The new window has no pixels yet (client
     * paints + DAMAGEs separately) but it does occupy scanout
     * real estate, so refresh from scratch.  This also makes
     * the first window after boot trigger the initial fb_present
     * if we somehow missed the startup one. */
    wsd_compose_all();
}

/* Handle one WM_WIN_CREATE_AT.  Identical to WM_WIN_CREATE
 * except the client supplies (x, y) via the d field
 * (high16=x, low16=y) and the cascade counter is not
 * advanced.  Used by apps that own a specific scanout slot
 * (wallpaper at 0,0; taskbar at 0,scanout_h-bar; etc) and
 * don't want to perturb where the next cascade-positioned
 * client lands. */
static void handle_create_at(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_CREATE_AT;

    uint32_t px = (req->d >> 16) & 0xFFFFu;
    uint32_t py = (req->d >>  0) & 0xFFFFu;

    if (create_window_impl(c, req->a, req->b, req->c,
                           1, px, py, &rep) == 0) {
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    write(c->cfd, &rep, sizeof(rep));

    /* Same recompose rationale as handle_create. */
    wsd_compose_all();
}

/* Handle one WM_WIN_DESTROY.  Only the conn that created
 * the window may destroy it.  Cross-conn destroys get
 * WM_ERR_NOTOWNER; the asymmetry matters once Phase D's
 * input router can sit between two clients.  GC-on-conn-
 * close (above) is the safety net for the common case
 * where a client just exits without an explicit DESTROY. */
static void handle_destroy(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_DESTROY;

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->owner_cfd != c->cfd) {
        rep.status = WM_ERR_NOTOWNER;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    /* Release the backing FB before we drop the
     * slot.  Order matters: free first (uninstalls client
     * mapping if any), then mark slot free.  If the client
     * already unmapped its end, free is still cheap. */
    if (w->fb_id != 0) {
        (void)win_fb_free(w->fb_id);
        w->fb_id = 0;
    }
    /* chapter 118 -- find the slot index and pull it out of
     * the z-order BEFORE clearing in_use, so z_remove can
     * still locate the slot by direct address arithmetic
     * (g_windows is a contiguous array; slot = w - g_windows). */
    int slot = (int)(w - g_windows);
    z_remove(slot);
    w->in_use = 0;
    rep.status = WM_OK;
    write(c->cfd, &rep, sizeof(rep));

    /* Window's old pixels need to be replaced by
     * wallpaper (or by whatever window underneath now shows
     * through).  Full recompose is the easiest correct
     * answer; an optimised compositor would just repaint the
     * window's old rect.  Save that for a later slice. */
    wsd_compose_all();
}

/* Handle one WM_WIN_MAP_FB.  Returns the kernel win-fb id
 * and the geometry so the client can sanity-check before
 * calling SYS_WIN_FB_MAP to install the pages into its own
 * AS.  Ownership-gated: only the conn that created the
 * window may map its FB.  Cross-conn map attempts get
 * WM_ERR_NOTOWNER. */
static void handle_map_fb(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_MAP_FB;

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->owner_cfd != c->cfd) {
        rep.status = WM_ERR_NOTOWNER;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->fb_id == 0) {
        /* Backing allocation failed at CREATE.  Client can
         * DESTROY and retry. */
        rep.status = WM_ERR_NOMEM;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }

    rep.status = WM_OK;
    rep.a = w->fb_id;
    rep.b = w->w;
    rep.c = w->h;
    rep.d = w->fb_stride;
    write(c->cfd, &rep, sizeof(rep));
}

/* Chapter 117 — blit `rw` x `rh` pixels from `src` (BGRA,
 * stride `src_stride`) into the scanout at (`dx`, `dy`).
 * Both buffers are mapped into wsd's AS (the scanout via
 * SYS_FB_MAP_SCANOUT, the source via SYS_WIN_FB_ALLOC). Byte-by-byte copy because freestanding
 * C has no memcpy and the pixel counts here are small enough
 * that the loop cost is invisible next to the IPC round-
 * trip.  Returns the BGRA value of the first dst pixel
 * AFTER the blit for the caller to use in a verification
 * log line; returns 0 if the rect was fully clipped away. */
static uint32_t blit_to_scanout(const uint8_t *src, uint32_t src_stride,
                                uint32_t dx, uint32_t dy,
                                uint32_t rw, uint32_t rh)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)g_scanout_va;
    for (uint32_t y = 0; y < rh; y++) {
        uint8_t       *drow = dst + (size_t)(dy + y) * g_scanout_stride
                                 + (size_t)dx * 4u;
        const uint8_t *srow = src + (size_t)y * src_stride;
        for (uint32_t x = 0; x < rw * 4u; x++) drow[x] = srow[x];
    }
    uint8_t *first = dst + (size_t)dy * g_scanout_stride
                         + (size_t)dx * 4u;
    return  (uint32_t)first[0]
         | ((uint32_t)first[1] <<  8)
         | ((uint32_t)first[2] << 16)
         | ((uint32_t)first[3] << 24);
}

/* Chapter 117 — solid-colour wallpaper.  Chosen distinct from
 * any window's magic colour the tests use (hellowsd's
 * 0xff7755aa, wmtest's 0xff332211) so a screenshot test
 * could later distinguish "wallpaper" from "wsd-painted
 * window".  BGRA bytes in memory: 0x33, 0x22, 0x11, 0xff —
 * R=0x11 G=0x22 B=0x33 on virtio-gpu's B8G8R8X8 format,
 * which reads as a dark navy. */
#define WSD_WALLPAPER_BGRA 0xff112233u

static void paint_wallpaper(void)
{
    if (g_scanout_va == 0) return;
    uint32_t *p   = (uint32_t *)(uintptr_t)g_scanout_va;
    uint32_t n_px = g_scanout_w * g_scanout_h;
    for (uint32_t i = 0; i < n_px; i++) p[i] = WSD_WALLPAPER_BGRA;
}

/* chapter 118 -- build a transient gui_fb over the scanout so
 * we can call libgui/draw.h primitives (draw_fill_rect,
 * draw_text, draw_blit_bgra) to paint decoration.  Cheap to
 * construct; lives on the stack of each compose call so wsd
 * doesn't need a long-lived gui_fb global to keep in sync
 * with g_scanout_*. */
static struct gui_fb scanout_fb(void)
{
    struct gui_fb fb;
    fb.pixels = (uint8_t *)(uintptr_t)g_scanout_va;
    fb.stride = g_scanout_stride;
    fb.w      = g_scanout_w;
    fb.h      = g_scanout_h;
    fb.id     = 0;
    return fb;
}

/* chapter 118 -- the cursor sprite, an 11x18 X11-style left-
 * pointer arrow.  '.' transparent, 'X' black border, '#' white
 * fill.  Hotspot is (0, 0): the tip of the arrow corresponds
 * to the actual pointer position the kernel reports.
 *
 * chapter 118 -- to keep cursor movement smooth, wsd does NOT
 * full-recompose on every pixel-of-motion.  Instead it uses a
 * compose-based cursor: cursor_move_only re-composes just the
 * union of (old, new) cursor rects from window/wallpaper
 * sources, then overlays the sprite at the new position.  Two
 * tiny GPU submits (the recompose write + a single fb_present)
 * instead of a 1280x800 wallpaper repaint.  Full compose
 * (wsd_compose_all) is reserved for window state changes
 * (CREATE, DESTROY, drag, raise). */
#define WSD_CURSOR_W   11u
#define WSD_CURSOR_H   18u
static const char WSD_CURSOR_BITMAP[WSD_CURSOR_H][WSD_CURSOR_W] = {
    "X..........",
    "XX.........",
    "X#X........",
    "X##X.......",
    "X###X......",
    "X####X.....",
    "X#####X....",
    "X######X...",
    "X#######X..",
    "X########X.",
    "X#########X",
    "X######XXXX",
    "X###X##X...",
    "X##X.X##X..",
    "X#X..X##X..",
    "XX....X##X.",
    "X.....X##X.",
    ".......XX.."
};

/* Tracked separately from the windows so move-only updates
 * (pointer moved over the wallpaper) can be detected and
 * recomposed cheaply.  Initialised to -1/-1 so the first
 * pointer_state call always counts as a change. */
static int32_t  g_cursor_x = -1;
static int32_t  g_cursor_y = -1;
static uint32_t g_cursor_btn = 0;

/* chapter 118 -- compose-based cursor model.
 *
 * Prior versions used a "save-under" buffer that captured the
 * pixels behind the sprite before painting and restored them
 * before each move.  That model is the X11 / classic-Mac
 * approach and is O(sprite_area) per move, but it has a
 * subtle correctness pitfall: if the save buffer EVER captures
 * stale or corrupt pixels (because some path mutated the
 * scanout outside the cursor's restore-then-save-then-paint
 * critical section), the corruption gets resurrected on the
 * next cursor move and persists until something else paints
 * over it.  The user reported exactly this symptom in chapter
 * 109b -- text content under the cursor's path stays distorted
 * "forever" until enough motion forces a full recompose.
 *
 * The compose-based model eliminates the save buffer entirely.
 * On every cursor move we compose_rect the union of (old, new)
 * cursor positions, which reconstructs the background from the
 * actual window FBs + wallpaper.  Then we overlay the sprite
 * at the new position.  Cost goes up modestly (one compose_rect
 * per move instead of two memcpy() pairs) but every move
 * RECOMPUTES the background from canonical sources -- so no
 * stale-buffer staleness is possible.  Stack: GPU > sprite >
 * window content > wallpaper, recomputed bottom-up every move.
 *
 * The sprite-only paint primitive (paint_cursor_sprite below)
 * does NOT modify any cached state.  It just OR's the sprite
 * pixels onto the scanout at the given position.  Restoring is
 * not a separate operation any more -- composing the same area
 * "restores" by recomputing the background.
 */

/* Overlay the sprite at (sx, sy).  No state tracked -- caller
 * is responsible for ensuring the background underneath was
 * freshly composed (via compose_rect or wsd_compose_all)
 * BEFORE this call, otherwise the sprite lands on top of
 * whatever stale pixels were there.  Used by:
 *   - wsd_compose_all (after wallpaper+windows are painted)
 *   - cursor_move_only (after compose_rect of the union rect)
 *   - handle_damage (after compose_rect of the damage rect)
 * Pixels outside the scanout are silently skipped (the cursor
 * is allowed to overlap the bottom-right edge).  Sprite
 * pixels: 'X' = black opaque, '#' = white opaque, '.' =
 * transparent (no write). */
static void paint_cursor_sprite(int32_t sx, int32_t sy)
{
    struct gui_fb fb = scanout_fb();
    for (uint32_t cy = 0; cy < WSD_CURSOR_H; cy++) {
        for (uint32_t cx = 0; cx < WSD_CURSOR_W; cx++) {
            char ch = WSD_CURSOR_BITMAP[cy][cx];
            uint32_t color;
            switch (ch) {
            case 'X': color = 0xff000000u; break;
            case '#': color = 0xffffffffu; break;
            default:  continue; /* '.' transparent */
            }
            int32_t px = sx + (int32_t)cx;
            int32_t py = sy + (int32_t)cy;
            if (px < 0 || py < 0) continue;
            if ((uint32_t)px >= fb.w || (uint32_t)py >= fb.h) continue;
            uint32_t *row =
                (uint32_t *)(fb.pixels + (size_t)py * fb.stride);
            row[px] = color;
        }
    }
}

/* Paint the cursor sprite at the current cursor position.
 * No-op if the cursor has never moved (g_cursor_x/y < 0).
 * Replaces the chapter-109 paint_cursor that also captured a
 * save-under buffer (now removed -- compose-based model). */
static void paint_cursor(void)
{
    if (g_scanout_va == 0) return;
    if (g_cursor_x < 0 || g_cursor_y < 0) return;
    paint_cursor_sprite(g_cursor_x, g_cursor_y);
}

/* Move the cursor sprite to (new_x, new_y).  Re-composes the
 * union of (old, new) cursor rects from window content +
 * wallpaper (which erases the old sprite as a side effect by
 * overwriting it with real background), then overlays the
 * sprite at the new position.  Falls back to a full compose
 * the first time (when there's no previous position). */
static void cursor_move_only(int32_t new_x, int32_t new_y);

/* Paint the title bar + close button for a decorated window.
 * Title text comes from w->title (set via WM_WIN_TITLE, used by
 * every wmclient app since chapter 117).  Title bar bg
 * picks the "active" colour when the window is the topmost
 * non-pinned one in slot order (rough proxy for focus until we
 * grow real focus tracking), idle otherwise.  Close button is
 * a solid red rect with a 2-px white X drawn via two diagonal
 * line passes.
 *
 * chapter 118 follow-up -- the (cx, cy, cw, ch) clip rect
 * confines every paint to its intersection with the clip
 * rect, in scanout coords.  This matters when compose_rect
 * re-decorates a BACK window during a partial redraw: without
 * clipping, the back window's full-width bar paint would
 * spill outside the damage rect and overwrite foreground
 * window pixels that the per-window body blit (which is
 * properly clipped to the damage rect) never gets a chance
 * to repair.  Pass (0, 0, scanout_w, scanout_h) for a full-
 * scanout repaint. */
static int rects_intersect_clip(int32_t cx, int32_t cy,
                                int32_t cw, int32_t ch,
                                int32_t *x, int32_t *y,
                                int32_t *w, int32_t *h)
{
    int32_t x0 = *x, y0 = *y;
    int32_t x1 = x0 + *w, y1 = y0 + *h;
    int32_t cx1 = cx + cw, cy1 = cy + ch;
    if (x0 < cx)  x0 = cx;
    if (y0 < cy)  y0 = cy;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0;
    *w = x1 - x0; *h = y1 - y0;
    return 1;
}

/* draw_fill_rect intersected with the clip rect before drawing.
 * No-op if the rect is fully outside the clip. */
static void cfill_rect(struct gui_fb *fb,
                       int32_t cx, int32_t cy,
                       int32_t cw, int32_t ch,
                       int32_t x, int32_t y,
                       uint32_t w, uint32_t h, uint32_t bgra)
{
    int32_t rx = x, ry = y;
    int32_t rw = (int32_t)w, rh = (int32_t)h;
    if (!rects_intersect_clip(cx, cy, cw, ch, &rx, &ry, &rw, &rh))
        return;
    draw_fill_rect(fb, rx, ry, (uint32_t)rw, (uint32_t)rh, bgra);
}

/* Single-pixel write clipped against the clip rect AND the
 * scanout.  Used for the per-pixel close/min/grip glyphs. */
static inline void cput_pixel(struct gui_fb *fb,
                              int32_t cx, int32_t cy,
                              int32_t cw, int32_t ch,
                              int32_t px, int32_t py, uint32_t bgra)
{
    if (px < cx || py < cy)         return;
    if (px >= cx + cw || py >= cy + ch) return;
    if (px < 0 || py < 0)           return;
    if (px >= (int32_t)fb->w || py >= (int32_t)fb->h) return;
    ((uint32_t *)(fb->pixels + (size_t)py * fb->stride))[px] = bgra;
}

static void paint_decoration_clipped(const struct wm_window *w,
                                     int is_focused,
                                     int32_t cx, int32_t cy,
                                     int32_t cw, int32_t ch)
{
    if (w->flags & WM_WF_NODECORATION) return;
    if (g_scanout_va == 0) return;

    struct gui_fb fb = scanout_fb();
    uint32_t bar_w  = w->w;
    uint32_t bar_h  = WSD_TITLE_H;
    int32_t  bar_x  = (int32_t)w->x;
    int32_t  bar_y  = (int32_t)w->y;
    uint32_t bg     = is_focused ? WSD_DECO_BG_ACTIVE : WSD_DECO_BG_IDLE;

    /* Early bar-vs-clip rejection: if the bar rect doesn't
     * intersect the clip rect at all, every paint below is a
     * no-op, so skip the work entirely.  Grip is the only
     * decoration not inside the bar; we re-check below. */
    int32_t bar_check_x = bar_x, bar_check_y = bar_y;
    int32_t bar_check_w = (int32_t)bar_w, bar_check_h = (int32_t)bar_h;
    int bar_visible = rects_intersect_clip(cx, cy, cw, ch,
                                           &bar_check_x, &bar_check_y,
                                           &bar_check_w, &bar_check_h);

    if (bar_visible) {
        /* Title bar background. */
        cfill_rect(&fb, cx, cy, cw, ch,
                   bar_x, bar_y, bar_w, bar_h, bg);

        /* 1-px bottom border so the bar is visually separated
         * from the body even when the body's first row happens
         * to be the same shade as the bar bg. */
        cfill_rect(&fb, cx, cy, cw, ch,
                   bar_x, bar_y + (int32_t)bar_h - 1,
                   bar_w, 1u, WSD_BORDER_COLOR);

        /* Title text -- left-padded by 8 px, vertically
         * centred in the bar.  We use draw_text_clipped so
         * the per-pixel clip rect rejects writes outside the
         * dirty region on all four sides without relying on
         * a sub-fb trick.  An earlier sub-fb-shifted-origin
         * version was buggy: it could not represent a
         * left-edge clip without dropping the entire string,
         * which left holes in the title text wherever the
         * cursor swept across the bar past the text start.
         * Each cursor_move_only compose painted bar bg over
         * the swept strip, the text was skipped, and the
         * launcher's periodic damage (which DID include the
         * bar's left edge) only arrived if the launcher had
         * a reason to re-render. */
        if (w->title[0] != 0) {
            int32_t tx = bar_x + 8;
            int32_t ty = bar_y +
                         (int32_t)((bar_h - DRAW_TEXT_CELL_H) / 2u);
            draw_text_clipped(&fb, tx, ty, w->title,
                              WSD_DECO_FG, bg, 1,
                              bar_check_x, bar_check_y,
                              bar_check_w, bar_check_h);
        }

        /* Close button.  Only drawn if the bar is wide enough
         * to fit it without overlapping the title. */
        if (bar_w >= WSD_CLOSE_BTN_W + 2 * WSD_BTN_INSET) {
            int32_t cb_x = bar_x + (int32_t)bar_w - (int32_t)WSD_CLOSE_BTN_W
                         - (int32_t)WSD_BTN_INSET;
            int32_t cb_y = bar_y + (int32_t)WSD_BTN_INSET;
            uint32_t cb_h = bar_h - 2u * WSD_BTN_INSET;
            cfill_rect(&fb, cx, cy, cw, ch,
                       cb_x, cb_y, WSD_CLOSE_BTN_W, cb_h,
                       WSD_CLOSE_BG_IDLE);
            int32_t inset = 4;
            for (int32_t i = 0; i < (int32_t)cb_h - 2 * inset; i++) {
                int32_t x0 = cb_x + inset + i;
                int32_t y0 = cb_y + inset + i;
                int32_t x1 = cb_x + (int32_t)WSD_CLOSE_BTN_W - 1 - inset - i;
                int32_t y1 = cb_y + inset + i;
                cput_pixel(&fb, cx, cy, cw, ch, x0, y0, WSD_CLOSE_FG);
                cput_pixel(&fb, cx, cy, cw, ch, x1, y1, WSD_CLOSE_FG);
            }

            /* Minimize button immediately left of close. */
            uint32_t both_w = WSD_MIN_BTN_W + WSD_BTN_GAP
                            + WSD_CLOSE_BTN_W + 2 * WSD_BTN_INSET;
            if (bar_w >= both_w) {
                int32_t mb_x = cb_x - (int32_t)WSD_BTN_GAP
                                    - (int32_t)WSD_MIN_BTN_W;
                int32_t mb_y = cb_y;
                uint32_t mb_h = cb_h;
                cfill_rect(&fb, cx, cy, cw, ch,
                           mb_x, mb_y, WSD_MIN_BTN_W, mb_h,
                           WSD_MIN_BG_IDLE);
                int32_t glyph_inset_x = 4;
                int32_t glyph_y0 = mb_y + (int32_t)mb_h - 6;
                int32_t glyph_y1 = glyph_y0 + 1;
                int32_t glyph_x0 = mb_x + glyph_inset_x;
                int32_t glyph_x1 = mb_x + (int32_t)WSD_MIN_BTN_W
                                        - glyph_inset_x;
                for (int32_t gy = glyph_y0; gy <= glyph_y1; gy++) {
                    for (int32_t gx = glyph_x0; gx < glyph_x1; gx++) {
                        cput_pixel(&fb, cx, cy, cw, ch,
                                   gx, gy, WSD_MIN_FG);
                    }
                }
            }
        }
    }

    /* chapter 118 -- resize grip in the bottom-right corner
     * of the WINDOW (not the title bar) for RESIZABLE windows.
     * Lives outside the bar so we evaluate it independently
     * of bar_visible. */
    if (w->flags & GUI_WIN_FLAG_RESIZABLE) {
        int32_t gw = (int32_t)WSD_GRIP_SIZE;
        int32_t grip_x0 = (int32_t)w->x + (int32_t)w->w - gw;
        int32_t grip_y0 = (int32_t)w->y + (int32_t)deco_top_h(w->flags)
                        + (int32_t)w->h - gw;
        for (int32_t k = 0; k < 3; k++) {
            int32_t off = (int32_t)(2 + 4 * k);  /* 2, 6, 10 */
            for (int32_t t = 0; t < gw - off; t++) {
                int32_t px = grip_x0 + off + t;
                int32_t py = grip_y0 + gw - 1 - t;
                cput_pixel(&fb, cx, cy, cw, ch, px, py, WSD_GRIP_FG);
            }
        }
    }
}

/* Backwards-compatible wrapper: paint with a clip rect that
 * covers the entire scanout, matching the historical
 * "paint everything" semantics that wsd_compose_all wants. */
static void paint_decoration(const struct wm_window *w, int is_focused)
{
    paint_decoration_clipped(w, is_focused,
                             0, 0,
                             (int32_t)g_scanout_w,
                             (int32_t)g_scanout_h);
}

/* Blit a whole window into the scanout at (w->x, w->y).
 * Clipped against scanout bounds (a CREATE'd or MOVE'd
 * window can be partially off-screen; both axes are
 * checked).  Used by wsd_compose_all for from-scratch
 * repaints — handle_damage has its own per-rect path
 * that doesn't go through this helper.  Skips slots with
 * no backing FB (e.g. a CREATE whose WIN_FB_ALLOC failed
 * but that returned a slot for later DESTROY).
 *
 * chapter 118 -- decorated windows place their body at
 * (w->x, w->y + WSD_TITLE_H); the title bar covers
 * (w->x, w->y) ... (w->x + w->w, w->y + WSD_TITLE_H).
 * Decoration paint is delegated to paint_decoration so
 * compose_all can choose a per-window focused flag. */
static void blit_full_window(const struct wm_window *w)
{
    if (w->fb_id == 0 || w->fb_va == 0) return;
    if (w->x >= g_scanout_w || w->y >= g_scanout_h) return;
    uint32_t body_y = w->y + deco_top_h(w->flags);
    if (body_y >= g_scanout_h) return;
    uint32_t rw = w->w;
    uint32_t rh = w->h;
    if (w->x + rw > g_scanout_w) rw = g_scanout_w - w->x;
    if (body_y + rh > g_scanout_h) rh = g_scanout_h - body_y;
    const uint8_t *src = (const uint8_t *)(uintptr_t)w->fb_va;
    (void)blit_to_scanout(src, w->fb_stride, w->x, body_y, rw, rh);
}

/* Repaint the entire scanout: wallpaper, then every in-use
 * window in slot order.  Called from CREATE / DESTROY /
 * MOVE / startup / gc — anything that changes scanout
 * geometry globally.  handle_damage skips this and does
 * a single-rect blit + targeted fb_present instead because
 * its hot path is "client painted a few pixels and wants
 * them on screen now" and a full screen rewrite would be
 * 4 MB of stores per call on a 1280x800 scanout.
 *
 * Z order is currently slot-allocation order, which means
 * later-created windows obscure earlier ones.  Phase D
 * gains WM_WIN_RAISE / WM_WIN_LOWER and the order
 * becomes an explicit z field on the slot. */
static void wsd_compose_all(void)
{
    if (g_scanout_va == 0) return;

    paint_wallpaper();
    int painted = 0;

    /* chapter 118 -- paint in explicit z-order (back to
     * front) so the topmost wsd window is the last drawn and
     * therefore visually wins.  Focused = topmost decorated
     * window in z-order.
     * chapter 118 -- hidden (minimized) windows are skipped
     * for both focus-selection and paint -- they contribute
     * nothing visually until WM_WIN_RESTORE flips hidden off. */
    int focused_slot = -1;
    for (int i = (int)g_z_count - 1; i >= 0; i--) {
        int s = g_z_order[i];
        if (!g_windows[s].in_use) continue;
        if (g_windows[s].hidden)  continue;
        if (g_windows[s].flags & WM_WF_NODECORATION) continue;
        focused_slot = s;
        break;
    }
    for (uint32_t i = 0; i < g_z_count; i++) {
        int s = g_z_order[i];
        if (!g_windows[s].in_use) continue;
        if (g_windows[s].hidden)  continue;
        blit_full_window(&g_windows[s]);
        paint_decoration(&g_windows[s], s == focused_slot);
        painted++;
    }
    /* chapter 118 -- cursor sprite floats above every
     * window.  Painted last so it's never occluded.  The
     * compose-based model means no save-under buffer is
     * maintained here -- the next cursor_move_only will
     * recompose the union of (current, new) cursor rects
     * to erase this sprite. */
    paint_cursor();
    /* fb_present(0,0,0,0) means "the whole scanout".
     * Cheaper to ask the GPU to flush once at the end than
     * once per window, even though we'd technically know
     * each window's rect — virtio-gpu's submit cost
     * dwarfs the per-row DMA. */
    (void)fb_present(0, 0, 0, 0);
    printf("[wsd] compose_all painted=%d\n", painted);
}

/* chapter 118 -- z-order-respecting partial compose.  Paints
 * the wallpaper, then every in-use window in z-order back-to-
 * front, all clipped to the rect (rx, ry, rw, rh).  Used by
 * handle_damage so a background window updating its body
 * doesn't smear pixels over foreground windows that overlap
 * the damaged rect.  Cost is proportional to the rect area
 * times the number of windows that intersect it, which is
 * typically 1-2 windows; the wallpaper paint dominates for
 * full-window damages but is bounded by the rect size.
 *
 * The full wsd_compose_all path remains the right answer for
 * any state change (CREATE, DESTROY, MOVE, raise) because
 * those affect the whole scanout.  compose_rect is the
 * targeted fast path for "an app drew some pixels and wants
 * them on screen now" where state hasn't changed. */
static void compose_rect(int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    if (g_scanout_va == 0) return;
    if (rw <= 0 || rh <= 0) return;
    /* Clip the rect against the scanout. */
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx >= (int32_t)g_scanout_w || ry >= (int32_t)g_scanout_h) return;
    if (rx + rw > (int32_t)g_scanout_w) rw = (int32_t)g_scanout_w - rx;
    if (ry + rh > (int32_t)g_scanout_h) rh = (int32_t)g_scanout_h - ry;
    if (rw <= 0 || rh <= 0) return;

    /* 1. Wallpaper rect: solid colour so we can just fill the
     *    bytes directly without going through paint_wallpaper
     *    (which paints the whole scanout). */
    {
        uint8_t *dst = (uint8_t *)(uintptr_t)g_scanout_va;
        for (int32_t y = 0; y < rh; y++) {
            uint32_t *row = (uint32_t *)(dst
                + (size_t)(ry + y) * g_scanout_stride
                + (size_t)rx * 4u);
            for (int32_t x = 0; x < rw; x++) row[x] = WSD_WALLPAPER_BGRA;
        }
    }

    /* 2. Walk windows back-to-front (z-order).  For each, find
     *    the intersection of its body rect with the dirty
     *    rect.  Blit just that intersection from the window's
     *    FB to the scanout.  Then if the window is decorated,
     *    repaint the title bar with the dirty rect as a clip
     *    so the bar paint never spills outside the damaged
     *    region (chapter 118 follow-up fix: without the clip,
     *    a back window's full-bar repaint would overwrite
     *    foreground-window pixels that the per-window body
     *    blit -- properly clipped to the damage rect -- never
     *    gets a chance to repair, leaving the back bar
     *    visible "on top of" the foreground body).
     *    chapter 118 -- hidden (minimized) windows contribute
     *    nothing visually; skip them entirely so the wallpaper
     *    fill (step 1) stays visible where they used to be. */
    int focused_slot = -1;
    for (int i = (int)g_z_count - 1; i >= 0; i--) {
        int s = g_z_order[i];
        if (!g_windows[s].in_use) continue;
        if (g_windows[s].hidden)  continue;
        if (g_windows[s].flags & WM_WF_NODECORATION) continue;
        focused_slot = s;
        break;
    }
    for (uint32_t i = 0; i < g_z_count; i++) {
        int s = g_z_order[i];
        if (!g_windows[s].in_use) continue;
        if (g_windows[s].hidden)  continue;
        const struct wm_window *ww = &g_windows[s];

        int32_t wx = (int32_t)ww->x;
        int32_t wy = (int32_t)ww->y + (int32_t)deco_top_h(ww->flags);
        int32_t ww_w = (int32_t)ww->w;
        int32_t ww_h = (int32_t)ww->h;

        /* Body intersection with dirty rect.  Skipped if the
         * window has no backing FB yet (CREATE whose alloc
         * failed, or a window momentarily between resize
         * Phase 5 reinstall and the client's remap response):
         * the wallpaper fill above already painted the dirty
         * rect, so leaving the body unblit just shows
         * wallpaper through where the body would be -- which
         * is the correct fallback for "no pixels available".
         * Decoration is painted unconditionally below because
         * the title bar is wsd-owned: bg colour, title text,
         * close/min buttons, resize grip are all drawn from
         * constants and don't read from the window's FB. */
        if (ww->fb_id != 0 && ww->fb_va != 0) {
            int32_t ix0 = wx > rx ? wx : rx;
            int32_t iy0 = wy > ry ? wy : ry;
            int32_t ix1 = (wx + ww_w) < (rx + rw) ? (wx + ww_w) : (rx + rw);
            int32_t iy1 = (wy + ww_h) < (ry + rh) ? (wy + ww_h) : (ry + rh);
            if (ix0 < ix1 && iy0 < iy1) {
                int32_t blit_w = ix1 - ix0;
                int32_t blit_h = iy1 - iy0;
                int32_t sx = ix0 - wx;
                int32_t sy = iy0 - wy;
                const uint8_t *src = (const uint8_t *)(uintptr_t)ww->fb_va
                                   + (size_t)sy * ww->fb_stride
                                   + (size_t)sx * 4u;
                (void)blit_to_scanout(src, ww->fb_stride,
                                      (uint32_t)ix0, (uint32_t)iy0,
                                      (uint32_t)blit_w, (uint32_t)blit_h);
            }
        }

        /* Decoration repaint -- clipped to the dirty rect.
         * paint_decoration_clipped skips paints that fall
         * entirely outside the clip and trims those that
         * straddle it.  The resize grip is checked
         * independently of the bar rect (it lives in the
         * window body), so we always call regardless of
         * whether the bar itself overlaps the dirty rect.
         * Independent of fb_va -- decoration is wsd-painted
         * from constants. */
        if (!(ww->flags & WM_WF_NODECORATION)) {
            paint_decoration_clipped(ww, s == focused_slot,
                                     rx, ry, rw, rh);
        }
    }
}

/* chapter 118 -- cursor-only repaint, COMPOSE-BASED.
 *
 * Reconstructs the background under the union of (old, new)
 * cursor rects from the actual window FBs + wallpaper (via
 * compose_rect), then overlays the sprite at the new position.
 * Erasure of the old sprite happens implicitly -- compose_rect
 * paints whatever the canonical content is for those pixels,
 * which by definition isn't sprite.
 *
 * Cost per move:
 *   - compose_rect(union):  one full title bar repaint per
 *     overlapping bar (~25 KB writes for an 800-px wide bar)
 *     + body blits clipped to union + wallpaper fill clipped
 *     to union.  For a 1-px cursor motion the union is
 *     ~12x19 px = 228 px; for a 20-px motion across a 1024-
 *     px-wide window the bar paint dominates but still
 *     comfortably under 5% CPU at 100 ticks/s.
 *   - paint_cursor_sprite:  198 pixel touches max.
 *   - fb_present(union):    one virtio-gpu submit.
 *
 * In exchange we get: no save-buffer state machine, no
 * possibility of stale-buffer staleness propagating to the
 * scanout, and the cursor always lands on top of CANONICAL
 * pixel content (recomputed from sources, not cached).
 */
static void cursor_move_only(int32_t new_x, int32_t new_y)
{
    if (g_scanout_va == 0) return;

    int32_t old_x = g_cursor_x;
    int32_t old_y = g_cursor_y;

    /* No motion since last tick -- nothing to do.  The sprite
     * is already on screen at (old_x, old_y) == (new_x, new_y). */
    if (old_x == new_x && old_y == new_y && old_x >= 0) return;

    /* Compute the union of the OLD and NEW sprite rects.  If
     * there's no previous position (first cursor paint after
     * boot), just use the new rect. */
    int32_t ux0, uy0, ux1, uy1;
    if (old_x < 0 || old_y < 0) {
        ux0 = new_x;
        uy0 = new_y;
        ux1 = new_x + (int32_t)WSD_CURSOR_W;
        uy1 = new_y + (int32_t)WSD_CURSOR_H;
    } else {
        ux0 = old_x < new_x ? old_x : new_x;
        uy0 = old_y < new_y ? old_y : new_y;
        int32_t ox1 = old_x + (int32_t)WSD_CURSOR_W;
        int32_t oy1 = old_y + (int32_t)WSD_CURSOR_H;
        int32_t nx1 = new_x + (int32_t)WSD_CURSOR_W;
        int32_t ny1 = new_y + (int32_t)WSD_CURSOR_H;
        ux1 = ox1 > nx1 ? ox1 : nx1;
        uy1 = oy1 > ny1 ? oy1 : ny1;
    }

    /* Repaint the union from canonical sources.  Erases the
     * old sprite (by overwriting with real background) AND
     * prepares the new area (so the sprite lands on the right
     * canonical pixels). */
    compose_rect(ux0, uy0, ux1 - ux0, uy1 - uy0);

    /* Overlay the sprite at the new position. */
    paint_cursor_sprite(new_x, new_y);

    g_cursor_x = new_x;
    g_cursor_y = new_y;

    /* chapter 118 -- present the FULL scanout.
     *
     * We could (and previously did) present just the union
     * rect.  But the QEMU cocoa display backend on macOS
     * Retina has been observed to drop or coalesce small
     * partial-rect flushes when they arrive close together,
     * leaving stale pixels visible in the host window even
     * though the guest framebuffer is correct (verifiable via
     * QMP screendump, which shows the guest FB pixel-perfect).
     * The user's reported "distortion stays forever until I
     * move the cursor enough" symptom matches this behaviour:
     * a stale display tile persists until a *bigger* flush
     * (e.g. an app damage covering more area) forces cocoa
     * to refresh that region.
     *
     * fb_present(0,0,0,0) means "flush the entire scanout".
     * Cost per move: one TRANSFER_TO_HOST_2D + one
     * RESOURCE_FLUSH covering 1920x1080x4 = 8 MB.  At our
     * ~100 Hz poller cap that's 800 MB/s, comfortably below
     * any modern memory bandwidth.  In exchange we get a
     * cocoa display that's guaranteed to be in sync with
     * the guest framebuffer. */
    (void)fb_present(0, 0, 0, 0);
}

/* Handle one WM_WIN_DAMAGE.  Validates ownership, decodes
 * the packed rect, clips against the window's own size AND
 * the scanout's size, then blits BGRA pixels from the
 * per-window FB to the scanout.  Logs the dst-side first
 * pixel post-blit so the test can pin the pattern that
 * actually landed (catches blit-direction bugs, stride
 * mismatches, AS-install regressions in either map). */
static void handle_damage(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_DAMAGE;

    if (g_scanout_va == 0) {
        /* FB never came up; compositor is a no-op. */
        rep.status = WM_ERR_NOTIMPL;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->owner_cfd != c->cfd) {
        rep.status = WM_ERR_NOTOWNER;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->fb_id == 0) {
        rep.status = WM_ERR_NOMEM;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }

    uint32_t dx = req->b;
    uint32_t dy = req->c;
    uint32_t rw = WM_DAMAGE_W(req->d);
    uint32_t rh = WM_DAMAGE_H(req->d);

    /* Chapter 117 — req.b/req.c are WINDOW-LOCAL source
     * offsets into w's per-window FB; the destination on
     * the scanout is (w->x + req.b, w->y + req.c).  This
     * matches libgui's gui_window_dirty(fb, x, y, w, h)
     * shape, so wmclient can wire the call straight through.
     * Earlier in chapter 117, req.b/req.c were treated as
     * scanout coords directly; that was only equivalent when
     * the window sat at the origin. */
    uint32_t sx = dx;
    uint32_t sy = dy;

    /* Clip the source rect against the window's own FB.
     * Reading past w->w*w->h would touch someone else's
     * memory (the next slot's FB, or unmapped pages). */
    if (sx >= w->w || sy >= w->h || rw == 0 || rh == 0) {
        rep.status = WM_OK;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (sx + rw > w->w) rw = w->w - sx;
    if (sy + rh > w->h) rh = w->h - sy;

    /* Translate to scanout coords.  chapter 118 -- shift
     * by deco_top_h so the body lands below the title bar
     * for decorated windows; undecorated (panels) keep
     * the chapter-108d pixel-perfect mapping. */
    uint32_t scan_x = w->x + sx;
    uint32_t scan_y = w->y + deco_top_h(w->flags) + sy;

    /* Then clip against the scanout. */
    if (scan_x >= g_scanout_w || scan_y >= g_scanout_h) {
        rep.status = WM_OK;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (scan_x + rw > g_scanout_w) rw = g_scanout_w - scan_x;
    if (scan_y + rh > g_scanout_h) rh = g_scanout_h - scan_y;

    /* chapter 118 -- compose just the dirty rect using the
     * z-order-aware compose_rect.  This replaces the earlier
     * chapter-108d direct-blit, which wrote a background window's pixels
     * straight to scanout even when foreground windows
     * overlapped the rect -- causing visible smearing until
     * the next full compose.  compose_rect paints wallpaper +
     * every overlapping window back-to-front clipped to the
     * dirty rect, so a damage from a buried window only
     * affects the parts of the rect that are actually
     * visible.  Decoration (title bar) is repainted by
     * compose_rect for any window whose bar overlaps the
     * rect.  chapter 118 -- the rect we expand to include
     * the title bar if any part of the body was damaged
     * (some apps paint just a body line; we still want the
     * bar to refresh for focus tracking when raise happens). */
    int32_t crect_x = (int32_t)w->x;
    int32_t crect_y = (int32_t)w->y;
    int32_t crect_w = (int32_t)w->w;
    int32_t crect_h = (int32_t)(deco_top_h(w->flags) + w->h);
    /* Tighten to the union of (damage rect) and (full bar)
     * when decorated, or just the damage rect when not.
     * Damage rect already in scanout coords as (scan_x,
     * scan_y, rw, rh). */
    if (!(w->flags & WM_WF_NODECORATION)) {
        /* Bar = (w->x, w->y, w->w, WSD_TITLE_H).  Union with
         * damage rect = (scan_x, scan_y, rw, rh). */
        int32_t bar_x0 = (int32_t)w->x;
        int32_t bar_y0 = (int32_t)w->y;
        int32_t bar_x1 = bar_x0 + (int32_t)w->w;
        int32_t bar_y1 = bar_y0 + (int32_t)WSD_TITLE_H;
        int32_t dmg_x0 = (int32_t)scan_x;
        int32_t dmg_y0 = (int32_t)scan_y;
        int32_t dmg_x1 = dmg_x0 + (int32_t)rw;
        int32_t dmg_y1 = dmg_y0 + (int32_t)rh;
        crect_x = bar_x0 < dmg_x0 ? bar_x0 : dmg_x0;
        crect_y = bar_y0 < dmg_y0 ? bar_y0 : dmg_y0;
        int32_t crect_x1 = bar_x1 > dmg_x1 ? bar_x1 : dmg_x1;
        int32_t crect_y1 = bar_y1 > dmg_y1 ? bar_y1 : dmg_y1;
        crect_w = crect_x1 - crect_x;
        crect_h = crect_y1 - crect_y;
    } else {
        crect_x = (int32_t)scan_x;
        crect_y = (int32_t)scan_y;
        crect_w = (int32_t)rw;
        crect_h = (int32_t)rh;
    }

    /* chapter 118 -- compose-based cursor.  Just compose
     * the damage rect (which paints fresh window/wallpaper
     * content there), then overlay the sprite if the cursor
     * overlaps the rect.  No save buffer to maintain.  If
     * the cursor DOESN'T overlap the damage rect, the sprite
     * is already on screen from a previous compose and we
     * don't need to repaint it. */
    compose_rect(crect_x, crect_y, crect_w, crect_h);

    /* If the cursor rect overlaps the damage rect, the
     * compose_rect just overwrote (part of) the sprite with
     * window content.  Re-overlay the sprite on top. */
    if (g_cursor_x >= 0 && g_cursor_y >= 0) {
        int32_t cx0 = g_cursor_x;
        int32_t cy0 = g_cursor_y;
        int32_t cx1 = cx0 + (int32_t)WSD_CURSOR_W;
        int32_t cy1 = cy0 + (int32_t)WSD_CURSOR_H;
        int32_t dx1 = crect_x + crect_w;
        int32_t dy1 = crect_y + crect_h;
        if (cx0 < dx1 && cx1 > crect_x
            && cy0 < dy1 && cy1 > crect_y) {
            paint_cursor_sprite(g_cursor_x, g_cursor_y);
        }
    }

    printf("[wsd] damage win=%u src=%u,%u,%u,%u dst=%u,%u,%u,%u px=0x%08x\n",
           (unsigned)w->id,
           (unsigned)sx, (unsigned)sy,
           (unsigned)rw, (unsigned)rh,
           (unsigned)scan_x, (unsigned)scan_y,
           (unsigned)rw, (unsigned)rh,
           (unsigned)*(uint32_t *)(uintptr_t)
               (g_scanout_va
                + (size_t)scan_y * g_scanout_stride
                + (size_t)scan_x * 4u));

    /* chapter 118 -- present the FULL scanout.  See the
     * detailed comment in cursor_move_only for why we don't
     * present partial rects: cocoa display dropped/coalesced
     * small flushes, leaving stale tiles visible even though
     * the guest framebuffer is correct.  Full-scanout flush
     * each frame is cheap (8 MB DMA, well under memory
     * bandwidth) and reliable. */
    (void)fb_present(0, 0, 0, 0);

    rep.status = WM_OK;
    write(c->cfd, &rep, sizeof(rep));
}

/* Chapter 117 — WM_WIN_MOVE.  Reposition a window on the
 * scanout.  Owner-only (cross-conn moves get NOTOWNER).
 * Position must be within the scanout (-EINVAL via
 * WM_ERR_PROTO if x >= scanout_w or y >= scanout_h --
 * partial-off-screen positions are fine, fully-off-screen
 * is rejected because it almost always means a client bug).
 * Does NOT trigger a recompose; the client is expected to
 * follow MOVE with a DAMAGE for the new position.  That
 * keeps wsd's per-op work O(rect), not O(window). */
static void handle_move(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_MOVE;

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->owner_cfd != c->cfd) {
        rep.status = WM_ERR_NOTOWNER;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    uint32_t nx = req->b, ny = req->c;
    if (g_scanout_w != 0
        && (nx >= g_scanout_w || ny >= g_scanout_h)) {
        rep.status = WM_ERR_PROTO;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    w->x = nx;
    w->y = ny;
    printf("[wsd] move win=%u to=%u,%u\n",
           (unsigned)w->id, (unsigned)nx, (unsigned)ny);
    rep.status = WM_OK;
    write(c->cfd, &rep, sizeof(rep));

    /* The window's pixels need to appear at the
     * new position and disappear from the old.  Full
     * recompose handles both in one pass; a tighter
     * implementation would compose just the union of the
     * old and new rects. */
    wsd_compose_all();
}

/* Chapter 117 — handle one WM_WIN_TITLE.  Wire format: req
 * header followed immediately by `title_len` payload bytes
 * (max WM_TITLE_MAX-1 ascii chars, NOT including a
 * terminating NUL the sender doesn't have to send).  We
 * read both pieces with a single read() into a stack
 * scratch buffer, copy the title into the window's slot,
 * and reply with WM_OK / -ERR.  Empty payloads (title_len
 * == 0) are allowed — they clear the title. */
static void handle_title(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_TITLE;

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->owner_cfd != c->cfd) {
        rep.status = WM_ERR_NOTOWNER;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    uint32_t n = req->b;
    if (n >= sizeof(w->title)) n = sizeof(w->title) - 1;

    if (n > 0) {
        char tmp[WM_TITLE_MAX];
        long got = read(c->cfd, tmp, n);
        if (got != (long)n) {
            /* Short read -- protocol violation.  Don't write
             * a reply (the conn is now poisoned); serve_conn
             * will tear it down. */
            return;
        }
        for (uint32_t t = 0; t < n; t++) w->title[t] = tmp[t];
    }
    /* Always terminate; if n < previous length the old tail
     * would still be readable otherwise. */
    for (uint32_t t = n; t < sizeof(w->title); t++) w->title[t] = 0;

    rep.status = WM_OK;
    write(c->cfd, &rep, sizeof(rep));
}

/* chapter 118 -- WM_WIN_BIND_KERNEL.  Records the kernel-WM
 * "input shadow" id in the wsd window so the input poller
 * can:
 *   - call SYS_GUI_MOVE_WINDOW to keep the shadow's
 *     hit-test rect aligned with the wsd window as the user
 *     title-bar-drags it.
 *   - call SYS_GUI_DELIVER_EVENT to inject a
 *     GUI_EVENT_CLOSE on close-button click.
 * Owner-only -- a rogue connection shouldn't be able to
 * remap someone else's window onto its own kernel shadow.
 * Negative kernel_id means "unbind"; useful if a future
 * client decides to shed its shadow mid-session. */
static void handle_bind_kernel(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_BIND_KERNEL;

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->owner_cfd != c->cfd) {
        rep.status = WM_ERR_NOTOWNER;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    w->kernel_id = (int32_t)req->b;
    /* Push the initial position to the kernel shadow so the
     * very first body click hit-tests at (w->x, w->y +
     * deco_top) rather than wherever the kernel placed it
     * via its own cascade.  Errors here are non-fatal (the
     * client wasn't notified, but the drag path will retry
     * on the next move). */
    if (w->kernel_id >= 0 && !(w->flags & WM_WF_NODECORATION)) {
        (void)gui_move_window(w->kernel_id,
                              (int32_t)w->x,
                              (int32_t)w->y + (int32_t)WSD_TITLE_H);
    }
    /* chapter 118 -- turn on input passthrough on the kernel
     * shadow.  After this call, the kernel's hit-tester
     * ignores this window entirely; wsd routes ALL pointer
     * events (DOWN/UP/MOVE) to it via gui_deliver_event,
     * picked by wsd-side z-order rather than kernel-side.
     * Errors are non-fatal -- if the kernel happens not to
     * support passthrough, routing falls back to the old
     * kernel-decides behaviour and the user just loses
     * click-to-raise reliability. */
    if (w->kernel_id >= 0) {
        long pr = gui_set_input_passthrough(w->kernel_id, 1);
        if (pr != 0) {
            printf("[wsd] passthrough(%d) failed: %ld\n",
                   (int)w->kernel_id, pr);
        }
    }
    /* chapter 118 -- on bind, raise the wsd window to the
     * top of wsd's z-order so the newly-bound client is the
     * front-most candidate for input.  Mirror to kernel by
     * calling gui_raise_window so keyboard focus follows. */
    int slot = (int)(w - g_windows);
    if (z_raise(slot) && w->kernel_id >= 0) {
        (void)gui_raise_window(w->kernel_id);
    } else if (w->kernel_id >= 0) {
        /* Even on no-op z_raise (already top), force kernel
         * focus to this shadow so keyboard goes here.  This
         * makes "spawn app -> type immediately" do what the
         * user expects without an intervening click. */
        (void)gui_raise_window(w->kernel_id);
    }
    printf("[wsd] bind win=%u kernel_id=%d\n",
           (unsigned)w->id, (int)w->kernel_id);
    rep.status = WM_OK;
    write(c->cfd, &rep, sizeof(rep));
    /* Recompose so the newly-bound window paints on top
     * immediately.  Cheaper than waiting for the client's
     * first DAMAGE. */
    wsd_compose_all();
}

/* chapter 118 -- WM_WIN_RESTORE.  Counterpart to the
 * minimize-button click (which is an internal poller path).
 * Called by the taskbar when the user clicks a minimized
 * window's cell to bring it back.  Clears the wsd-side
 * hidden bit, raises to the top of wsd's z-order, tells
 * the kernel WM to clear its own minimized bit (so keyboard
 * focus can return), and triggers a full recompose so the
 * restored window paints immediately.  Idempotent: a no-op
 * if the window is already visible.
 *
 * chapter 118 follow-up -- the "no-op when visible" branch
 * caused taskbar clicks on a non-minimized window's cell to
 * do nothing.  The taskbar dispatches WM_WIN_RESTORE on
 * every cell click (it doesn't track minimize state itself
 * across protocol boundaries), so a click meant to "bring
 * the obscured paint window forward" silently went nowhere.
 * Always raise + focus + recompose on restore now; the
 * hidden-bit clear and the kernel un-minimize are the only
 * parts gated on hidden==1.
 *
 * NOT owner-only: the taskbar is a different process from
 * the window's owner, so we deliberately allow any
 * authenticated /srv/wm client to restore any window.  This
 * is the same trust model as the close button (wsd injects
 * GUI_EVENT_CLOSE into someone else's window because the
 * user clicked our decoration). */
static void handle_restore(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_RESTORE;

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }

    /* PIN_BOTTOM (wallpaper) refuses to come forward -- same
     * reasoning as the kernel WM's wm_set_minimized refusing
     * to minimize PIN_BOTTOM windows.  Returns WM_OK so a
     * future taskbar that lists the wallpaper for debugging
     * doesn't fail loudly. */
    if (w->flags & WM_WF_PIN_BOTTOM) {
        rep.status = WM_OK;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }

    int was_hidden = w->hidden;
    if (was_hidden) {
        w->hidden = 0;
        if (w->kernel_id >= 0) {
            (void)gui_set_minimized(w->kernel_id, 0);
        }
    }

    /* Find the slot index for z_raise. */
    int slot = -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (&g_windows[i] == w) { slot = (int)i; break; }
    }
    if (slot >= 0) {
        (void)z_raise(slot);
        if (w->kernel_id >= 0) {
            /* Unconditionally raise the kernel shadow so
             * keyboard focus follows -- z_raise may return 0
             * (window was already topmost wsd-side) but the
             * kernel might still have focus on someone else
             * (e.g. the launcher, after the user clicked it
             * to launch this app).  Hand focus over now. */
            (void)gui_raise_window(w->kernel_id);
        }
    }
    printf("[wsd] restore win=%u kernel_id=%d was_hidden=%d\n",
           (unsigned)w->id, (int)w->kernel_id, was_hidden);
    wsd_compose_all();

    rep.status = WM_OK;
    write(c->cfd, &rep, sizeof(rep));
}

/* WM_WIN_MINIMIZE.  Symmetric to handle_restore: an
 * external (or self-) request to hide a window.  Sets the
 * wsd-side hidden bit so compose and hit-test skip it, and
 * mirrors the state into the kernel shadow via
 * gui_set_minimized so the kernel's focus tracker won't
 * keep keyboard input on a hidden window.  Idempotent: a
 * no-op on a window that's already hidden.
 *
 * NOT owner-only: a future "minimize all" panel button
 * should be able to hide windows it doesn't own.  Same
 * trust model as WM_WIN_RESTORE.  PIN_BOTTOM windows refuse
 * (the wallpaper can't minimize itself). */
static void handle_minimize(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op = WM_WIN_MINIMIZE;

    struct wm_window *w = find_window(req->a);
    if (!w) {
        rep.status = WM_ERR_NOSUCHWIN;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }
    if (w->flags & WM_WF_PIN_BOTTOM) {
        rep.status = WM_OK;
        write(c->cfd, &rep, sizeof(rep));
        return;
    }

    int was_hidden = w->hidden;
    if (!was_hidden) {
        w->hidden = 1;
        if (w->kernel_id >= 0) {
            (void)gui_set_minimized(w->kernel_id, 1);
        }
        wsd_compose_all();
    }
    printf("[wsd] minimize win=%u kernel_id=%d was_hidden=%d\n",
           (unsigned)w->id, (int)w->kernel_id, was_hidden);

    rep.status = WM_OK;
    write(c->cfd, &rep, sizeof(rep));
}

/* For ops we know about but don't implement yet, reply with
 * WM_ERR + WM_ERR_NOTIMPL so a forward-leaning client can
 * detect the gap.  For ops we don't know at all (future
 * client talking to old wsd), same answer — the client can
 * decide whether to downgrade or fail. */
static void handle_notimpl(struct wm_conn *c, const struct wm_msg *req)
{
    struct wm_msg rep = {0};
    rep.op     = WM_ERR;
    rep.status = WM_ERR_NOTIMPL;
    rep.a      = req->op;   /* echo the op so client can log */
    write(c->cfd, &rep, sizeof(rep));
}

/* Serve one connection until the client EOFs.  Sequential
 * — one req/rep round-trip at a time.  Tearing down the
 * conn on the first protocol error (short read) keeps us
 * from looping on a stream of junk.
 *
 * Chapter 117 — this runs on a worker thread spawned per
 * accept (via thread_spawn_files).  Multiple instances run
 * concurrently, one per live client; each dispatch call
 * takes g_wsd_lock so the shared window table stays
 * consistent.
 *
 * Per-conn state lives on the stack here so it dies
 * naturally when this function returns. */
static void serve_conn(int cfd)
{
    struct wm_conn c = { .cfd = cfd, .session_id = 0 };
    struct wm_msg req;
    for (;;) {
        /* Read OUTSIDE the lock so other connections aren't
         * blocked while this one is idle. */
        long n = read(cfd, &req, sizeof(req));
        if (n == 0) return;                   /* clean EOF */
        if (n < 0) {
            printf("[wsd] conn read err=%ld\n", n);
            return;
        }
        if (n < (long)sizeof(req)) {
            struct wm_msg rep = {0};
            rep.op     = WM_ERR;
            rep.status = WM_ERR_PROTO;
            write(cfd, &rep, sizeof(rep));
            return;
        }
        mutex_lock(&g_wsd_lock);
        switch (req.op) {
        case WM_HELLO:         handle_hello    (&c, &req); break;
        case WM_LIST:          handle_list     (&c, &req); break;
        case WM_WIN_CREATE:    handle_create   (&c, &req); break;
        case WM_WIN_CREATE_AT: handle_create_at(&c, &req); break;
        case WM_WIN_DESTROY:   handle_destroy  (&c, &req); break;
        case WM_WIN_MAP_FB:    handle_map_fb   (&c, &req); break;
        case WM_WIN_DAMAGE:    handle_damage   (&c, &req); break;
        case WM_WIN_MOVE:      handle_move     (&c, &req); break;
        case WM_WIN_TITLE:     handle_title    (&c, &req); break;
        case WM_WIN_BIND_KERNEL: handle_bind_kernel(&c, &req); break;
        case WM_WIN_RESTORE:   handle_restore  (&c, &req); break;
        case WM_WIN_MINIMIZE:  handle_minimize (&c, &req); break;
        default:               handle_notimpl  (&c, &req); break;
        }
        mutex_unlock(&g_wsd_lock);
    }
}

/* Worker thread entry.  Owns the cfd until it EOFs, then
 * runs gc_conn_windows under the global lock and closes
 * the fd.  Must call exit() at the end -- thread_spawn_files
 * doesn't synthesise a return-to-exit trampoline. */
static void conn_thread(void *arg)
{
    int cfd = (int)(intptr_t)arg;
    serve_conn(cfd);
    mutex_lock(&g_wsd_lock);
    gc_conn_windows(cfd);
    mutex_unlock(&g_wsd_lock);
    close(cfd);
    exit(0);
}

/* ── chapter 118 — input poller ─────────────────────────────
 *
 * Spawned once at startup (after the FB is mapped, before
 * accept).  Polls the kernel-WM pointer state at ~60 Hz, and
 * for each tick:
 *
 *   1. If (x, y) changed, runs cursor_move_only — restore
 *      saved-under pixels at the old cursor rect, capture
 *      new ones, paint the sprite, present two small rects.
 *      ~ 1 KB of memory traffic vs. a full 4 MB compose.
 *
 *   2. On left-button rising edge (0 -> 1), hit-tests every
 *      window in WSD'S OWN z-order (top-first).  A hit on the
 *      close button injects GUI_EVENT_CLOSE; a hit on the
 *      title bar starts a drag AND raises the window;
 *      a hit on the body raises the window AND injects
 *      GUI_EVENT_MOUSE_DOWN, all routed via
 *      SYS_GUI_DELIVER_EVENT.
 *
 *   3. Raise = (a) move target to top of g_z_order, (b) call
 *      gui_raise_window on its kernel shadow so the kernel
 *      focuses it for keyboard input.  The kernel shadows
 *      themselves are pointer-passthrough (SYS_GUI_SET_INPUT_
 *      PASSTHROUGH set by handle_bind_kernel) so wsd is the
 *      sole authority on which window receives clicks; the
 *      kernel never auto-routes pointer events to passthrough
 *      shadows.  Keyboard still uses the kernel's focus tracker.
 *
 *   4. While dragging, updates w->x / w->y from (cursor_x -
 *      drag_off_x, cursor_y - drag_off_y) and pushes the new
 *      body position to the kernel shadow via
 *      SYS_GUI_MOVE_WINDOW so future hit-tests stay aligned.
 *
 *   5. On left-button falling edge (1 -> 0), exits drag,
 *      delivers MOUSE_UP to the window that received the
 *      matching MOUSE_DOWN (tracked via g_press_slot so a
 *      release outside the original window still pairs).
 *
 *   6. Plain cursor move while a button is held: routes
 *      MOUSE_MOVE to the press target (window-locked).  Move
 *      while no button is held: routes MOUSE_MOVE to the
 *      topmost window under the cursor.
 *
 * All table reads and mutations happen under g_wsd_lock.  The
 * poller never blocks on a syscall longer than the sleep_ms
 * tick (the pointer_state syscall is non-blocking), so it
 * can't starve the accept loop. */

/* Hit-test helpers.  All coords are scanout pixels.  Decoration
 * runs from (w->x, w->y) ... (w->x + w->w, w->y + WSD_TITLE_H);
 * the close button is at the right end of the bar. */
static int point_in_titlebar(const struct wm_window *w,
                             int32_t px, int32_t py)
{
    if (w->flags & WM_WF_NODECORATION) return 0;
    int32_t x0 = (int32_t)w->x;
    int32_t y0 = (int32_t)w->y;
    int32_t x1 = x0 + (int32_t)w->w;
    int32_t y1 = y0 + (int32_t)WSD_TITLE_H;
    return px >= x0 && px < x1 && py >= y0 && py < y1;
}

static int point_in_close_button(const struct wm_window *w,
                                 int32_t px, int32_t py)
{
    if (w->flags & WM_WF_NODECORATION) return 0;
    if (w->w < WSD_CLOSE_BTN_W + 2u * WSD_BTN_INSET) return 0;
    int32_t cb_x = (int32_t)w->x + (int32_t)w->w
                 - (int32_t)WSD_CLOSE_BTN_W - (int32_t)WSD_BTN_INSET;
    int32_t cb_y = (int32_t)w->y + (int32_t)WSD_BTN_INSET;
    int32_t cb_w = (int32_t)WSD_CLOSE_BTN_W;
    int32_t cb_h = (int32_t)WSD_TITLE_H - 2 * (int32_t)WSD_BTN_INSET;
    return px >= cb_x && px < cb_x + cb_w
        && py >= cb_y && py < cb_y + cb_h;
}

/* chapter 118 -- minimize button sits immediately to the
 * left of the close button (separated by WSD_BTN_GAP).
 * Geometry MUST match paint_decoration above; both functions
 * use the same anchor (right edge of bar - inset) so the hit
 * rect always lines up with the painted rect.  Returns 0 for
 * undecorated windows or windows narrower than min+close+
 * (3*inset+gap). */
static int point_in_minimize_button(const struct wm_window *w,
                                    int32_t px, int32_t py)
{
    if (w->flags & WM_WF_NODECORATION) return 0;
    uint32_t both_w = WSD_MIN_BTN_W + WSD_BTN_GAP
                    + WSD_CLOSE_BTN_W + 2u * WSD_BTN_INSET;
    if (w->w < both_w) return 0;
    int32_t cb_x = (int32_t)w->x + (int32_t)w->w
                 - (int32_t)WSD_CLOSE_BTN_W - (int32_t)WSD_BTN_INSET;
    int32_t mb_x = cb_x - (int32_t)WSD_BTN_GAP - (int32_t)WSD_MIN_BTN_W;
    int32_t mb_y = (int32_t)w->y + (int32_t)WSD_BTN_INSET;
    int32_t mb_h = (int32_t)WSD_TITLE_H - 2 * (int32_t)WSD_BTN_INSET;
    return px >= mb_x && px < mb_x + (int32_t)WSD_MIN_BTN_W
        && py >= mb_y && py < mb_y + mb_h;
}

/* chapter 118 -- resize grip hit-test.  Grip is the
 * bottom-right WSD_GRIP_SIZE x WSD_GRIP_SIZE square of the
 * window's body (NOT the title bar).  Only present on
 * RESIZABLE-flagged windows; everything else returns 0 so
 * the grip rect falls through to the normal body hit-test.
 * Geometry MUST match the grip-paint block in
 * paint_decoration. */
static int point_in_resize_grip(const struct wm_window *w,
                                int32_t px, int32_t py)
{
    if (!(w->flags & GUI_WIN_FLAG_RESIZABLE)) return 0;
    int32_t gw = (int32_t)WSD_GRIP_SIZE;
    int32_t grip_x0 = (int32_t)w->x + (int32_t)w->w - gw;
    int32_t grip_y0 = (int32_t)w->y + (int32_t)deco_top_h(w->flags)
                    + (int32_t)w->h - gw;
    return px >= grip_x0 && px < grip_x0 + gw
        && py >= grip_y0 && py < grip_y0 + gw;
}

static int point_in_body(const struct wm_window *w,
                         int32_t px, int32_t py)
{
    int32_t bx0 = (int32_t)w->x;
    int32_t by0 = (int32_t)w->y + (int32_t)deco_top_h(w->flags);
    int32_t bx1 = bx0 + (int32_t)w->w;
    int32_t by1 = by0 + (int32_t)w->h;
    return px >= bx0 && px < bx1 && py >= by0 && py < by1;
}

/* Walk wsd's z-order top-to-bottom, return the first slot
 * whose body/titlebar covers (px, py).  -1 = wallpaper.
 * chapter 118 -- hidden (minimized) windows are skipped:
 * the user can't click into them, and the cursor sees them
 * as if they weren't there. */
static int hit_test_topmost(int32_t px, int32_t py)
{
    for (int i = (int)g_z_count - 1; i >= 0; i--) {
        int s = g_z_order[i];
        if (!g_windows[s].in_use) continue;
        if (g_windows[s].hidden)  continue;
        const struct wm_window *w = &g_windows[s];
        if (point_in_titlebar(w, px, py)) return s;
        if (point_in_body(w, px, py))     return s;
    }
    return -1;
}

/* -1 means "nobody being dragged".  Drag offset is the click
 * point's offset within the title bar so the bar tracks the
 * cursor exactly under the original click point as the user
 * drags. */
static int     g_drag_slot     = -1;
static int32_t g_drag_off_x    = 0;
static int32_t g_drag_off_y    = 0;
static uint32_t g_prev_btn     = 0;

/* chapter 118 -- resize state.  -1 means "nobody being
 * resized".  The anchor is the cursor position at drag start;
 * orig_w/orig_h are the window's logical dims at drag start.
 * On each tick the new (w, h) = orig + (cursor - anchor),
 * clamped to [RESIZE_MIN_*, fb_*] so we stay within the FB
 * allocation. */
static int     g_resize_slot   = -1;
static int32_t g_resize_anchor_x = 0;
static int32_t g_resize_anchor_y = 0;
static uint32_t g_resize_orig_w  = 0;
static uint32_t g_resize_orig_h  = 0;

/* chapter 118 -- the slot whose body is the current "press
 * target" between left-DOWN and the matching left-UP.  Used to
 * route the UP to the same window as the DOWN, even if the
 * user dragged the cursor off that window between press and
 * release.  -1 if no left button is held down on a body. */
static int     g_press_slot    = -1;

/* chapter 118 -- the slot the cursor was OVER on the last
 * tick (or -1 if cursor was over wallpaper / a title bar).
 * Tracked separately from g_press_slot so we can deliver a
 * "leave" MOUSE_MOVE to the previous body when the cursor
 * exits it -- otherwise hover-on-mouse-move apps (launcher
 * buttons, browser toolbar buttons) get stuck in the hover
 * state because they never see a MOVE outside their body.
 *
 * The leave event is just a synthetic MOUSE_MOVE delivered
 * with the cursor's CURRENT scanout position translated into
 * the OLD window's local coords -- typically negative or past
 * w->w / w->h, which any sane hit_test treats as "no widget".
 * Apps that DO care about precise enter/leave can compare
 * local coords against window bounds; the existing browser
 * and launcher just call hit_test(lx, ly) and get -1. */
static int     g_hover_slot    = -1;

/* Inject a body-relative pointer event into a wsd-managed
 * window's kernel shadow.  Caller computes wnd-local coords;
 * we just shape the gui_event and call gui_deliver_event.
 * No-op if the window has no kernel shadow bound (typical for
 * output-only popups). */
static void inject_pointer(const struct wm_window *w, uint32_t type,
                           int32_t lx, int32_t ly,
                           uint32_t button, uint32_t btn_mask)
{
    if (!w || w->kernel_id < 0) return;
    struct gui_event ev;
    ev.type      = type;
    ev.window_id = w->kernel_id;
    ev.arg0      = (uint32_t)lx;
    ev.arg1      = (uint32_t)ly;
    ev.arg2      = (type == GUI_EVENT_MOUSE_MOVE) ? btn_mask : button;
    ev.arg3      = btn_mask;
    (void)gui_deliver_event(w->kernel_id, &ev);
}

/* chapter 118 -- apply a target (new_w, new_h) to window w
 * with clamping.  Calls the kernel's SYS_WIN_FB_RESIZE to
 * reallocate the backing pages, then re-maps to discover
 * wsd's new owner-VA, then delivers a coalesced
 * GUI_EVENT_RESIZE to the client so it can re-call
 * win_fb_map on its own AS (its old VA is now translation-
 * faulting -- it must remap before its next paint).
 *
 * Returns 1 if anything actually changed, 0 if not (or on
 * kernel failure, treating both as "no recompose needed").
 * Caller must hold g_wsd_lock and is responsible for the
 * subsequent recompose.
 *
 * Clamping policy:
 *   - lower bound: WSD_RESIZE_MIN_W/H (title-bar buttons fit);
 *   - upper bound: scanout dims (keeps the FB sane and
 *     bounds the worst-case kernel page allocation). */
static int resize_apply(struct wm_window *w,
                        uint32_t new_w, uint32_t new_h)
{
    if (!(w->flags & GUI_WIN_FLAG_RESIZABLE)) return 0;
    if (w->fb_id == 0) return 0;     /* no backing FB */

    if (new_w < WSD_RESIZE_MIN_W) new_w = WSD_RESIZE_MIN_W;
    if (new_h < WSD_RESIZE_MIN_H) new_h = WSD_RESIZE_MIN_H;
    if (g_scanout_w != 0 && new_w > g_scanout_w) new_w = g_scanout_w;
    if (g_scanout_h != 0 && new_h > g_scanout_h) new_h = g_scanout_h;
    if (new_w == w->w && new_h == w->h) return 0;

    /* Remember the old dims so we can fill the GROWN region
     * (everything outside the old top-left rect, inside the
     * new) with a neutral colour after the kernel resize.
     * The kernel zero-fills new pages, so without this fix
     * wsd's immediate post-resize recompose would paint the
     * grown region black until the client app sees the
     * GUI_EVENT_RESIZE event and repaints itself.  For apps
     * that repaint asynchronously (browser's parser thread)
     * this black flash can persist for hundreds of ms or, if
     * the user keeps dragging the grip, indefinitely. */
    uint32_t old_w = w->w;
    uint32_t old_h = w->h;

    /* Kernel reallocates backing.  On success the old owner
     * VA is GONE (kernel uninstalled it).  We MUST re-map
     * before touching w->fb_va again. */
    int kr = win_fb_resize(w->fb_id, new_w, new_h);
    if (kr != 0) {
        printf("[wsd] win_fb_resize id=%u %ux%u failed err=%d\n",
               (unsigned)w->fb_id,
               (unsigned)new_w, (unsigned)new_h, kr);
        /* Kernel guarantees the FB is unchanged on failure;
         * leave w->fb_va alone too. */
        return 0;
    }

    /* Discover wsd's new owner-VA.  win_fb_map sees we're
     * the owner and returns fb->owner_va (which the kernel
     * just re-installed inside SYS_WIN_FB_RESIZE).  If THIS
     * fails, the kernel's owner reinstall must have failed;
     * we zero fb_va so compose skips this window until the
     * next resize succeeds.  Don't kill the window -- the
     * pixel pages still exist and other tools could in
     * theory recover. */
    struct win_fb_map_args ma;
    int mr = win_fb_map(w->fb_id, &ma);
    if (mr != 0) {
        printf("[wsd] win_fb_map after resize id=%u failed err=%d\n",
               (unsigned)w->fb_id, mr);
        w->fb_va = 0;
        w->fb_w  = new_w;
        w->fb_h  = new_h;
        w->w     = new_w;
        w->h     = new_h;
        /* Still deliver RESIZE so the client knows. */
        if (w->kernel_id >= 0) {
            struct gui_event ev = {0};
            ev.type      = GUI_EVENT_RESIZE;
            ev.window_id = w->kernel_id;
            ev.arg0      = new_w;
            ev.arg1      = new_h;
            (void)gui_deliver_event(w->kernel_id, &ev);
        }
        return 1;
    }

    w->fb_va     = ma.va;
    w->fb_stride = ma.stride;
    w->fb_size   = ma.size;
    w->fb_w      = ma.w;
    w->fb_h      = ma.h;
    w->w         = new_w;
    w->h         = new_h;

    /* Fill the GROWN region of the new FB with a neutral
     * placeholder colour.  Two rectangles cover everything
     * outside (0, 0, old_w, old_h) but inside (0, 0, new_w,
     * new_h):
     *
     *   A) right strip:  x in [old_w, new_w),  y in [0, new_h)
     *      -- only exists if new_w > old_w
     *   B) bottom strip: x in [0, old_w),      y in [old_h, new_h)
     *      -- only exists if new_h > old_h
     *
     * Shrink-only resize (new <= old in both dims) is a no-op
     * here; the kernel just copied the surviving top-left
     * rect and we're done.  Mixed grow-one-shrink-other
     * works fine: only the grown axis runs its strip.
     *
     * Colour choice: WSD_DECO_BG_IDLE (the unfocused-title
     * gray-blue) looks intentional and matches the rest of
     * the decoration palette, so the user perceives "the
     * window is growing" rather than "the window is
     * broken".  Once the client repaints, this placeholder
     * is overwritten. */
    if ((new_w > old_w || new_h > old_h) && w->fb_va != 0) {
        uint8_t *fb = (uint8_t *)(uintptr_t)w->fb_va;
        uint32_t s  = w->fb_stride;
        uint32_t col = WSD_DECO_BG_IDLE;

        /* Right strip. */
        if (new_w > old_w) {
            for (uint32_t yy = 0; yy < new_h; yy++) {
                uint32_t *row = (uint32_t *)(fb
                    + (size_t)yy * s + (size_t)old_w * 4u);
                for (uint32_t xx = old_w; xx < new_w; xx++)
                    *row++ = col;
            }
        }
        /* Bottom strip (excludes the right strip already filled). */
        if (new_h > old_h) {
            uint32_t bw = old_w < new_w ? old_w : new_w;
            for (uint32_t yy = old_h; yy < new_h; yy++) {
                uint32_t *row = (uint32_t *)(fb
                    + (size_t)yy * s);
                for (uint32_t xx = 0; xx < bw; xx++)
                    *row++ = col;
            }
        }
    }

    /* Tell the client app the logical viewport changed AND
     * its old win_fb mapping is dead.  The client's
     * GUI_EVENT_RESIZE handler must call wm_window_remap_fb
     * (libgui helper) before its next paint or it will
     * translation-fault on the old VA. */
    if (w->kernel_id >= 0) {
        struct gui_event ev = {0};
        ev.type      = GUI_EVENT_RESIZE;
        ev.window_id = w->kernel_id;
        ev.arg0      = new_w;
        ev.arg1      = new_h;
        (void)gui_deliver_event(w->kernel_id, &ev);
    }
    return 1;
}

static void poller_tick(void)
{
    int32_t  x = 0, y = 0;
    uint32_t btn = 0;
    long r = pointer_state(&x, &y, &btn);
    if (r != 0) return;

    int moved   = (x != g_cursor_x) || (y != g_cursor_y);
    int rising  = ((btn & GUI_BTN_LEFT) && !(g_prev_btn & GUI_BTN_LEFT));
    int falling = (!(btn & GUI_BTN_LEFT) && (g_prev_btn & GUI_BTN_LEFT));
    /* chapter 118 follow-up -- right-button edges.  Right-click
     * is used by apps for context-menu / colour-cycle gestures
     * (paint's palette cycle, future text-area context menu,
     * etc).  We never raise/focus/drag on right-button, just
     * forward the DOWN/UP into the body-hit window so the app
     * can react.  Without this, right-clicks were dropped on the
     * floor because wsd's poller_tick was LEFT-only. */
    int rrising  = ((btn & GUI_BTN_RIGHT) && !(g_prev_btn & GUI_BTN_RIGHT));
    int rfalling = (!(btn & GUI_BTN_RIGHT) && (g_prev_btn & GUI_BTN_RIGHT));

    if (!moved && !rising && !falling && !rrising && !rfalling) {
        g_prev_btn = btn;
        return;
    }

    g_cursor_btn = btn;

    mutex_lock(&g_wsd_lock);

    /* Falling edge: end any drag. */
    if (falling) {
        if (g_drag_slot >= 0) {
            printf("[wsd] drag end win-slot=%d\n", g_drag_slot);
        }
        g_drag_slot = -1;
        /* chapter 118 -- also end any active resize.  We've
         * been live-applying new dims each tick, so there's
         * nothing to commit on release; just clear the slot
         * so the next click starts fresh. */
        if (g_resize_slot >= 0) {
            printf("[wsd] resize end win-slot=%d final=%ux%u\n",
                   g_resize_slot,
                   (unsigned)g_windows[g_resize_slot].w,
                   (unsigned)g_windows[g_resize_slot].h);
        }
        g_resize_slot = -1;
    }

    /* Drag in progress: reposition the dragged window.  Done
     * before rising-edge handling so a one-tick drag (press +
     * release in the same tick — unrealistic but defensible)
     * doesn't crash on stale slot indices.  This is the ONE
     * case where a cursor-move triggers a full recompose;
     * the window contents have to redraw under the cursor. */
    int dragged_this_tick = 0;
    if (g_drag_slot >= 0 && (btn & GUI_BTN_LEFT)) {
        struct wm_window *w = &g_windows[g_drag_slot];
        if (w->in_use && !(w->flags & WM_WF_NODECORATION)) {
            int32_t new_x = x - g_drag_off_x;
            int32_t new_y = y - g_drag_off_y;
            if (new_x < 0) new_x = 0;
            if (new_y < 0) new_y = 0;
            if ((uint32_t)new_x >= g_scanout_w) new_x = (int32_t)g_scanout_w - 1;
            if ((uint32_t)new_y >= g_scanout_h) new_y = (int32_t)g_scanout_h - 1;
            if ((uint32_t)new_x != w->x || (uint32_t)new_y != w->y) {
                w->x = (uint32_t)new_x;
                w->y = (uint32_t)new_y;
                if (w->kernel_id >= 0) {
                    (void)gui_move_window(w->kernel_id,
                                          new_x,
                                          new_y + (int32_t)WSD_TITLE_H);
                }
                dragged_this_tick = 1;
            }
        } else {
            g_drag_slot = -1;
        }
    }

    /* chapter 118 -- resize in progress.  Mirror of the
     * drag block above, but for the grip.  new_w/new_h are
     * (orig + cursor_delta), clamped inside resize_apply.
     * Triggers a full recompose so the body and grip stay
     * under the cursor.  We don't move the window's (x, y);
     * the grip is in the bottom-right corner so dragging
     * out grows the body in place. */
    int resized_this_tick = 0;
    if (g_resize_slot >= 0 && (btn & GUI_BTN_LEFT)) {
        struct wm_window *w = &g_windows[g_resize_slot];
        if (w->in_use && (w->flags & GUI_WIN_FLAG_RESIZABLE)) {
            int32_t dx = x - g_resize_anchor_x;
            int32_t dy = y - g_resize_anchor_y;
            int32_t nw = (int32_t)g_resize_orig_w + dx;
            int32_t nh = (int32_t)g_resize_orig_h + dy;
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
            if (resize_apply(w, (uint32_t)nw, (uint32_t)nh)) {
                if (w->kernel_id >= 0) {
                    /* Keep the kernel-WM input shadow rect
                     * sized to match -- otherwise the
                     * shadow's hit-test ignores clicks in
                     * the newly-exposed strip.  We use the
                     * existing move syscall to push position
                     * (unchanged) and trust the kernel WM's
                     * own resize path is currently a no-op
                     * for passthrough shadows. */
                    (void)gui_move_window(w->kernel_id,
                                          (int32_t)w->x,
                                          (int32_t)w->y
                                              + (int32_t)WSD_TITLE_H);
                }
                resized_this_tick = 1;
            }
        } else {
            g_resize_slot = -1;
        }
    }

    /* Rising edge: hit-test wsd's z-order (top-first) and act:
     *   close button   -> inject GUI_EVENT_CLOSE
     *   title bar      -> raise + start drag
     *   body           -> raise + inject MOUSE_DOWN
     *   wallpaper      -> do nothing (focus stays put)
     * chapter 118: also calls gui_raise_window so the kernel
     * focuses the same window for keyboard input. */
    if (rising && g_drag_slot < 0) {
        int hit = hit_test_topmost(x, y);
        if (hit >= 0) {
            struct wm_window *w = &g_windows[hit];
            if (point_in_close_button(w, x, y)) {
                if (w->kernel_id >= 0) {
                    struct gui_event ev = {0};
                    ev.type      = GUI_EVENT_CLOSE;
                    ev.window_id = w->kernel_id;
                    (void)gui_deliver_event(w->kernel_id, &ev);
                    printf("[wsd] decoration close win=%u kernel_id=%d\n",
                           (unsigned)w->id, (int)w->kernel_id);
                }
            } else if (point_in_minimize_button(w, x, y)) {
                /* chapter 118 -- minimize click.  Mark the
                 * wsd-side hidden bit and tell the kernel WM
                 * to drop keyboard focus from this window.
                 * Then full-recompose so the now-hidden
                 * window's pixels are replaced by whatever
                 * was behind it (other windows + wallpaper).
                 * The taskbar's WM_LIST poll will see the
                 * GUI_WIN_FLAG_MINIMIZED bit on the next
                 * tick and render the cell in its minimized
                 * style; a click on that cell sends
                 * WM_WIN_RESTORE back to us. */
                w->hidden = 1;
                if (w->kernel_id >= 0) {
                    (void)gui_set_minimized(w->kernel_id, 1);
                }
                printf("[wsd] decoration minimize win=%u kernel_id=%d\n",
                       (unsigned)w->id, (int)w->kernel_id);
                wsd_compose_all();
            } else if (point_in_titlebar(w, x, y)) {
                g_drag_slot  = hit;
                g_drag_off_x = x - (int32_t)w->x;
                g_drag_off_y = y - (int32_t)w->y;
                if (z_raise(hit) && w->kernel_id >= 0) {
                    (void)gui_raise_window(w->kernel_id);
                }
                printf("[wsd] drag start win=%u slot=%d off=%d,%d\n",
                       (unsigned)w->id, hit,
                       (int)g_drag_off_x, (int)g_drag_off_y);
            } else if (point_in_resize_grip(w, x, y)) {
                /* chapter 118 -- grip-press.  Grip is in the
                 * window's BODY rect, so this check has to run
                 * before point_in_body below; otherwise the
                 * body branch would steal the click and inject
                 * a MOUSE_DOWN into the app.  Raise the
                 * window (matches drag-start behaviour) and
                 * stash the anchor + original dims; the
                 * resize-in-progress block above does the
                 * per-tick application. */
                if (z_raise(hit) && w->kernel_id >= 0) {
                    (void)gui_raise_window(w->kernel_id);
                }
                g_resize_slot     = hit;
                g_resize_anchor_x = x;
                g_resize_anchor_y = y;
                g_resize_orig_w   = w->w;
                g_resize_orig_h   = w->h;
                printf("[wsd] resize start win=%u slot=%d "
                       "orig=%ux%u anchor=%d,%d\n",
                       (unsigned)w->id, hit,
                       (unsigned)w->w, (unsigned)w->h,
                       (int)x, (int)y);
            } else if (point_in_body(w, x, y)) {
                /* chapter 118 follow-up -- PIN_BOTTOM windows
                 * (the wallpaper) are click-transparent for
                 * focus/raise purposes.  Without this skip,
                 * clicking through to the wallpaper would call
                 * gui_raise_window(wallpaper_kernel_id), which
                 * sets the kernel's g_focus_id to the wallpaper
                 * (silently stealing keyboard focus from
                 * whatever app the user was using -- e.g. ESC
                 * stops exiting paint because it now reaches
                 * the desktop process instead).  Wallpaper
                 * clicks are not interesting to the desktop
                 * app either, so we skip the MOUSE_DOWN
                 * injection too.  z_raise was already a no-op
                 * for PIN_BOTTOM (bug #1 fix above), so the
                 * window also doesn't move in wsd's z-order. */
                if (w->flags & WM_WF_PIN_BOTTOM) {
                    /* deliberately empty */
                } else {
                    int z_changed = z_raise(hit);
                    if (z_changed && w->kernel_id >= 0) {
                        (void)gui_raise_window(w->kernel_id);
                    } else if (w->kernel_id >= 0) {
                        /* Already top in wsd z -- still tell the
                         * kernel to focus this shadow for keyboard. */
                        (void)gui_raise_window(w->kernel_id);
                    }
                    g_press_slot = hit;
                    int32_t lx = x - (int32_t)w->x;
                    int32_t ly = y - ((int32_t)w->y
                                      + (int32_t)deco_top_h(w->flags));
                    inject_pointer(w, GUI_EVENT_MOUSE_DOWN,
                                   lx, ly, GUI_BTN_LEFT, btn);
                }
            }
        }
    }

    /* Cursor moved: route MOUSE_MOVE to the topmost window
     * under the cursor (or to the press-target if a button
     * is held -- gives "drag past edge keeps the press").
     * Skipped during a title-bar drag so the dragged window
     * doesn't see phantom moves.
     *
     * chapter 118 -- ALSO synthesize a "leave" MOUSE_MOVE
     * to the previously-hovered window when the cursor exits
     * its body.  Without this, hover-on-mouse-move apps
     * (launcher buttons, browser toolbar buttons) stay stuck
     * in their last hover state because they never see a
     * MOVE outside their body.  The leave event uses the
     * cursor's CURRENT position translated into the OLD
     * window's local coords -- typically negative or beyond
     * (w->w, w->h), which any hit_test treats as "no widget"
     * and clears the hover state via the existing change-
     * detect (new_hover != g_hover) branch the app already
     * has.
     *
     * chapter 118 follow-up #3 -- also skip during a resize
     * drag.  Without this, every poller tick injects a phantom
     * MOUSE_MOVE on top of the GUI_EVENT_RESIZE, leaving an
     * interleaved [RESIZE, MOVE, RESIZE, MOVE, ...] ring that
     * the kernel's tail-only ring_coalesce_resize can't merge.
     * Coalescing isn't critical for correctness (the kernel's
     * lazy-unmap of stale FB pages keeps the browser from
     * faulting on intermediate VAs -- see win_fb.c), but it
     * eliminates a spurious flood of hover/leave routing on
     * the window being resized. */
    if (moved && g_drag_slot < 0 && g_resize_slot < 0) {
        int target = (g_press_slot >= 0
                      && g_windows[g_press_slot].in_use)
                     ? g_press_slot
                     : hit_test_topmost(x, y);
        int new_hover = -1;
        if (target >= 0 && point_in_body(&g_windows[target], x, y)) {
            new_hover = target;
        }

        /* Deliver leave to the previous hover target if it
         * changed.  Skip when the new target is the same
         * (move within the same body) or when the previous
         * slot is dead (gc_conn_windows already cleared it
         * out -- not our problem). */
        if (g_hover_slot >= 0 && g_hover_slot != new_hover
            && g_windows[g_hover_slot].in_use) {
            const struct wm_window *prev = &g_windows[g_hover_slot];
            int32_t lx = x - (int32_t)prev->x;
            int32_t ly = y - ((int32_t)prev->y
                              + (int32_t)deco_top_h(prev->flags));
            inject_pointer(prev, GUI_EVENT_MOUSE_MOVE,
                           lx, ly, 0, btn);
        }

        if (new_hover >= 0) {
            const struct wm_window *w = &g_windows[new_hover];
            int32_t lx = x - (int32_t)w->x;
            int32_t ly = y - ((int32_t)w->y
                              + (int32_t)deco_top_h(w->flags));
            inject_pointer(w, GUI_EVENT_MOUSE_MOVE,
                           lx, ly, 0, btn);
        }

        g_hover_slot = new_hover;
    }

    /* Falling edge: deliver MOUSE_UP to the press target (if
     * any), regardless of where the cursor is now.  Then
     * clear the press target. */
    if (falling && g_press_slot >= 0
        && g_windows[g_press_slot].in_use) {
        const struct wm_window *w = &g_windows[g_press_slot];
        int32_t lx = x - (int32_t)w->x;
        int32_t ly = y - ((int32_t)w->y
                          + (int32_t)deco_top_h(w->flags));
        inject_pointer(w, GUI_EVENT_MOUSE_UP, lx, ly, GUI_BTN_LEFT, btn);
    }
    if (falling) g_press_slot = -1;

    /* chapter 118 follow-up -- right-button DOWN/UP.  Routed
     * to the topmost decorated window under the cursor (which
     * is wsd's notion of "focused", same as the focused_slot
     * compose_all picks).  Does NOT raise, does NOT focus, does
     * NOT touch g_press_slot (that's left-only -- a right-drag
     * past a window's edge should not magnetically follow the
     * previous left-press target).  Skips PIN_BOTTOM windows
     * because the wallpaper has no use for right-clicks and
     * forwarding them would noisily wake the desktop process.
     *
     * The UP is paired by hit-testing again at release time
     * (matches what the kernel WM does for non-LEFT releases)
     * rather than remembering the press target -- this avoids
     * paying for a g_rpress_slot field for a feature that
     * doesn't need drag semantics today. */
    if (rrising) {
        int hit = hit_test_topmost(x, y);
        if (hit >= 0) {
            struct wm_window *w = &g_windows[hit];
            if (w->kernel_id >= 0
                && !(w->flags & WM_WF_PIN_BOTTOM)
                && point_in_body(w, x, y)) {
                int32_t lx = x - (int32_t)w->x;
                int32_t ly = y - ((int32_t)w->y
                                  + (int32_t)deco_top_h(w->flags));
                inject_pointer(w, GUI_EVENT_MOUSE_DOWN,
                               lx, ly, GUI_BTN_RIGHT, btn);
            }
        }
    }
    if (rfalling) {
        int hit = hit_test_topmost(x, y);
        if (hit >= 0) {
            struct wm_window *w = &g_windows[hit];
            if (w->kernel_id >= 0
                && !(w->flags & WM_WF_PIN_BOTTOM)
                && point_in_body(w, x, y)) {
                int32_t lx = x - (int32_t)w->x;
                int32_t ly = y - ((int32_t)w->y
                                  + (int32_t)deco_top_h(w->flags));
                inject_pointer(w, GUI_EVENT_MOUSE_UP,
                               lx, ly, GUI_BTN_RIGHT, btn);
            }
        }
    }

    /* Frame composition.  Order of preference (cheapest first):
     *   - drag this tick: full compose (window moved)
     *   - resize this tick: full compose (window changed size)
     *   - rising/falling: full compose (z order or focus
     *     changed, or windows may have appeared/disappeared
     *     via close)
     *   - pure cursor move: cursor_move_only -- two tiny
     *     fb_present rects, no wallpaper repaint */
    if (dragged_this_tick || resized_this_tick || rising || falling) {
        g_cursor_x = x;
        g_cursor_y = y;
        wsd_compose_all();
    } else if (moved) {
        cursor_move_only(x, y);
    }

    g_prev_btn = btn;
    mutex_unlock(&g_wsd_lock);
}

static void input_poller_thread(void *arg)
{
    (void)arg;
    printf("[wsd] input poller alive\n");
    for (;;) {
        poller_tick();
        /* chapter 118 -- the kernel scheduler tick is 100 ms
         * (TICK_INTERVAL_MS), so sleep_ms() with anything less
         * than 100 actually sleeps a full quantum -- the
         * resulting 10 Hz cursor polling is what made the
         * pointer feel jerky.  Use yield() instead: on SMP
         * with most threads idle, yield() returns as soon as
         * the runq has nothing else, giving us effectively
         * continuous polling on one CPU while the other CPU
         * stays free for accept / damage / app work.  Cost
         * is ~1 CPU of "wasted" cycles while idle; the win
         * is sub-millisecond cursor latency. */
        yield();
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("[wsd] starting (chapter 117)\n");

    struct fb_map_args fb = {0};
    wait_for_fb_then_map(&fb);

    if (fb.va == 0) {
        printf("[wsd] no FB; idling\n");
    } else {
        /* Cache the scanout descriptors for handle_damage. */
        g_scanout_va     = fb.va;
        g_scanout_w      = fb.w;
        g_scanout_h      = fb.h;
        g_scanout_stride = fb.stride;

        /* The single canonical banner that test_wsd_smoke.py
         * greps for.  Format is stable; tests pin to the
         * "mapped FB" prefix only, but include the geometry
         * so a human looking at boot output can sanity-check
         * the values match the virtio-gpu negotiated mode. */
        printf("[wsd] mapped FB va=0x%lx w=%u h=%u stride=%u size=%u\n",
               (unsigned long)fb.va,
               (unsigned)fb.w,
               (unsigned)fb.h,
               (unsigned)fb.stride,
               (unsigned)fb.size);
    }

    /* Bind /srv/wm *before* the wallpaper paint.  init spawns
     * /bin/desktop /bin/taskbar /bin/launcher back-to-back
     * right after /bin/wsd; their first wmclient call hits
     * srv_connect(/srv/wm) within a handful of ticks.  The
     * 1920x1080x4 wallpaper compose below is ~8 MiB of byte-
     * by-byte copy and takes long enough on -display cocoa
     * that any GUI app racing it gets -ENOENT and exits.
     * Binding first means connects queue against the listener
     * and the post-compose srv_accept loop drains them in
     * order.  If this fails we exit non-zero and let init's
     * supervisor respawn us. */
    int lfd = srv_bind(WM_SOCK_PATH);
    if (lfd < 0) {
        printf("[wsd] srv_bind(%s) failed: %d\n", WM_SOCK_PATH, lfd);
        return 1;
    }
    /* The canonical "wsd's bus is up" line.  test_wsd_hello
     * waits for this before running wmtest so it doesn't
     * race the bind. */
    printf("[wsd] ready on %s (lfd=%d)\n", WM_SOCK_PATH, lfd);

    if (fb.va != 0) {
        /* Paint the wallpaper and flush to GPU
         * so the screen isn't whatever garbage the BIOS or
         * the previous kernel-WM-driven compose left in
         * scanout RAM.  Until chapter 117 the kernel WM did
         * this implicitly on its first event; now wsd owns
         * it.  No windows yet so compose_all is just
         * wallpaper + fb_present. */
        wsd_compose_all();
    }

    /* chapter 118 -- spawn the input poller now (after FB is
     * mapped, before accept).  The poller needs the scanout
     * cached so its first compose_all draws the cursor.
     * Spawning *before* accept means the poller is alive
     * even if no clients ever connect (the cursor still
     * follows the mouse over the bare wallpaper, which is
     * the chapter-109 acceptance test).
     *
     * Skip if the FB never came up — a wsd with no scanout
     * has nothing to paint a cursor onto, and the poller's
     * wsd_compose_all path early-returns on g_scanout_va==0
     * anyway, so the loop would just burn CPU. */
    if (g_scanout_va != 0) {
        int ptid = thread_spawn_files(input_poller_thread, NULL, -1);
        if (ptid < 0) {
            printf("[wsd] input poller spawn failed: %d\n", ptid);
        } else {
            printf("[wsd] input poller tid=%d\n", ptid);
        }
    }

    /* Chapter 117 — one worker thread per accepted connection.
     * Threads share our fd table via thread_spawn_files so
     * the cfd we just accepted is usable from the worker.
     * Each worker calls exit(0) when its conn EOFs; the
     * stack mmap'd by thread_spawn_files leaks until process
     * exit (chapter-91 caveat -- fine, conns are scarce). */
    for (;;) {
        int cfd = srv_accept(lfd);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            printf("[wsd] srv_accept failed: %s\n", strerror(errno));
            close(lfd);
            return 1;
        }
        int tid = thread_spawn_files(conn_thread,
                                     (void *)(intptr_t)cfd,
                                     -1 /* inherit cpu */);
        if (tid < 0) {
            printf("[wsd] thread_spawn_files failed: %d\n", tid);
            close(cfd);
            continue;
        }
    }
    /* unreachable */
    return 0;
}

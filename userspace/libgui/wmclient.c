/*
 * userspace/libgui/wmclient.c — chapter 108d
 * implementation of the /srv/wm client.
 *
 * State model
 * -----------
 *
 * One persistent fd to /srv/wm per process (g_conn), opened
 * lazily on the first wm_connect.  All windows in this
 * process share it.  wsd's session_id is held in g_session
 * for completeness; nothing in this module reads it back
 * (wsd does the ownership check internally), but it's
 * useful in debug prints.
 *
 * Wire I/O discipline
 * -------------------
 *
 * Chapter 107's read/write semantics are atomic-datagram:
 * one write delivers one whole message, one read consumes
 * exactly one whole message.  Short writes / short reads
 * mean "protocol error, conn is now poisoned" -- we mark
 * g_conn = -1 and the next wm_connect re-opens cleanly.
 *
 * Freestanding C reminders
 * ------------------------
 *
 * No libc.  No memset / memcpy in the hot path (printf has
 * its own).  All struct inits use field-by-field assignment
 * (see /memories/freestanding-c-memset-trap.md).
 */

#include "wmclient.h"
#include "../libc/printf.h"

static int      g_conn    = -1;
static uint32_t g_session = 0;

/* Transient IPC errors we should retry, not treat as a poisoned
 * connection.  On this kernel read/write can return -EINTR/-EAGAIN
 * under scheduler/signal races; tearing down g_conn on those causes
 * avoidable remap failures during resize storms. */
#define WM_IO_EINTR   (-4)
#define WM_IO_EAGAIN  (-11)

/* Send one wm_msg over g_conn.  Returns 0 on success, -1
 * on any short / failed write (and poisons g_conn so the
 * next operation forces a reconnect). */
static int wm_send(const struct wm_msg *m)
{
    for (;;) {
        long n = write(g_conn, m, sizeof(*m));
        if (n == (long)sizeof(*m)) return 0;
        if (n == WM_IO_EINTR || n == WM_IO_EAGAIN) {
            /* Retry transient interruption without poisoning the
             * session; caller still sees a single logical send. */
            yield();
            continue;
        }
        printf("[wmclient] write err n=%ld op=%u\n",
               n, (unsigned)m->op);
        g_conn = -1;
        return -1;
    }
}

/* Receive one wm_msg over g_conn.  Returns 0 on success,
 * -1 on any failure (with g_conn poisoned). */
static int wm_recv(struct wm_msg *m)
{
    for (;;) {
        long n = read(g_conn, m, sizeof(*m));
        if (n == (long)sizeof(*m)) return 0;
        if (n == WM_IO_EINTR || n == WM_IO_EAGAIN) {
            /* Retry transient interruption without dropping g_conn. */
            yield();
            continue;
        }
        printf("[wmclient] read err n=%ld\n", n);
        g_conn = -1;
        return -1;
    }
}

int wm_connect(void)
{
    if (g_conn >= 0) return 0;

    int fd = srv_connect(WM_SOCK_PATH);
    if (fd < 0) {
        printf("[wmclient] srv_connect(%s) failed err=%d\n",
               WM_SOCK_PATH, fd);
        return -1;
    }
    g_conn = fd;

    struct wm_msg req;
    req.op     = WM_HELLO;
    req.status = 0;
    req.a      = WM_PROTO_VERSION;
    req.b      = 0;
    req.c      = 0;
    req.d      = 0;
    if (wm_send(&req) < 0) return -1;

    struct wm_msg rep;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != WM_HELLO || rep.status != WM_OK) {
        printf("[wmclient] HELLO failed op=%u status=%d\n",
               (unsigned)rep.op, (int)rep.status);
        g_conn = -1;
        return -1;
    }
    g_session = rep.a;
    printf("[wmclient] connected session=%u wsd_version=%u\n",
           (unsigned)g_session, (unsigned)rep.b);
    return 0;
}

void wm_disconnect(void)
{
    if (g_conn >= 0) {
        close(g_conn);
        g_conn    = -1;
        g_session = 0;
    }
}

int wm_create_window(uint32_t w, uint32_t h, uint32_t flags,
                     struct wm_window *out)
{
    return wm_create_window_input(w, h, flags, NULL, out);
}

/* Internal worker for both wm_create_window_input and
 * wm_create_window_at.  `create_op` is the wsd op to send
 * (WM_WIN_CREATE or WM_WIN_CREATE_AT).  When use_pos is
 * non-zero, px/py are packed into req->d as (x<<16)|y so
 * wsd uses them verbatim. */
static int wm_create_window_impl(uint32_t w, uint32_t h, uint32_t flags,
                                 const char *title,
                                 int use_pos, uint32_t px, uint32_t py,
                                 struct wm_window *out);

int wm_create_window_input(uint32_t w, uint32_t h, uint32_t flags,
                           const char *title,
                           struct wm_window *out)
{
    return wm_create_window_impl(w, h, flags, title, 0, 0, 0, out);
}

int wm_create_window_at(uint32_t w, uint32_t h, uint32_t flags,
                        uint32_t x, uint32_t y,
                        const char *title,
                        struct wm_window *out)
{
    return wm_create_window_impl(w, h, flags, title, 1, x, y, out);
}

static int wm_create_window_impl(uint32_t w, uint32_t h, uint32_t flags,
                                 const char *title,
                                 int use_pos, uint32_t px, uint32_t py,
                                 struct wm_window *out)
{
    /* Zero by hand -- struct contains a gui_fb whose pixels
     * pointer is uint8_t* (== 0 here means "not mapped"). */
    out->fb.pixels = 0;
    out->fb.stride = 0;
    out->fb.w      = 0;
    out->fb.h      = 0;
    out->fb.id     = 0;
    out->id        = 0;
    out->fb_id     = 0;
    out->x         = 0;
    out->y         = 0;
    out->kernel_id = -1;

    if (wm_connect() < 0) return -1;

    /* 1. CREATE / CREATE_AT -- wsd assigns id, FB, and
     *    either a cascade slot (use_pos==0) or the caller's
     *    explicit (px,py).  Either path returns the assigned
     *    (x,y) in rep.b/rep.c so we always know where the
     *    window actually landed. */
    struct wm_msg req;
    req.status = 0;
    req.a = w; req.b = h; req.c = flags;
    if (use_pos) {
        req.op = WM_WIN_CREATE_AT;
        req.d  = ((px & 0xFFFFu) << 16) | (py & 0xFFFFu);
    } else {
        req.op = WM_WIN_CREATE;
        req.d  = 0;
    }
    if (wm_send(&req) < 0) return -1;

    struct wm_msg rep;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != req.op || rep.status != WM_OK || rep.a == 0) {
        printf("[wmclient] CREATE failed status=%d id=%u op=%u\n",
               (int)rep.status, (unsigned)rep.a, (unsigned)req.op);
        return -1;
    }
    uint32_t id = rep.a;
    uint32_t ax = rep.b;
    uint32_t ay = rep.c;

    /* 2. MAP_FB -- wsd returns the kernel-side win_fb id and
     *    geometry; we then call SYS_WIN_FB_MAP locally to
     *    install the same physical pages into our AS. */
    req.op = WM_WIN_MAP_FB; req.status = 0;
    req.a = id; req.b = 0; req.c = 0; req.d = 0;
    if (wm_send(&req) < 0) return -1;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != WM_WIN_MAP_FB || rep.status != WM_OK || rep.a == 0) {
        printf("[wmclient] MAP_FB failed status=%d fb_id=%u\n",
               (int)rep.status, (unsigned)rep.a);
        /* Best effort: tell wsd to drop the window. */
        struct wm_msg destroy;
        destroy.op = WM_WIN_DESTROY; destroy.status = 0;
        destroy.a = id; destroy.b = 0; destroy.c = 0; destroy.d = 0;
        (void)wm_send(&destroy);
        (void)wm_recv(&destroy);
        return -1;
    }
    uint32_t fb_id   = rep.a;
    uint32_t fb_w    = rep.b;
    uint32_t fb_h    = rep.c;
    uint32_t fb_str  = rep.d;

    /* 3. SYS_WIN_FB_MAP -- install the FB pages into our AS. */
    struct win_fb_map_args ma;
    int r = win_fb_map(fb_id, &ma);
    if (r != 0 || ma.va == 0) {
        printf("[wmclient] win_fb_map failed r=%d va=0x%lx\n",
               r, (unsigned long)ma.va);
        struct wm_msg destroy;
        destroy.op = WM_WIN_DESTROY; destroy.status = 0;
        destroy.a = id; destroy.b = 0; destroy.c = 0; destroy.d = 0;
        (void)wm_send(&destroy);
        (void)wm_recv(&destroy);
        return -1;
    }
    /* Cross-check geometry: a mismatch would mean wsd's
     * table is out of sync with the kernel win_fb table, a
     * serious bug worth catching loudly rather than
     * tolerating. */
    if (ma.w != fb_w || ma.h != fb_h || ma.stride != fb_str) {
        printf("[wmclient] geom mismatch wsd=%ux%u/%u kernel=%ux%u/%u\n",
               (unsigned)fb_w, (unsigned)fb_h, (unsigned)fb_str,
               (unsigned)ma.w, (unsigned)ma.h, (unsigned)ma.stride);
        struct wm_msg destroy;
        destroy.op = WM_WIN_DESTROY; destroy.status = 0;
        destroy.a = id; destroy.b = 0; destroy.c = 0; destroy.d = 0;
        (void)wm_send(&destroy);
        (void)wm_recv(&destroy);
        return -1;
    }

    out->fb.pixels = (uint8_t *)(uintptr_t)ma.va;
    out->fb.stride = ma.stride;
    out->fb.w      = ma.w;
    out->fb.h      = ma.h;
    out->fb.id     = id;    /* keep gui_fb.id in sync for draw_text plumbing */
    out->id        = id;
    out->fb_id     = fb_id;
    out->x         = ax;
    out->y         = ay;
    printf("[wmclient] window id=%u %ux%u pos=%u,%u fb_va=0x%lx\n",
           (unsigned)id, (unsigned)fb_w, (unsigned)fb_h,
           (unsigned)ax, (unsigned)ay, (unsigned long)ma.va);

    /* If the caller asked for input routing,
     * open a NO_DECORATION kernel-WM shadow window at the
     * same scanout position so the kernel can hit-test the
     * pointer and queue events to this process.  No pixel
     * data ever flows through this window (kernel WM
     * compose is stubbed); it exists purely as an input
     * sink. */
    if (title != NULL) {
        /* chapter 108e -- strip GUI_WIN_FLAG_RESIZABLE before
         * passing flags to the kernel shadow.  Resize lives
         * entirely in wsd (grip paint + hit-test +
         * SYS_WIN_FB_RESIZE call), and the kernel WM rejects
         * NO_DECORATION + RESIZABLE with -EINVAL because the
         * grip has no decoration to anchor to in its world.
         * Keeping the bit out of the shadow flags is the
         * only sensible interpretation: the shadow is an
         * input rect, not a window, and "RESIZABLE input
         * rect" is meaningless. */
        uint32_t shadow_flags = (GUI_WIN_FLAG_NO_DECORATION | flags)
                              & ~GUI_WIN_FLAG_RESIZABLE;
        int kid = gui_create_window_ex(
            ma.w, ma.h, title,
            shadow_flags,
            (int32_t)ax, (int32_t)ay);
        if (kid < 0) {
            printf("[wmclient] kernel shadow failed err=%d "
                   "(input routing disabled for this window)\n", kid);
            out->kernel_id = -1;
        } else {
            out->kernel_id = kid;
            printf("[wmclient] kernel shadow id=%d for wsd id=%u\n",
                   kid, (unsigned)id);
            /* chapter 108e -- tell wsd which kernel shadow this
             * wsd window is attached to.  Lets wsd drive title-
             * bar drag (SYS_GUI_MOVE_WINDOW on the shadow) and
             * close-button click (SYS_GUI_DELIVER_EVENT into
             * the shadow's event ring) without a second
             * round-trip through this client. */
            struct wm_msg bind = {0};
            bind.op = WM_WIN_BIND_KERNEL;
            bind.a  = id;
            bind.b  = (uint32_t)kid;
            if (wm_send(&bind) == 0) {
                struct wm_msg bind_rep;
                (void)wm_recv(&bind_rep);
            }
        }
        /* Also publish the title to wsd so it
         * shows up in WM_LIST replies (used by the taskbar).
         * Done unconditionally when title != NULL, regardless
         * of whether the kernel shadow succeeded; the wsd-side
         * title is independent of input routing. */
        (void)wm_set_title(out, title);
    } else {
        out->kernel_id = -1;
    }
    return 0;
}

int wm_poll_event(struct gui_event *out)
{
    if (!out) return -1;
    return gui_poll_event(out);
}

int wm_set_title(struct wm_window *win, const char *title)
{
    if (!win || win->id == 0) return -1;
    if (wm_connect() < 0) return -1;

    /* Compute payload length, clamping to WM_TITLE_MAX-1
     * (the protocol's hard limit; senders MUST stay under
     * the receiver's slot size or wsd will truncate). */
    uint32_t n = 0;
    if (title) {
        while (n < WM_TITLE_MAX - 1 && title[n]) n++;
    }

    struct wm_msg req;
    req.op = WM_WIN_TITLE; req.status = 0;
    req.a  = win->id;
    req.b  = n;
    req.c  = 0;
    req.d  = 0;
    if (wm_send(&req) < 0) return -1;

    if (n > 0) {
        long w = write(g_conn, title, n);
        if (w != (long)n) {
            printf("[wmclient] TITLE payload short write n=%ld\n", w);
            g_conn = -1;
            return -1;
        }
    }

    struct wm_msg rep;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != WM_WIN_TITLE || rep.status != WM_OK) {
        printf("[wmclient] TITLE failed status=%d\n", (int)rep.status);
        return -1;
    }
    return 0;
}

int wm_list_windows(struct wm_win_desc *out, int max)
{
    if (!out || max <= 0) return -1;
    if (wm_connect() < 0) return -1;

    struct wm_msg req;
    req.op = WM_LIST; req.status = 0;
    req.a = 0; req.b = 0; req.c = 0; req.d = 0;
    if (wm_send(&req) < 0) return -1;

    /* WM_LIST reply is header + N descriptors.  Read the
     * header first to learn N, then the descriptor array.
     * The chapter-107 IPC layer is atomic-datagram so wsd's
     * single big write lands as a single read here -- but
     * read() only fills up to the buffer size, so we cap at
     * what the caller asked for. */
    uint8_t buf[WM_LIST_REPLY_MAX];
    long got = read(g_conn, buf, sizeof(buf));
    if (got < (long)sizeof(struct wm_msg)) {
        printf("[wmclient] LIST short reply got=%ld\n", got);
        g_conn = -1;
        return -1;
    }
    struct wm_msg *rep = (struct wm_msg *)buf;
    if (rep->op != WM_LIST || rep->status != WM_OK) {
        printf("[wmclient] LIST failed status=%d\n", (int)rep->status);
        return -1;
    }
    uint32_t n = rep->a;
    if (n > (uint32_t)max) n = (uint32_t)max;

    const struct wm_win_desc *src =
        (const struct wm_win_desc *)(buf + sizeof(struct wm_msg));
    /* Bounds check: header + n*desc must fit within what we
     * actually read.  If wsd lied about n, clip to safety. */
    size_t needed = sizeof(struct wm_msg) + (size_t)n * sizeof(*src);
    if (needed > (size_t)got) {
        n = (uint32_t)(((size_t)got - sizeof(struct wm_msg))
                       / sizeof(*src));
    }
    /* Manual struct copy (freestanding C; no memcpy in hot
     * paths -- see /memories/freestanding-c-memset-trap.md). */
    for (uint32_t i = 0; i < n; i++) {
        out[i].win_id        = src[i].win_id;
        out[i].owner_session = src[i].owner_session;
        out[i].w             = src[i].w;
        out[i].h             = src[i].h;
        out[i].flags         = src[i].flags;
        out[i].x             = src[i].x;
        out[i].y             = src[i].y;
        for (uint32_t t = 0; t < sizeof(out[i].title); t++)
            out[i].title[t] = src[i].title[t];
    }
    return (int)n;
}

int wm_window_dirty(struct wm_window *win,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!win || win->id == 0 || g_conn < 0) return -1;
    if (w == 0 || h == 0) return 0;

    struct wm_msg req;
    req.op = WM_WIN_DAMAGE; req.status = 0;
    req.a  = win->id;
    req.b  = x;
    req.c  = y;
    req.d  = WM_DAMAGE_PACK_WH(w, h);
    if (wm_send(&req) < 0) return -1;

    struct wm_msg rep;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != WM_WIN_DAMAGE || rep.status != WM_OK) {
        printf("[wmclient] DAMAGE failed status=%d\n", (int)rep.status);
        return -1;
    }
    return 0;
}

int wm_window_move(struct wm_window *win, uint32_t x, uint32_t y)
{
    if (!win || win->id == 0 || g_conn < 0) return -1;

    struct wm_msg req;
    req.op = WM_WIN_MOVE; req.status = 0;
    req.a = win->id; req.b = x; req.c = y; req.d = 0;
    if (wm_send(&req) < 0) return -1;

    struct wm_msg rep;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != WM_WIN_MOVE || rep.status != WM_OK) {
        printf("[wmclient] MOVE failed status=%d\n", (int)rep.status);
        return -1;
    }
    win->x = x;
    win->y = y;
    return 0;
}

/* chapter 108e — restore a window by id (not by struct
 * wm_window*).  The taskbar doesn't OWN the windows it
 * lists — it just sees them in WM_LIST — so it has no
 * wm_window struct to hand back, only the win_id.  Sends
 * WM_WIN_RESTORE; wsd clears its hidden bit, re-raises,
 * and full-recomposes.  Returns 0 on WM_OK, -1 otherwise.
 *
 * Safe to call on a window that isn't minimized: wsd
 * treats it as a no-op and replies WM_OK. */
int wm_window_restore_id(uint32_t win_id)
{
    if (win_id == 0 || g_conn < 0) return -1;

    struct wm_msg req;
    req.op = WM_WIN_RESTORE; req.status = 0;
    req.a = win_id; req.b = 0; req.c = 0; req.d = 0;
    if (wm_send(&req) < 0) return -1;

    struct wm_msg rep;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != WM_WIN_RESTORE || rep.status != WM_OK) {
        printf("[wmclient] RESTORE id=%u failed op=%d status=%d\n",
               (unsigned)win_id, (int)rep.op, (int)rep.status);
        return -1;
    }
    return 0;
}

/* Symmetric to wm_window_restore_id: hide the named window.
 * Sends WM_WIN_MINIMIZE; wsd sets hidden=1, mirrors to the
 * kernel shadow via gui_set_minimized, and recomposes.
 *
 * Apps use this to behave like a Start menu: the launcher
 * calls it on its own win_id from the close-button /
 * post-spawn / ESC paths so the window stays around for the
 * next click on its taskbar cell instead of exiting.  Safe
 * to call on an already-hidden window (no-op, replies
 * WM_OK). */
int wm_window_minimize_id(uint32_t win_id)
{
    if (win_id == 0 || g_conn < 0) return -1;

    struct wm_msg req;
    req.op = WM_WIN_MINIMIZE; req.status = 0;
    req.a = win_id; req.b = 0; req.c = 0; req.d = 0;
    if (wm_send(&req) < 0) return -1;

    struct wm_msg rep;
    if (wm_recv(&rep) < 0) return -1;
    if (rep.op != WM_WIN_MINIMIZE || rep.status != WM_OK) {
        printf("[wmclient] MINIMIZE id=%u failed op=%d status=%d\n",
               (unsigned)win_id, (int)rep.op, (int)rep.status);
        return -1;
    }
    return 0;
}

/* chapter 108e (revised by follow-up #3) -- re-establish our
 * win_fb mapping after a wsd-driven resize.  When the user
 * drags the bottom-right grip wsd calls SYS_WIN_FB_RESIZE
 * which:
 *   1. allocates fresh backing pages,
 *   2. keeps our OLD VA mapped to OLD pages (lazy unmap so
 *      we don't translation-fault mid-render),
 *   3. reinstalls a NEW VA for wsd (the owner).
 * The OLD pages live until we call this function: SYS_WIN_FB_MAP
 * detects our stale mapping, uninstalls the old VA, installs a
 * fresh one against the new pages, and (when we were the last
 * reference) frees the old backing.  Must be called from the
 * GUI_EVENT_RESIZE handler before the next paint; without it
 * we'd keep painting onto the soon-to-be-collected old pages.
 *
 * On failure: zero out win->fb.pixels so a defensive caller
 * gets a NULL deref rather than a silent fault.  win->id /
 * win->kernel_id are left untouched -- the window is still
 * alive on the wsd side, the caller can retry on the next
 * resize, or fall through to wm_destroy_window. */
int wm_window_remap_fb(struct wm_window *win)
{
    if (!win || win->id == 0) return -1;
    if (g_conn < 0 && wm_connect() < 0) return -1;

    /* 1. Ask wsd for the post-resize FB id + geometry.
     *    wsd's WM_WIN_MAP_FB handler returns the SAME fb_id
     *    we got at create time (kernel keeps the win_fb slot
     *    across resize -- only the backing pages change). */
    struct wm_msg req;
    req.op = WM_WIN_MAP_FB; req.status = 0;
    req.a = win->id; req.b = 0; req.c = 0; req.d = 0;
    if (wm_send(&req) < 0) {
        win->fb.pixels = 0;
        return -1;
    }
    struct wm_msg rep;
    if (wm_recv(&rep) < 0) {
        win->fb.pixels = 0;
        return -1;
    }
    if (rep.op != WM_WIN_MAP_FB || rep.status != WM_OK || rep.a == 0) {
        printf("[wmclient] remap MAP_FB id=%u failed status=%d\n",
               (unsigned)win->id, (int)rep.status);
        win->fb.pixels = 0;
        return -1;
    }
    uint32_t fb_id  = rep.a;
    uint32_t fb_w   = rep.b;
    uint32_t fb_h   = rep.c;
    uint32_t fb_str = rep.d;

    /* 2. Re-run SYS_WIN_FB_MAP -- the kernel uninstalled
     *    our previous mapping during sys_win_fb_resize, so
     *    this is a fresh install (not the idempotent
     *    same-AS hit). */
    struct win_fb_map_args ma;
    int r = win_fb_map(fb_id, &ma);
    if (r != 0 || ma.va == 0) {
        printf("[wmclient] remap win_fb_map id=%u failed r=%d va=0x%lx\n",
               (unsigned)fb_id, r, (unsigned long)ma.va);
        win->fb.pixels = 0;
        return -1;
    }
    if (ma.w != fb_w || ma.h != fb_h || ma.stride != fb_str) {
        printf("[wmclient] remap geom mismatch wsd=%ux%u/%u kernel=%ux%u/%u\n",
               (unsigned)fb_w, (unsigned)fb_h, (unsigned)fb_str,
               (unsigned)ma.w, (unsigned)ma.h, (unsigned)ma.stride);
        /* Use kernel's numbers -- they're what the pixels
         * actually back -- but keep going. */
    }

    win->fb.pixels = (uint8_t *)(uintptr_t)ma.va;
    win->fb.stride = ma.stride;
    win->fb.w      = ma.w;
    win->fb.h      = ma.h;
    win->fb.id     = win->id;     /* gui_fb.id is the wsd window id */
    win->fb_id     = fb_id;
    printf("[wmclient] remap id=%u %ux%u fb_va=0x%lx\n",
           (unsigned)win->id, (unsigned)ma.w, (unsigned)ma.h,
           (unsigned long)ma.va);
    return 0;
}

int wm_destroy_window(struct wm_window *win)
{
    if (!win || win->id == 0) return 0;

    /* Tear down the kernel shadow first so a
     * slow wsd doesn't leave us holding a kernel window
     * for an already-destroyed wsd one. */
    if (win->kernel_id >= 0) {
        (void)gui_destroy_window(win->kernel_id);
        win->kernel_id = -1;
    }

    if (g_conn < 0) {
        /* Conn dropped; wsd will GC the window on its side
         * when it notices the disconnect.  Locally clear so
         * accidental reuse is safe. */
        win->id = 0; win->fb_id = 0; win->fb.pixels = 0;
        return 0;
    }
    struct wm_msg req;
    req.op = WM_WIN_DESTROY; req.status = 0;
    req.a = win->id; req.b = 0; req.c = 0; req.d = 0;
    int rc = 0;
    if (wm_send(&req) == 0) {
        struct wm_msg rep;
        if (wm_recv(&rep) < 0 || rep.op != WM_WIN_DESTROY
            || rep.status != WM_OK) {
            rc = -1;
        }
    } else {
        rc = -1;
    }
    /* Zero the handle regardless -- wsd will GC on its side
     * even if our write/read round-trip failed. */
    win->id        = 0;
    win->fb_id     = 0;
    win->fb.pixels = 0;
    win->fb.stride = 0;
    win->fb.w      = 0;
    win->fb.h      = 0;
    win->fb.id     = 0;
    return rc;
}

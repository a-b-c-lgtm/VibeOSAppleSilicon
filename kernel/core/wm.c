/*
 * kernel/core/wm.c — minimal in-kernel window manager (milestone 40).
 *
 * Implementation notes
 * --------------------
 * - Window pixel buffers are kheap-allocated BGRA arrays (4 bytes
 *   per pixel).  Maximum window size is bounded so we cannot exhaust
 *   the kernel heap with a single sys_gui_create_window call.
 * - The window list is a static array of WM_MAX_WINDOWS entries.
 *   `id == array index` for the lifetime of the window.
 * - Z-order is a small monotonically-increasing integer; the topmost
 *   window has the highest z.  `wm_focus_id` always tracks the
 *   topmost window's id.
 * - Every mutator that changes pixels on screen calls `compose_all`
 *   which re-blits all windows over a wallpaper, then `fb_present`s
 *   the bounding rectangle of the affected windows.  This is O(N)
 *   per flush which is fine for N <= 16.
 * - Decorations are painted by the WM, NOT by the app.  The app's
 *   pixel buffer is just the content area (w x h); the WM adds a
 *   title bar (WM_TITLE_HEIGHT) and a 1-px border.  Coordinates the
 *   app passes to wm_present / wm_fill_rect / wm_draw_text are
 *   interpreted relative to the content area.
 */

#include "wm.h"
#include "heap.h"
#include "serial.h"
#include "uaccess.h"
#include "thread.h"
#include "pmem.h"
#include "wm_font.h"
#include "../arch/address_space.h"
#include "../device/fb.h"
#include "../device/font.h"
#include "../device/text.h"

#include <stddef.h>
#include <stdint.h>

#ifndef EINVAL
#define EINVAL 22
#define ENOMEM 12
#define EPERM   1
#define ENOENT  2
#define EFAULT 14
#define ENOSPC 28
#define EBUSY  16
#endif

/* GCC-emitted memset/memcpy fallbacks for freestanding builds.  */
__attribute__((weak))
void *memset(void *d, int c, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
__attribute__((weak))
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}

struct gui_event_ring {
    struct gui_event slots[WM_EVENT_QUEUE_CAP];
    uint32_t head;          /* producer index (next write) */
    uint32_t tail;          /* consumer index (next read) */
};

struct wm_window {
    int      in_use;
    int32_t  id;
    uint64_t owner_pid;
    uint32_t z;             /* monotonically-increasing z order */
    uint32_t flags;         /* GUI_WIN_FLAG_* — milestone 47 */
    int      minimized;     /* milestone 51: hidden but live */
    int32_t  x, y;          /* origin of decoration in framebuffer pixels */
    uint32_t w, h;          /* content-area dimensions (excludes deco) */
    uint8_t *pixels;        /* w*h*4 BGRA, kheap-allocated */
    char     title[WM_TITLE_MAX + 1];
    struct gui_event_ring events;
    /* Chapter 108a \u2014 userspace pixel-buffer mapping.  Lazily
     * populated by wm_map_window; cleared by wm_unmap_window /
     * wm_destroy_window / wm_destroy_owner.
     *
     *   user_pages_n   : number of 4 KiB frames in the mapping.
     *                    0 means "no mapping installed."  When
     *                    nonzero we also know which AS the
     *                    mapping lives in (the owner's, by
     *                    construction \u2014 only the owner can map
     *                    a window).
     *   user_pages_pa  : kheap-allocated array of length
     *                    user_pages_n, holding the physical
     *                    addresses of the mapped frames in
     *                    user-VA order.  Used both for tearing
     *                    the mapping down and for the wm_damage
     *                    copy back into `pixels`.
     *   user_va        : the user-VA base of the mapping.
     *   user_as        : non-owning pointer to the owner's
     *                    address space (we don't refcount it;
     *                    on owner exit, wm_destroy_owner is
     *                    called BEFORE address_space_destroy
     *                    so we can still uninstall).
     */
    uint32_t              user_pages_n;
    uint64_t              user_va;
    uint64_t             *user_pages_pa;
    struct address_space *user_as;

    /* chapter 108e -- when set, the kernel WM's pointer router
     * skips this window entirely.  hit_test() pretends the window
     * isn't there; the focused-window MOVE/UP forwarding path
     * also bails before pushing to this window's event ring.
     * Set by wsd via SYS_GUI_SET_INPUT_PASSTHROUGH on every
     * shadow it binds (WM_WIN_BIND_KERNEL).  Once passthrough is
     * on, wsd is the sole authority on input routing for this
     * window: it does hit-test in its own z-order and injects
     * MOUSE_DOWN / MOUSE_UP / MOUSE_MOVE via
     * SYS_GUI_DELIVER_EVENT.  Keyboard input still flows via the
     * kernel's g_focus_id, which wsd keeps in sync by calling
     * SYS_GUI_RAISE_WINDOW after each click-to-raise. */
    int                   input_passthrough;
};

static struct wm_window g_wins[WM_MAX_WINDOWS];
static uint32_t         g_next_z      = 1;
/* Auto-cascade step counter, incremented only when a window is
 * created with GUI_WIN_POS_AUTO.  Decoupled from the array-slot
 * index so an explicitly-positioned window (e.g. the taskbar that
 * pins itself to the bottom of the framebuffer) doesn't push the
 * next auto-positioned window into a non-(80,60) slot. */
static uint32_t         g_next_cascade = 0;
static int32_t          g_focus_id    = -1;

/* Per-key held-state for the currently focused window: 1 if the
 * key has been delivered as GUI_EVENT_KEY but no matching
 * GUI_EVENT_KEY_UP has been delivered yet.  Indexed by GUI key
 * code, so ASCII 0..255 share the table with the GUI_KEY_*
 * extended codes (0x101..0x108).  Used for two things:
 *
 *   1. Drop spurious / duplicate releases — wm_keyboard_release
 *      returns immediately if g_keys_held[key] is 0, so a release
 *      that arrives after focus has already moved doesn't show up
 *      as a phantom UP in the new focus's ring.
 *   2. Synthesise releases on focus change so the OLD focused
 *      window sees a clean UP for every key still down.  Without
 *      this, an app like Doom that maintains gamekeydown[] would
 *      have a stuck "walking forward" key the moment the user
 *      alt-tabbed or clicked another window mid-walk.
 *
 * The table is cleared whenever focus shifts, so it always
 * reflects the held state for the *current* g_focus_id and nothing
 * else.  Size 0x110 covers ASCII + the GUI_KEY_* range with a
 * little headroom; deliver_key range-checks before indexing. */
static uint8_t          g_keys_held[0x110];

/* ---- mouse / pointer state (milestone 41) ---- */
static int32_t  g_pointer_x  = -1;          /* -1 = uninitialised */
static int32_t  g_pointer_y  = -1;
static uint32_t g_buttons    = 0;           /* GUI_BTN_* bitmap */

/* When the user is dragging a title bar, g_drag_id is the window
 * being dragged and g_drag_dx/dy is the cursor offset within the
 * decoration when the drag started. */
static int32_t  g_drag_id    = -1;
static int32_t  g_drag_dx    = 0;
static int32_t  g_drag_dy    = 0;

/* When the user is dragging the bottom-right grip of a RESIZABLE
 * window, g_resize_id is the window id, g_resize_grip_dx/dy is the
 * cursor offset within the grip square at drag start (so the grip
 * tracks the cursor without snapping to the corner), and
 * g_resize_origin_w/h is the window size at drag start so we can
 * recompute the new size from absolute cursor position rather than
 * accumulating per-event deltas (which would drift on coalesced
 * MOUSE_MOVE events). */
static int32_t  g_resize_id    = -1;
static int32_t  g_resize_grip_dx = 0;
static int32_t  g_resize_grip_dy = 0;
static uint32_t g_resize_origin_w = 0;
static uint32_t g_resize_origin_h = 0;
static int32_t  g_resize_origin_x = 0;
static int32_t  g_resize_origin_y = 0;

/* ---- keyboard CSI parser state (see wm_keyboard_byte) ----
 *
 *   state 0 : idle.  ASCII bytes deliver immediately; ESC moves
 *             us into state 1 and is held back.
 *   state 1 : got an ESC, waiting for '['.
 *   state 2 : got "ESC [", waiting for either a single-byte
 *             final (A/B/C/D/H/F) OR a parameter digit.
 *   state 3 : got "ESC [ <digit>", waiting for the '~' final
 *             of the parametric form (PageUp = `5~`,
 *             PageDown = `6~`, Insert = `2~`, etc).  The
 *             accumulated parameter is in g_csi_param. */
static int g_csi_state = 0;
static int g_csi_param = 0;

/* Width of the close button on the right side of the title bar. */
#define WM_CLOSE_BTN_W   20
/* Width of the milestone-51 minimize button, drawn immediately to
 * the LEFT of the close button.  Same height as the close button
 * (WM_TITLE_HEIGHT - 4); same 2-px gap to the right edge of the
 * title bar applies cumulatively. */
#define WM_MIN_BTN_W     20
#define WM_BTN_GAP        2

/* ---- ring helpers ---- */
static int ring_push(struct gui_event_ring *r, const struct gui_event *ev)
{
    uint32_t next = (r->head + 1) % WM_EVENT_QUEUE_CAP;
    if (next == r->tail) return 0;       /* full → drop newest */
    r->slots[r->head] = *ev;
    r->head = next;
    return 1;
}
static int ring_pop(struct gui_event_ring *r, struct gui_event *out)
{
    if (r->head == r->tail) return 0;
    *out = r->slots[r->tail];
    r->tail = (r->tail + 1) % WM_EVENT_QUEUE_CAP;
    return 1;
}

/* If the most recently-pushed (and not-yet-consumed) event in the
 * ring is a MOUSE_MOVE for the same button bitmap as `ev`, overwrite
 * its (x, y) with the new coordinates and return 1.  Otherwise
 * return 0 and the caller should ring_push() normally.
 *
 * This bounds the per-window backlog of MOUSE_MOVE events to one,
 * so that under fast drags the consuming app always sees the most
 * recent cursor position on its next poll instead of working
 * through tens of stale intermediate positions.  We only coalesce
 * when the button bitmap is identical to avoid hiding a press/
 * release event boundary, although in practice button transitions
 * are MOUSE_DOWN / MOUSE_UP events, never MOUSE_MOVE. */
static int ring_coalesce_mouse_move(struct gui_event_ring *r,
                                    const struct gui_event *ev)
{
    if (r->head == r->tail) return 0;       /* ring is empty */
    uint32_t last = (r->head + WM_EVENT_QUEUE_CAP - 1) %
                    WM_EVENT_QUEUE_CAP;
    if (r->slots[last].type != GUI_EVENT_MOUSE_MOVE) return 0;
    if (r->slots[last].arg2 != ev->arg2)             return 0;
    r->slots[last].arg0 = ev->arg0;
    r->slots[last].arg1 = ev->arg1;
    return 1;
}

/* Same idea as the MOUSE_MOVE coalescer but for GUI_EVENT_RESIZE.
 * A continuous grip-drag would otherwise queue dozens of
 * intermediate sizes; we only need the latest because re-layout
 * is expensive.  Returns 1 if the trailing event was overwritten,
 * 0 if the caller should ring_push as usual. */
static int ring_coalesce_resize(struct gui_event_ring *r,
                                const struct gui_event *ev)
{
    if (r->head == r->tail) return 0;
    uint32_t last = (r->head + WM_EVENT_QUEUE_CAP - 1) %
                    WM_EVENT_QUEUE_CAP;
    if (r->slots[last].type != GUI_EVENT_RESIZE) return 0;
    r->slots[last].arg0 = ev->arg0;
    r->slots[last].arg1 = ev->arg1;
    return 1;
}

/* Forward-decl: build a GUI_EVENT_KEY and push it into the focused
 * window's ring.  `key` is either a printable byte (0..255) or one
 * of the GUI_KEY_* extended codes. */
static struct wm_window *win_by_id(int32_t id);
static int deliver_key(struct wm_window *w, uint32_t key)
{
    struct gui_event ev = (struct gui_event){
        .type = GUI_EVENT_KEY,
        .window_id = w->id,
        .arg0 = key,
        .arg1 = 0, .arg2 = 0, .arg3 = 0,
    };
    ring_push(&w->events, &ev);
    /* Track held-state so wm_keyboard_release can pair UP with
     * DOWN, and so the focus-change synthesiser can flush stuck
     * keys to the old window.  Auto-repeat (virtio_input delivers
     * KEY_VAL_REPEAT as another deliver_key call) just keeps the
     * bit set; it was already 1. */
    if (key < (uint32_t)(sizeof g_keys_held))
        g_keys_held[key] = 1;
    return 1;
}

/* Push a GUI_EVENT_KEY_UP for every key currently marked held to
 * `w`'s event ring, then clear the held-state table.  Used by
 * set_focus() so the OLD focused window sees a release for every
 * key it ever saw a press for before focus moves away.  If `w` is
 * NULL (the old focus was already destroyed), just clear the
 * table — there's nothing to release to. */
static void synthesize_releases_to(struct wm_window *w)
{
    for (uint32_t k = 0; k < (uint32_t)(sizeof g_keys_held); k++) {
        if (!g_keys_held[k]) continue;
        g_keys_held[k] = 0;
        if (w) {
            struct gui_event ev = (struct gui_event){
                .type = GUI_EVENT_KEY_UP,
                .window_id = w->id,
                .arg0 = k,
                .arg1 = 0, .arg2 = 0, .arg3 = 0,
            };
            ring_push(&w->events, &ev);
        }
    }
}

/* All focus-id transitions funnel through here so a single place is
 * responsible for flushing stuck keys to the previously-focused
 * window.  No-op when new_id == g_focus_id (so repeated raises of
 * the same window don't generate spurious UPs). */
static void set_focus(int32_t new_id)
{
    if (new_id == g_focus_id) return;
    if (g_focus_id >= 0)
        synthesize_releases_to(win_by_id(g_focus_id));
    g_focus_id = new_id;
}

/* ---- color helpers ---- */
static inline uint32_t bgra_pack(struct fb_color c)
{
    /* B in low byte, then G, then R, then 0 (ignored alpha). */
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}
static inline struct fb_color bgra_unpack(uint32_t v)
{
    return (struct fb_color){
        .r = (uint8_t)((v >> 16) & 0xFFu),
        .g = (uint8_t)((v >> 8)  & 0xFFu),
        .b = (uint8_t)( v        & 0xFFu),
        .a = 0xFF,
    };
}

/* ---- window / id helpers ---- */
static struct wm_window *win_by_id(int32_t id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS) return NULL;
    if (!g_wins[id].in_use) return NULL;
    return &g_wins[id];
}
static struct wm_window *win_owned_by(int32_t id, uint64_t pid)
{
    struct wm_window *w = win_by_id(id);
    if (!w) return NULL;
    if (w->owner_pid != pid) return NULL;
    return w;
}
/* Forward decl: defined down at the chapter-108a block but called
 * by the destroy paths just below. */
static void wm_drop_user_pages(struct wm_window *w);

static int32_t topmost_id(void)
{
    int32_t  best_id = -1;
    uint32_t best_z  = 0;
    for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_wins[i].in_use && g_wins[i].z >= best_z) {
            best_z  = g_wins[i].z;
            best_id = i;
        }
    }
    return best_id;
}

/* ---- painting (chapter 108d: retired) ----
 *
 * Prior to chapter 108d the kernel WM owned the entire scanout
 * paint path: paint_wallpaper, blit_window (with decoration,
 * close button, minimise button, resize grip, content blit),
 * blit_cursor (a hand-rolled X11-style sprite), wm_draw_text_fb
 * + wm_text_glyph + wm_blend_pixel for TTF/fontd-backed title
 * text, and compose_all() that walked the window table in
 * three z-order passes (pin-to-bottom, regular, always-on-top)
 * then called fb_present() to push the result to virtio-gpu.
 *
 * Every public WM mutator below used to end with a call to
 * compose_all().  This chapter moves the compositor into
 * userspace (/bin/wsd via /srv/wm) so the kernel keeps only
 * the window table, focus tracking, input routing into per-
 * window event queues, and the chapter-108a backing-buffer
 * mappings — none of which need to touch the scanout.
 *
 * What got deleted in this slice:
 *   - paint_wallpaper(), blit_window(), blit_cursor()
 *   - wm_draw_text_fb(), wm_text_glyph(), wm_blend_pixel()
 *   - the CURSOR_BITMAP[] sprite
 *   - DECO_BG, DECO_BG_F, DECO_FG, BORDER colour constants
 *   - WALLPAPER, WALLPAPER_TOP colour constants
 *   - the g_wm_painted_wallpaper bit (never read elsewhere)
 *   - compose_all() and every caller's call to it
 *   - kernel/core/wm_font.{c,h} entirely (its only caller
 *     was wm_text_glyph)
 *
 * What got stubbed (still called by legacy apps, must not
 * crash, but does no painting):
 *   - wm_present()    — returns 0
 *   - wm_fill_rect()  — returns 0
 *   - wm_draw_text()  — returns 0
 *   - wm_flush()      — returns 0
 *
 * What stayed (kernel still owns it):
 *   - g_wins[] window table, slot allocation, owner pid,
 *     focus tracking (g_focus_id), drag tracking (g_drag_id),
 *     resize tracking
 *   - g_pointer_x/y position tracking (still used by the
 *     pointer router to decide which window an EV_ABS hits)
 *   - all input routing (wm_keyboard_byte, wm_pointer_move,
 *     wm_pointer_button) — pixel-side compose_all() calls
 *     deleted; event-queue enqueues stay
 *   - chapter 108a wm_map_window / wm_unmap_window / wm_damage
 *     mapped-buffer machinery
 *   - WM_LIST, WM_GET_SCREEN_SIZE, gui_create_window,
 *     gui_destroy_window, gui_window_fb, gui_poll_event,
 *     wm_raise_window, wm_set_minimized
 *
 * Legacy GUI apps (notepad, gui_term, browser, launcher,
 * taskbar, desktop, paint, notify, hellogui, pixapp,
 * save_dialog) were ported to wmclient (userspace/libgui/wmclient.h)
 * and the /srv/wm bus in chapter 108d, one app at a time.
 * Hellowsd (which already uses wmclient) IS visible
 * after this slice — paint the magic 0xff7755aa colour into
 * its window and the wsd compose path puts it on screen
 * for real, no longer overwritten by the kernel compositor
 * because the kernel compositor doesn't exist.
 */


/* ---- public API ---- */

/* Chapter 108d — compose_all is retired.  The 12 in-tree call sites
 * (focus changes, drags, destroys, raises, minimises, damage)
 * used to trigger a kernel-side framebuffer recomposite.  Now
 * that wsd owns the scanout, the right thing is for those state
 * transitions to be observable to wsd over /srv/wm so it can
 * re-render.  Until that wire is built, the call
 * sites stay textually but invoke this no-op.  Keeping the calls
 * preserves the historical paint points so a future refactor
 * knows where the publish-state hooks belong. */
static inline void compose_all(void)
{
    /* intentionally empty */
}

void wm_init(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        g_wins[i].in_use = 0;
    g_next_z   = 1;
    g_focus_id = -1;
    g_drag_id  = -1;
    g_resize_id = -1;
    g_pointer_x = -1;
    g_pointer_y = -1;
    g_buttons   = 0;
}

int wm_has_windows(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_wins[i].in_use) return 1;
    return 0;
}

int wm_keyboard_byte(char c)
{
    if (g_focus_id < 0) { g_csi_state = 0; return 0; }
    struct wm_window *w = win_by_id(g_focus_id);
    if (!w) { g_csi_state = 0; return 0; }

    /* CSI parser state machine.  See the g_csi_state declaration
     * for the per-state semantics.  If the batch ends with state
     * >= 1 (no follow-up bytes), wm_flush_pending_keys is
     * responsible for delivering the orphaned ESC. */
    switch (g_csi_state) {
    case 0:
        if (c == 0x1B) { g_csi_state = 1; return 1; }
        return deliver_key(w, (uint8_t)c);
    case 1:
        if (c == '[') { g_csi_state = 2; g_csi_param = 0; return 1; }
        /* Bare ESC followed by a non-'[' byte.  Flush both. */
        g_csi_state = 0;
        deliver_key(w, 0x1B);
        return wm_keyboard_byte(c);
    case 2: /* ESC [ */
        if (c >= '0' && c <= '9') {
            g_csi_param = g_csi_param * 10 + (c - '0');
            g_csi_state = 3;
            return 1;
        }
        g_csi_state = 0;
        switch (c) {
        case 'A': return deliver_key(w, GUI_KEY_UP);
        case 'B': return deliver_key(w, GUI_KEY_DOWN);
        case 'C': return deliver_key(w, GUI_KEY_RIGHT);
        case 'D': return deliver_key(w, GUI_KEY_LEFT);
        case 'H': return deliver_key(w, GUI_KEY_HOME);
        case 'F': return deliver_key(w, GUI_KEY_END);
        default:  return 1;     /* drop unknown CSI */
        }
    default: /* state 3: ESC [ <digit(s)> */
        if (c >= '0' && c <= '9') {
            g_csi_param = g_csi_param * 10 + (c - '0');
            return 1;
        }
        if (c == '~') {
            int p = g_csi_param;
            g_csi_state = 0; g_csi_param = 0;
            switch (p) {
            case 5: return deliver_key(w, GUI_KEY_PGUP);
            case 6: return deliver_key(w, GUI_KEY_PGDN);
            default: return 1;  /* drop unknown parametric CSI */
            }
        }
        /* Anything else aborts the CSI; drop the partial sequence. */
        g_csi_state = 0; g_csi_param = 0;
        return 1;
    }
}

void wm_flush_pending_keys(void)
{
    if (g_csi_state == 0) return;
    int prev_state = g_csi_state;
    g_csi_state = 0;
    g_csi_param = 0;
    if (g_focus_id < 0) return;
    struct wm_window *w = win_by_id(g_focus_id);
    if (!w) return;
    /* In state 1 we received only ESC.  In states 2/3 we received
     * the start of a CSI; drop the partial sequence and emit the
     * bare ESC, matching what GNU readline does on a CSI timeout. */
    (void)prev_state;
    deliver_key(w, 0x1B);
}

int wm_keyboard_release(uint32_t key)
{
    if (g_focus_id < 0) return 0;
    if (key < (uint32_t)(sizeof g_keys_held)) {
        if (!g_keys_held[key]) return 0;   /* drop spurious release */
        g_keys_held[key] = 0;
    }
    struct wm_window *w = win_by_id(g_focus_id);
    if (!w) return 0;
    struct gui_event ev = (struct gui_event){
        .type = GUI_EVENT_KEY_UP,
        .window_id = w->id,
        .arg0 = key,
        .arg1 = 0, .arg2 = 0, .arg3 = 0,
    };
    ring_push(&w->events, &ev);
    return 1;
}

/* ---- pointer input ---- */

/* Find the topmost window whose decoration rectangle covers (sx,sy).
 * Returns -1 if the click landed on the wallpaper. */
static int32_t hit_test(int32_t sx, int32_t sy)
{
    int32_t  best_id = -1;
    uint32_t best_z  = 0;
    int      best_pinned = 0;
    for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_wins[i].in_use) continue;
        struct wm_window *w = &g_wins[i];
        /* Pin-to-bottom windows (the desktop wallpaper) are
         * click-transparent: pretend they aren't there. */
        if (w->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
        /* Minimized windows aren't on screen — clicks ignore them. */
        if (w->minimized) continue;
        /* chapter 108e -- wsd-routed shadows are invisible to the
         * kernel hit-tester.  wsd does its own hit-test in
         * wsd-z-order and injects events via gui_deliver_event. */
        if (w->input_passthrough) continue;
        int32_t x0 = w->x;
        int32_t y0 = w->y;
        int32_t x1, y1;
        if (w->flags & GUI_WIN_FLAG_NO_DECORATION) {
            x1 = x0 + (int32_t)w->w;
            y1 = y0 + (int32_t)w->h;
        } else {
            x1 = x0 + (int32_t)w->w + 2 * WM_BORDER;
            y1 = y0 + (int32_t)w->h + WM_TITLE_HEIGHT + WM_BORDER;
        }
        if (sx < x0 || sx >= x1) continue;
        if (sy < y0 || sy >= y1) continue;
        int pinned = (w->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP) ? 1 : 0;
        /* Pinned windows beat non-pinned regardless of z (they paint
         * last, so visually they're on top — hit-test must agree). */
        if (best_id < 0 ||
            (pinned && !best_pinned) ||
            (pinned == best_pinned && w->z > best_z)) {
            best_id = i;
            best_z  = w->z;
            best_pinned = pinned;
        }
    }
    return best_id;
}

/* Decompose a screen-relative click for a known window into one of:
 *   'C' = close button
 *   'M' = minimize button (milestone 51)
 *   'R' = bottom-right resize grip (milestone 63)
 *   'T' = title bar (drag handle)
 *   'B' = body / content area (forward to app)
 *   '-' = miss
 * Also returns the click point in window-content coordinates via
 * *cx_out / *cy_out (only meaningful when result == 'B'). */
static char classify_click(const struct wm_window *w,
                           int32_t sx, int32_t sy,
                           int32_t *cx_out, int32_t *cy_out)
{
    /* No-decoration windows have no title/close/border zones — every
     * point inside the window is body. */
    if (w->flags & GUI_WIN_FLAG_NO_DECORATION) {
        if (sx < w->x || sx >= w->x + (int32_t)w->w) return '-';
        if (sy < w->y || sy >= w->y + (int32_t)w->h) return '-';
        if (cx_out) *cx_out = sx - w->x;
        if (cy_out) *cy_out = sy - w->y;
        return 'B';
    }

    int32_t deco_w = (int32_t)w->w + 2 * WM_BORDER;
    int32_t deco_h = (int32_t)w->h + WM_TITLE_HEIGHT + WM_BORDER;
    if (sx < w->x || sx >= w->x + deco_w) return '-';
    if (sy < w->y || sy >= w->y + deco_h) return '-';

    /* Resize grip — checked BEFORE close/minimize so that the
     * bottom-right corner is the grip even if a tiny window's
     * deco rect happens to overlap (it can't, but cheap defence).
     * Only applies to RESIZABLE windows; non-resizable apps treat
     * the corner as ordinary body. */
    if ((w->flags & GUI_WIN_FLAG_RESIZABLE) &&
        deco_w >= WM_GRIP_SIZE + 2 &&
        deco_h >= WM_GRIP_SIZE + WM_TITLE_HEIGHT + 2) {
        int32_t gx0 = w->x + deco_w - WM_GRIP_SIZE - WM_BORDER;
        int32_t gy0 = w->y + deco_h - WM_GRIP_SIZE - WM_BORDER;
        int32_t gx1 = gx0 + WM_GRIP_SIZE;
        int32_t gy1 = gy0 + WM_GRIP_SIZE;
        if (sx >= gx0 && sx < gx1 && sy >= gy0 && sy < gy1)
            return 'R';
    }

    /* Close button rect (mirror of blit_window). */
    if (deco_w >= WM_CLOSE_BTN_W + 4) {
        int32_t bx0 = w->x + deco_w - WM_CLOSE_BTN_W - 2;
        int32_t bx1 = bx0 + WM_CLOSE_BTN_W;
        int32_t by0 = w->y + 2;
        int32_t by1 = by0 + WM_TITLE_HEIGHT - 4;
        if (sx >= bx0 && sx < bx1 && sy >= by0 && sy < by1)
            return 'C';
    }
    /* Minimize button rect (mirror of blit_window).  Same width-
     * gating as the painter so we don't return 'M' for a click on
     * a button that wasn't actually drawn. */
    if (deco_w >= WM_CLOSE_BTN_W + WM_MIN_BTN_W + WM_BTN_GAP + 8) {
        int32_t bx0 = w->x + deco_w
                    - WM_CLOSE_BTN_W - 2 - WM_BTN_GAP - WM_MIN_BTN_W;
        int32_t bx1 = bx0 + WM_MIN_BTN_W;
        int32_t by0 = w->y + 2;
        int32_t by1 = by0 + WM_TITLE_HEIGHT - 4;
        if (sx >= bx0 && sx < bx1 && sy >= by0 && sy < by1)
            return 'M';
    }
    /* Title bar (everything in the deco strip but not the buttons). */
    if (sy < w->y + WM_TITLE_HEIGHT) return 'T';

    /* Content area. */
    int32_t cx = sx - (w->x + WM_BORDER);
    int32_t cy = sy - (w->y + WM_TITLE_HEIGHT);
    if (cx < 0 || cy < 0)                  return '-';
    if (cx >= (int32_t)w->w || cy >= (int32_t)w->h) return '-';
    if (cx_out) *cx_out = cx;
    if (cy_out) *cy_out = cy;
    return 'B';
}

/* Reallocate `w->pixels` to (new_w, new_h), copy as much of the old
 * content as fits to the top-left of the new buffer, fill any new
 * area with the default dark-gray (matching wm_create_window_ex's
 * blanking colour), and push a coalesced GUI_EVENT_RESIZE.  Returns
 * 1 if anything actually changed, 0 if the size was identical or
 * the realloc failed.  Caller must hold the implicit WM lock (we
 * have only one CPU and run from soft IRQ / syscall context).
 *
 * Out-of-memory path: leave the window untouched.  This is a best-
 * effort behaviour — the user just won't get a bigger window when
 * the heap is exhausted.  The grip stays draggable so a subsequent
 * shrink can still succeed. */
static int resize_window_to(struct wm_window *w,
                            uint32_t new_w, uint32_t new_h)
{
    if (new_w < WM_MIN_WIDTH)  new_w = WM_MIN_WIDTH;
    if (new_h < WM_MIN_HEIGHT) new_h = WM_MIN_HEIGHT;
    if (new_w > WM_MAX_WIDTH)  new_w = WM_MAX_WIDTH;
    if (new_h > WM_MAX_HEIGHT) new_h = WM_MAX_HEIGHT;
    if (new_w == w->w && new_h == w->h) return 0;

    size_t bytes = (size_t)new_w * (size_t)new_h * 4u;
    uint8_t *buf = (uint8_t *)kmalloc(bytes);
    if (!buf) return 0;

    /* Default fill: same dark gray that wm_create_window_ex uses
     * so apps that ignore the resize event see a consistent
     * background in any newly-revealed area. */
    for (size_t k = 0; k + 3 < bytes; k += 4) {
        buf[k+0] = 0x20; buf[k+1] = 0x20; buf[k+2] = 0x20; buf[k+3] = 0;
    }

    /* Copy as many rows / columns as fit.  For shrinks we crop;
     * for grows we leave the new strip on the right and bottom
     * filled with the default. */
    uint32_t copy_w = w->w < new_w ? w->w : new_w;
    uint32_t copy_h = w->h < new_h ? w->h : new_h;
    for (uint32_t row = 0; row < copy_h; row++) {
        uint32_t *dst = (uint32_t *)buf      + row * new_w;
        uint32_t *src = (uint32_t *)w->pixels + row * w->w;
        for (uint32_t col = 0; col < copy_w; col++)
            dst[col] = src[col];
    }

    kfree(w->pixels);
    w->pixels = buf;
    w->w = new_w;
    w->h = new_h;

    struct gui_event ev = (struct gui_event){
        .type      = GUI_EVENT_RESIZE,
        .window_id = w->id,
        .arg0      = new_w,
        .arg1      = new_h,
        .arg2      = 0,
        .arg3      = 0,
    };
    if (!ring_coalesce_resize(&w->events, &ev))
        ring_push(&w->events, &ev);
    return 1;
}

void wm_pointer_move(int32_t sx, int32_t sy)
{
    if (g_pointer_x == sx && g_pointer_y == sy) return;
    g_pointer_x = sx;
    g_pointer_y = sy;

    /* If the user is dragging, move the dragged window with the
     * cursor.  If the user is grip-resizing, recompute the new
     * window dimensions from the absolute cursor position (so
     * coalesced MOUSE_MOVE events don't drift) and reallocate the
     * pixel buffer.  Otherwise just deliver a MOUSE_MOVE event to
     * the focused window if the cursor is in its content area. */
    if (g_drag_id >= 0) {
        struct wm_window *w = win_by_id(g_drag_id);
        if (w) {
            w->x = sx - g_drag_dx;
            w->y = sy - g_drag_dy;
        } else {
            g_drag_id = -1;
        }
    } else if (g_resize_id >= 0) {
        struct wm_window *w = win_by_id(g_resize_id);
        if (w && (w->flags & GUI_WIN_FLAG_RESIZABLE)) {
            /* The grip's bottom-right corner should land at
             * (sx + (WM_GRIP_SIZE - g_resize_grip_dx),
             *  sy + (WM_GRIP_SIZE - g_resize_grip_dy)).
             * Working back to the new content w/h:
             *   deco_w = (corner_x - w->x + 1)
             *   new_w  = deco_w - 2 * WM_BORDER
             *   deco_h = (corner_y - w->y + 1)
             *   new_h  = deco_h - WM_TITLE_HEIGHT - WM_BORDER
             */
            int32_t corner_x = sx + (WM_GRIP_SIZE - g_resize_grip_dx) - 1;
            int32_t corner_y = sy + (WM_GRIP_SIZE - g_resize_grip_dy) - 1;
            int32_t new_deco_w = corner_x - w->x + 1;
            int32_t new_deco_h = corner_y - w->y + 1;
            int32_t new_w = new_deco_w - 2 * WM_BORDER;
            int32_t new_h = new_deco_h - WM_TITLE_HEIGHT - WM_BORDER;
            if (new_w < (int32_t)WM_MIN_WIDTH)  new_w = WM_MIN_WIDTH;
            if (new_h < (int32_t)WM_MIN_HEIGHT) new_h = WM_MIN_HEIGHT;
            if (new_w > (int32_t)WM_MAX_WIDTH)  new_w = WM_MAX_WIDTH;
            if (new_h > (int32_t)WM_MAX_HEIGHT) new_h = WM_MAX_HEIGHT;
            (void)g_resize_origin_w; (void)g_resize_origin_h;
            (void)g_resize_origin_x; (void)g_resize_origin_y;
            resize_window_to(w, (uint32_t)new_w, (uint32_t)new_h);
        } else {
            g_resize_id = -1;
        }
    } else if (g_focus_id >= 0) {
        struct wm_window *w = win_by_id(g_focus_id);
        /* chapter 108e -- wsd-routed shadows get their MOVE
         * events injected by wsd, not auto-forwarded by kernel. */
        if (w && w->input_passthrough) w = NULL;
        if (w) {
            int32_t cx = 0, cy = 0;
            if (classify_click(w, sx, sy, &cx, &cy) == 'B') {
                struct gui_event ev = (struct gui_event){
                    .type = GUI_EVENT_MOUSE_MOVE,
                    .window_id = w->id,
                    .arg0 = (uint32_t)cx,
                    .arg1 = (uint32_t)cy,
                    .arg2 = g_buttons,
                    .arg3 = 0,
                };
                /* Coalesce against the trailing MOUSE_MOVE in the
                 * ring (if any) so a fast drag doesn't fill the
                 * 64-slot per-window event queue with intermediate
                 * stale positions.  See ring_coalesce_mouse_move. */
                if (!ring_coalesce_mouse_move(&w->events, &ev))
                    ring_push(&w->events, &ev);
            }
        }
    }
    compose_all();
}

void wm_pointer_button(uint32_t button, int down)
{
    if (down) g_buttons |=  button;
    else      g_buttons &= ~button;

    /* Releases always end any active drag or resize.  A drag in
     * progress captures the mouse, so the button-down case below
     * skips focus / hit-test logic. */
    if (!down && button == GUI_BTN_LEFT && g_drag_id >= 0) {
        g_drag_id = -1;
        compose_all();
        return;
    }
    if (!down && button == GUI_BTN_LEFT && g_resize_id >= 0) {
        g_resize_id = -1;
        compose_all();
        return;
    }

    int32_t sx = g_pointer_x;
    int32_t sy = g_pointer_y;
    if (sx < 0 || sy < 0) return;

    int32_t hit = hit_test(sx, sy);
    if (down && button == GUI_BTN_LEFT) {
        if (hit < 0) {
            /* chapter 108e follow-up -- click landed where the
             * kernel WM sees no window.  In a wsd-managed
             * session this is the common case: every wsd
             * client's shadow is input_passthrough, so kernel
             * hit_test ignores it.  The click most likely hit
             * paint / notepad / etc. and wsd will handle it
             * in its own hit-test below.  Do NOT clear
             * g_focus_id here -- that would silently break
             * keyboard focus on the app the user was using
             * (e.g. ESC stops exiting paint because focus has
             * been quietly handed to nobody).  wsd remains
             * the authority on focus transitions; the kernel
             * just routes keys to whoever wsd last raised. */
            compose_all();
            return;
        }
        struct wm_window *w = &g_wins[hit];
        int32_t cx = 0, cy = 0;
        char zone = classify_click(w, sx, sy, &cx, &cy);

        /* Always raise to top + take focus on left-down.  Pinned
         * (always-on-top) windows skip the raise — they paint on a
         * separate pass anyway, and bumping their z would needlessly
         * inflate g_next_z. */
        if (!(w->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP)) {
            w->z = ++g_next_z;
        }
        set_focus(w->id);

        switch (zone) {
        case 'C': {
            struct gui_event ev = (struct gui_event){
                .type = GUI_EVENT_CLOSE,
                .window_id = w->id,
            };
            ring_push(&w->events, &ev);
            break;
        }
        case 'M':
            /* Minimize self.  wm_set_minimized handles focus
             * hand-off and recompose. */
            wm_set_minimized(w->owner_pid, w->id, 1);
            return;     /* compose_all already called inside */
        case 'T':
            g_drag_id = w->id;
            g_drag_dx = sx - w->x;
            g_drag_dy = sy - w->y;
            break;
        case 'R': {
            /* Begin a grip-resize.  Capture the cursor offset
             * within the grip square so the grip tracks the
             * cursor instead of snapping to the corner. */
            int32_t deco_w = (int32_t)w->w + 2 * WM_BORDER;
            int32_t deco_h = (int32_t)w->h + WM_TITLE_HEIGHT + WM_BORDER;
            int32_t gx0 = w->x + deco_w - WM_GRIP_SIZE - WM_BORDER;
            int32_t gy0 = w->y + deco_h - WM_GRIP_SIZE - WM_BORDER;
            g_resize_id       = w->id;
            g_resize_grip_dx  = sx - gx0;
            g_resize_grip_dy  = sy - gy0;
            g_resize_origin_w = w->w;
            g_resize_origin_h = w->h;
            g_resize_origin_x = w->x;
            g_resize_origin_y = w->y;
            break;
        }
        case 'B': {
            struct gui_event ev = (struct gui_event){
                .type = GUI_EVENT_MOUSE_DOWN,
                .window_id = w->id,
                .arg0 = (uint32_t)cx,
                .arg1 = (uint32_t)cy,
                .arg2 = button,
                .arg3 = g_buttons,
            };
            ring_push(&w->events, &ev);
            break;
        }
        default: break;
        }
        compose_all();
        return;
    }

    /* Non-left button or release: forward to the focused window's
     * content area if applicable. */
    if (g_focus_id >= 0) {
        struct wm_window *w = win_by_id(g_focus_id);
        /* chapter 108e -- wsd-routed shadows get their UP/non-left
         * events injected by wsd, not auto-forwarded by kernel. */
        if (w && w->input_passthrough) w = NULL;
        if (w) {
            int32_t cx = 0, cy = 0;
            if (classify_click(w, sx, sy, &cx, &cy) == 'B') {
                struct gui_event ev = (struct gui_event){
                    .type = down ? GUI_EVENT_MOUSE_DOWN : GUI_EVENT_MOUSE_UP,
                    .window_id = w->id,
                    .arg0 = (uint32_t)cx,
                    .arg1 = (uint32_t)cy,
                    .arg2 = button,
                    .arg3 = g_buttons,
                };
                ring_push(&w->events, &ev);
            }
        }
    }
    compose_all();
}

long wm_create_window_ex(uint64_t pid, uint32_t w, uint32_t h,
                         const char *title_user,
                         uint32_t flags, int32_t want_x, int32_t want_y)
{
    /* Reject unknown flag bits early so future-rev callers don't
     * silently get partial behaviour. */
    if (flags & ~(GUI_WIN_FLAG_NO_DECORATION |
                  GUI_WIN_FLAG_ALWAYS_ON_TOP |
                  GUI_WIN_FLAG_PIN_TO_BOTTOM |
                  GUI_WIN_FLAG_RESIZABLE))
        return -EINVAL;
    /* PIN_TO_BOTTOM and ALWAYS_ON_TOP are mutually exclusive —
     * one paints first, the other paints last; setting both
     * would make compose_all paint the same window twice. */
    if ((flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) &&
        (flags & GUI_WIN_FLAG_ALWAYS_ON_TOP))
        return -EINVAL;
    /* RESIZABLE only makes sense for decorated windows — the grip
     * sits in the bottom-right of the title-bar-bordered rect, and
     * NO_DECORATION windows have no decoration to anchor it to. */
    if ((flags & GUI_WIN_FLAG_RESIZABLE) &&
        (flags & GUI_WIN_FLAG_NO_DECORATION))
        return -EINVAL;
    /* Decorated windows have a 24-px title bar that needs to fit in
     * the content; the body itself must be at least WM_MIN_HEIGHT.
     * Undecorated panels (taskbars, popups) can be much shorter — a
     * 28px bar is reasonable.  Use 8px as the absolute floor. */
    uint32_t min_h = (flags & GUI_WIN_FLAG_NO_DECORATION) ? 8u : WM_MIN_HEIGHT;
    uint32_t min_w = (flags & GUI_WIN_FLAG_NO_DECORATION) ? 8u : WM_MIN_WIDTH;
    if (w < min_w || w > WM_MAX_WIDTH)   return -EINVAL;
    if (h < min_h || h > WM_MAX_HEIGHT)  return -EINVAL;

    int32_t id = -1;
    for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_wins[i].in_use) { id = i; break; }
    }
    if (id < 0) return -ENOSPC;

    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *buf = (uint8_t *)kmalloc(bytes);
    if (!buf) return -ENOMEM;
    /* Default content is dark gray so a freshly-created window is
     * visible even before the app paints. */
    for (size_t k = 0; k + 3 < bytes; k += 4) {
        buf[k+0] = 0x20; buf[k+1] = 0x20; buf[k+2] = 0x20; buf[k+3] = 0;
    }

    struct wm_window *win = &g_wins[id];
    win->in_use     = 1;
    win->id         = id;
    win->owner_pid  = pid;
    /* Pre-increment so newly-created windows always sit ABOVE every
     * existing one, *and* so we never collide with a focus-raise
     * elsewhere in this file (which also uses ++g_next_z).  If the
     * two operations used different increment styles a click-then-
     * spawn sequence could leave two windows sharing a z value,
     * which the painter's algorithm in compose_all silently mis-
     * handles (skips the duplicate). */
    win->z          = ++g_next_z;
    win->flags      = flags;
    win->minimized  = 0;        /* milestone 51: created visible */
    win->w          = w;
    win->h          = h;
    win->pixels     = buf;
    win->events.head = win->events.tail = 0;
    /* Chapter 108a \u2014 no user-visible mapping yet; lazily
     * populated by the first wm_map_window call. */
    win->user_pages_n  = 0;
    win->user_pages_pa = NULL;
    win->user_va       = 0;
    win->user_as       = NULL;

    if (want_x >= 0 && want_y >= 0) {
        win->x = want_x;
        win->y = want_y;
    } else {
        /* Stagger windows so multiple apps don't pile up at (0,0).
         * Use a dedicated cascade counter (NOT the array slot id)
         * so windows with explicit positions don't shift the
         * cascade for subsequent auto-positioned windows. */
        int32_t step = (g_next_cascade % 8) * 32;
        win->x = 80 + step;
        win->y = 60 + step;
        g_next_cascade++;
    }

    /* Copy title from user space; tolerate a NULL pointer. */
    win->title[0] = '\0';
    if (title_user) {
        long got = copy_string_from_user(win->title, (uint64_t)(uintptr_t)title_user,
                                         WM_TITLE_MAX);
        if (got < 0) {
            /* Don't fail the create on a bad title — just blank it. */
            win->title[0] = '\0';
        }
        win->title[WM_TITLE_MAX] = '\0';
    }

    /* Pinned windows never auto-take focus on creation; they exist
     * to be ambient.  This avoids a freshly-spawned taskbar
     * stealing keyboard input from whatever app the user just
     * launched.  Same logic for pin-to-bottom (the wallpaper). */
    if (!(flags & (GUI_WIN_FLAG_ALWAYS_ON_TOP |
                   GUI_WIN_FLAG_PIN_TO_BOTTOM))) {
        set_focus(id);
    }
    compose_all();
    serial_puts("[wm] window created id=");
    serial_puthex((uint64_t)id);
    serial_puts(" pid=");
    serial_puthex(pid);
    serial_puts(" size=");
    serial_puthex(w);
    serial_puts("x");
    serial_puthex(h);
    serial_puts(" flags=");
    serial_puthex(flags);
    serial_puts("\n");
    return id;
}

long wm_create_window(uint64_t pid, uint32_t w, uint32_t h,
                      const char *title_user)
{
    return wm_create_window_ex(pid, w, h, title_user, 0,
                               GUI_WIN_POS_AUTO, GUI_WIN_POS_AUTO);
}

long wm_destroy_window(uint64_t pid, int32_t id)
{
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -ENOENT;
    /* Chapter 108a \u2014 tear the user-visible mapping down before
     * we forget about the window.  The AS is still live (we're
     * called from sys_gui_destroy_window, EL0 caller is the
     * owner thread).  Failure here is logged and ignored \u2014
     * worst case the kheap allocation linked to user_pages_pa
     * leaks for the lifetime of the kernel, which is bounded. */
    if (w->user_pages_n != 0 && w->user_as) {
        (void)address_space_uninstall_wm_window(w->user_as,
                                                w->user_va,
                                                w->user_pages_n);
    }
    wm_drop_user_pages(w);
    kfree(w->pixels);
    w->pixels = NULL;
    w->in_use = 0;
    /* The window we're destroying is gone, so synthesising releases
     * to it would just push into a dead ring.  Clear the held-keys
     * table directly so the next focused window starts clean. */
    if (g_focus_id == id) {
        for (uint32_t k = 0; k < (uint32_t)(sizeof g_keys_held); k++)
            g_keys_held[k] = 0;
        g_focus_id = topmost_id();
    }
    if (g_drag_id   == id) g_drag_id   = -1;
    if (g_resize_id == id) g_resize_id = -1;
    compose_all();
    return 0;
}

void wm_destroy_owner(uint64_t pid)
{
    int any = 0;
    for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_wins[i].in_use && g_wins[i].owner_pid == pid) {
            /* Chapter 108a \u2014 uninstall the user-visible mapping
             * first.  Owner-exit teardown runs from thread_exit
             * BEFORE address_space_destroy, so the owner's AS
             * is still live and the uninstall succeeds.  If for
             * any reason it doesn't (AS already gone, descriptor
             * mismatch), we still drop our half via
             * wm_drop_user_pages \u2014 the AS teardown path skips
             * DESC_SW_WM_WINDOW entries so this won't double-
             * free. */
            if (g_wins[i].user_pages_n != 0 && g_wins[i].user_as) {
                (void)address_space_uninstall_wm_window(
                        g_wins[i].user_as,
                        g_wins[i].user_va,
                        g_wins[i].user_pages_n);
            }
            wm_drop_user_pages(&g_wins[i]);
            kfree(g_wins[i].pixels);
            g_wins[i].pixels = NULL;
            g_wins[i].in_use = 0;
            if (g_drag_id   == i) g_drag_id   = -1;
            if (g_resize_id == i) g_resize_id = -1;
            any = 1;
        }
    }
    if (!any) return;
    if (g_focus_id >= 0 && !win_by_id(g_focus_id)) {
        for (uint32_t k = 0; k < (uint32_t)(sizeof g_keys_held); k++)
            g_keys_held[k] = 0;
        g_focus_id = topmost_id();
    }
    compose_all();
}

long wm_present(uint64_t pid, int32_t id,
                uint32_t x, uint32_t y, uint32_t rw, uint32_t rh,
                const uint8_t *src_user)
{
    /* Chapter 108d: kernel compositor retired.  Legacy apps that
     * still call SYS_WIN_PRESENT get a successful no-op so they
     * don't crash; they will draw nothing on screen until they
     * port to wmclient + the userspace wsd compose path.
     *
     * milestone-15 invariant preserved: once the app has mapped
     * the FB into its own AS (user_pages_n > 0), the kernel
     * refuses to keep writing through its copy -- prevents
     * tearing with the user's direct writes.  mixtest.c verifies
     * this. */
    (void)x; (void)y; (void)rw; (void)rh; (void)src_user;
    struct wm_window *w = win_by_id(id);
    if (w && w->in_use && w->owner_pid == pid && w->user_pages_n != 0)
        return -EBUSY;
    return 0;
}

long wm_fill_rect(uint64_t pid, int32_t id,
                  uint32_t x, uint32_t y,
                  uint32_t rw, uint32_t rh,
                  uint32_t bgra)
{
    /* Chapter 108d: see wm_present above.  No-op success unless
     * the window has been mapped to userspace -- then -EBUSY. */
    (void)x; (void)y; (void)rw; (void)rh; (void)bgra;
    struct wm_window *w = win_by_id(id);
    if (w && w->in_use && w->owner_pid == pid && w->user_pages_n != 0)
        return -EBUSY;
    return 0;
}

/* Chapter 108b -- look up a single codepoint, preferring the
 * userspace font server.  Returns 0 and fills *out_gi on
 * success; also returns the cell height and baseline offset
 * appropriate to whichever source was used.  Always succeeds
 * for valid inputs (falls back to the kernel bitmap font when
 * fontd isn't reachable). */
static int wm_text_glyph(uint32_t cp,
                         struct glyph_info *out_gi,
                         uint32_t *out_cell_h,
                         uint32_t *out_baseline,
                         uint32_t *out_advance_fallback)
{
    if (wm_font_get_glyph(cp, out_gi) == 0) {
        *out_cell_h           = wm_font_cell_height();
        *out_baseline         = wm_font_baseline_offset();
        *out_advance_fallback = (out_gi->advance ? out_gi->advance
                                                 : wm_font_cell_height() / 2);
        return 0;
    }
    /* Fontd unreachable (boot window / respawning) or codepoint
     * outside the TTF cache range -- fall back to the kernel's
     * always-available bitmap font. */
    const struct bitmap_font *fb_font = font_get_bitmap();
    if (!fb_font) return -1;
    if (font_get_glyph(fb_font, cp, out_gi) != 0) return -1;
    *out_cell_h           = fb_font->cell_height;
    /* The bitmap font has no "real" baseline -- the cell IS the
     * glyph -- so we pretend the baseline is the bottom of the
     * cell, the same convention text.c uses. */
    *out_baseline         = fb_font->cell_height;
    *out_advance_fallback = fb_font->cell_width;
    return 0;
}

/* Chapter 102 -- pixel-accurate text measurement.
 *
 * Chapter 108b: the source of glyph metrics is fontd (via
 * wm_font_get_glyph) when the daemon is up, the bitmap font
 * otherwise.  Either way the per-glyph advance is summed
 * exactly the way wm_draw_text will lay them out.  Stops at
 * '\n'.  Returns the pixel width as a non-negative long, or
 * -EFAULT if `s_user` isn't readable. */
long wm_measure_text(const char *s_user)
{
    char buf[256];
    long got = copy_string_from_user(buf, (uint64_t)(uintptr_t)s_user,
                                     sizeof(buf));
    if (got < 0) return -EFAULT;

    uint32_t w = 0;
    for (size_t i = 0; buf[i]; i++) {
        char ch = buf[i];
        if (ch == '\n') break;
        struct glyph_info gi;
        uint32_t cell_h = 0, baseline = 0, fallback_adv = 0;
        if (wm_text_glyph((uint32_t)(uint8_t)ch, &gi,
                          &cell_h, &baseline, &fallback_adv) != 0) continue;
        uint32_t adv = gi.advance ? gi.advance : fallback_adv;
        w += adv;
    }
    return (long)w;
}

long wm_draw_text(uint64_t pid, int32_t id,
                  uint32_t x, uint32_t y,
                  const char *s_user,
                  uint32_t fg_bgra, uint32_t bg_bgra,
                  int transparent)
{
    /* Chapter 108d: see wm_present above.  No-op success unless
     * the window has been mapped to userspace -- then -EBUSY.
     * We deliberately don't read s_user -- legacy callers pass
     * pointers from their address space we don't need to touch. */
    (void)x; (void)y; (void)s_user;
    (void)fg_bgra; (void)bg_bgra; (void)transparent;
    struct wm_window *w = win_by_id(id);
    if (w && w->in_use && w->owner_pid == pid && w->user_pages_n != 0)
        return -EBUSY;
    return 0;
}

long wm_flush(uint64_t pid, int32_t id)
{
    /* Chapter 108d: kernel compositor retired; wsd flushes its own
     * scanout via SYS_FB_PRESENT.  Legacy SYS_WIN_FLUSH callers
     * get a success return so they don't error out. */
    (void)pid; (void)id;
    return 0;
}

long wm_poll_event(uint64_t pid, struct gui_event *out_user)
{
    /* Find any window owned by this pid that has events queued.
     * Simple linear search is fine. */
    for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_wins[i].in_use) continue;
        if (g_wins[i].owner_pid != pid) continue;
        struct gui_event ev;
        if (ring_pop(&g_wins[i].events, &ev)) {
            if (copy_to_user((uint64_t)(uintptr_t)out_user,
                             &ev, sizeof(ev)) < 0)
                return -EFAULT;
            return 1;
        }
    }
    return 0;
}

long wm_list_windows(uint64_t pid, struct gui_window_info *out_user,
                     int32_t max)
{
    (void)pid;  /* Any process may enumerate the WM's window list. */
    if (max <= 0) return 0;
    if (max > WM_MAX_WINDOWS) max = WM_MAX_WINDOWS;

    /* Walk windows in ascending z so the first entry is the
     * back-most window and the last is the front-most.  Convenient
     * for taskbars that want a stable display order. */
    int32_t emitted = 0;
    uint32_t painted = 0;
    for (int32_t pass = 0; pass < WM_MAX_WINDOWS && emitted < max; pass++) {
        int32_t  pick = -1;
        uint32_t pick_z = 0;
        for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!g_wins[i].in_use) continue;
            if (painted & (1u << i)) continue;
            if (pick < 0 || g_wins[i].z < pick_z) {
                pick = i;
                pick_z = g_wins[i].z;
            }
        }
        if (pick < 0) break;
        painted |= (1u << pick);

        const struct wm_window *w = &g_wins[pick];
        struct gui_window_info info;
        info.id        = w->id;
        info.flags     = w->flags | (w->minimized ? GUI_WIN_FLAG_MINIMIZED : 0u);
        info.x         = w->x;
        info.y         = w->y;
        info.w         = w->w;
        info.h         = w->h;
        info.z         = w->z;
        info.focused   = (w->id == g_focus_id) ? 1 : 0;
        info.owner_pid = w->owner_pid;
        /* Copy title with explicit length so we never read past
         * the source's WM_TITLE_MAX+1 bytes. */
        size_t tlen = 0;
        while (tlen < sizeof(info.title) - 1 && w->title[tlen]) {
            info.title[tlen] = w->title[tlen];
            tlen++;
        }
        info.title[tlen] = '\0';

        uint64_t dst = (uint64_t)(uintptr_t)(out_user + emitted);
        if (copy_to_user(dst, &info, sizeof(info)) < 0)
            return -EFAULT;
        emitted++;
    }
    return emitted;
}

long wm_raise_window(uint64_t pid, int32_t id)
{
    (void)pid;  /* Any process may raise any window for now.  Future:
                 * restrict to owner_pid + a "trusted shell" pid. */
    if (id < 0 || id >= WM_MAX_WINDOWS) return -EINVAL;
    struct wm_window *w = &g_wins[id];
    if (!w->in_use) return -ENOENT;
    /* Raising a minimized window implicitly restores it: the user's
     * intent ("bring this to the front") makes no sense if the
     * window stays hidden.  This also matches the taskbar's
     * "click to restore" UX in milestone 51. */
    if (w->minimized) w->minimized = 0;
    /* Pinned windows always paint last regardless of z, so a raise
     * is a no-op visually — but still legal so callers don't have
     * to special-case. */
    if (!(w->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP)) {
        w->z = ++g_next_z;
    }
    set_focus(w->id);
    compose_all();
    return 0;
}

/* Milestone 51 — hide / show a window without destroying it. */
long wm_set_minimized(uint64_t pid, int32_t id, int on)
{
    (void)pid;  /* Any process may toggle for now (taskbar lives in
                 * a separate process from most app windows). */
    if (id < 0 || id >= WM_MAX_WINDOWS) return -EINVAL;
    struct wm_window *w = &g_wins[id];
    if (!w->in_use) return -ENOENT;
    /* Pin-to-bottom windows (the wallpaper) refuse to minimize —
     * they're click-transparent and have no decorations, so the
     * UX makes no sense and the desktop becoming hidden would
     * leave a black wallpaper. */
    if (w->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) return -EINVAL;

    if (on) {
        if (w->minimized) return 0;     /* idempotent */
        w->minimized = 1;
        /* If we just hid the focused window, hand focus to the
         * topmost remaining non-minimized, non-pin window so
         * keyboard input doesn't go into the void. */
        if (g_focus_id == w->id) {
            int32_t  best = -1;
            uint32_t best_z = 0;
            for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
                struct wm_window *o = &g_wins[i];
                if (!o->in_use) continue;
                if (o->minimized) continue;
                if (o->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
                if (best < 0 || o->z > best_z) {
                    best = i;
                    best_z = o->z;
                }
            }
            set_focus((best >= 0) ? g_wins[best].id : -1);
        }
    } else {
        if (!w->minimized) return 0;    /* idempotent */
        w->minimized = 0;
        /* Restore implies raise + focus: matches gui_raise_window's
         * behaviour and what users expect when they click the
         * taskbar entry of a hidden window. */
        if (!(w->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP)) {
            w->z = ++g_next_z;
        }
        set_focus(w->id);
    }
    compose_all();
    return 0;
}

/* ---------- Chapter 108a: userspace window-buffer mapping ---------- */

/* Free the user-visible page set for `w` WITHOUT touching the
 * AS layer.  Used by destroy paths where the AS uninstall has
 * either already happened or is about to via address_space
 * teardown's DESC_SW_WM_WINDOW skip path. */
static void wm_drop_user_pages(struct wm_window *w)
{
    if (!w || !w->user_pages_pa) return;
    for (uint32_t i = 0; i < w->user_pages_n; i++) {
        if (w->user_pages_pa[i])
            pmem_free_page(w->user_pages_pa[i]);
    }
    kfree(w->user_pages_pa);
    w->user_pages_pa = NULL;
    w->user_pages_n  = 0;
    w->user_va       = 0;
    w->user_as       = NULL;
}

long wm_map_window(uint64_t pid, int32_t id,
                   uint64_t *va_out, uint32_t *stride_out,
                   uint32_t *w_out, uint32_t *h_out)
{
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    /* Chapter 108a defers resize coherence to 108b.  RESIZABLE
     * windows can't be mapped today because the resize-grip
     * drag path realloc()s w->pixels but has no story for the
     * user-visible mapping.  Apps that opt in to gui_window_fb
     * just have to leave RESIZABLE clear; the only existing
     * apps with RESIZABLE set (notepad) don't use the mapping
     * path so this restriction is invisible to them. */
    if (w->flags & GUI_WIN_FLAG_RESIZABLE) return -EINVAL;

    struct thread *t = thread_current();
    if (!t || !t->as) return -EFAULT;

    /* Compute payload size in bytes, then in 4 KiB pages
     * rounded up.  The window's tail-of-last-page leftover is
     * zero-filled and never touched by either side. */
    size_t bytes  = (size_t)w->w * (size_t)w->h * 4u;
    uint32_t n    = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t stride = w->w * 4u;
    uint32_t ww   = w->w;
    uint32_t hh   = w->h;

    /* Idempotency: if the window is already mapped (same owner),
     * return the cached descriptors.  This lets apps that
     * call gui_window_fb() in a tight loop (e.g. paint after
     * every event) avoid quadratic re-mapping cost. */
    if (w->user_pages_n != 0) {
        if (w->user_as != t->as) return -EPERM;
        if (va_out     && copy_to_user((uint64_t)(uintptr_t)va_out,
                                       &w->user_va, sizeof(w->user_va)) < 0)
            return -EFAULT;
        if (stride_out && copy_to_user((uint64_t)(uintptr_t)stride_out,
                                       &stride, sizeof(stride)) < 0)
            return -EFAULT;
        if (w_out      && copy_to_user((uint64_t)(uintptr_t)w_out,
                                       &ww, sizeof(ww)) < 0)
            return -EFAULT;
        if (h_out      && copy_to_user((uint64_t)(uintptr_t)h_out,
                                       &hh, sizeof(hh)) < 0)
            return -EFAULT;
        return 0;
    }

    /* Phase 1: pull `n` page frames out of pmem.  These don't
     * have to be physically contiguous \u2014 the user sees a
     * contiguous VA range regardless. */
    uint64_t *pages = (uint64_t *)kmalloc((size_t)n * sizeof(uint64_t));
    if (!pages) return -ENOMEM;
    for (uint32_t i = 0; i < n; i++) pages[i] = 0;
    for (uint32_t i = 0; i < n; i++) {
        pages[i] = pmem_alloc_page();
        if (!pages[i]) {
            for (uint32_t k = 0; k < i; k++) pmem_free_page(pages[k]);
            kfree(pages);
            return -ENOMEM;
        }
    }

    /* Phase 2: seed the user-visible pages with the current
     * compositor-side bytes so the app sees what's on screen
     * right now (typically the dark-gray default fill, but if
     * the app painted via fill_rect/draw_text before calling
     * gui_window_fb the seed reflects that).  Each frame holds
     * up to PAGE_SIZE bytes from `w->pixels`; the last frame
     * gets clamped to the leftover. */
    size_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *frame = (uint8_t *)(uintptr_t)pages[i];
        size_t   chunk = (bytes - pos < PAGE_SIZE) ? (bytes - pos) : PAGE_SIZE;
        if (chunk > 0) {
            const uint8_t *src = w->pixels + pos;
            for (size_t k = 0; k < chunk; k++) frame[k] = src[k];
        }
        /* Pages beyond the payload (only happens for the last
         * frame) were zero-filled by pmem_alloc_page; nothing
         * to do. */
        pos += chunk;
    }

    /* Phase 3: install the run into the caller's AS at a fresh
     * VA range.  On failure, give back the frames and report
     * the AS layer's verdict. */
    uint64_t va = 0;
    if (address_space_install_wm_window(t->as, pages, n, &va) != 0) {
        for (uint32_t i = 0; i < n; i++) pmem_free_page(pages[i]);
        kfree(pages);
        return -ENOMEM;
    }

    /* Phase 4: remember the mapping on the window struct so
     * destroy / unmap can find it. */
    w->user_pages_pa = pages;
    w->user_pages_n  = n;
    w->user_va       = va;
    w->user_as       = t->as;

    /* Phase 5: copy descriptors out to userspace.  If any of
     * the user pointers is bad we still have a valid mapping;
     * the caller can recover by calling wm_unmap_window. */
    if (va_out     && copy_to_user((uint64_t)(uintptr_t)va_out,
                                   &w->user_va, sizeof(w->user_va)) < 0)
        return -EFAULT;
    if (stride_out && copy_to_user((uint64_t)(uintptr_t)stride_out,
                                   &stride, sizeof(stride)) < 0)
        return -EFAULT;
    if (w_out      && copy_to_user((uint64_t)(uintptr_t)w_out,
                                   &ww, sizeof(ww)) < 0)
        return -EFAULT;
    if (h_out      && copy_to_user((uint64_t)(uintptr_t)h_out,
                                   &hh, sizeof(hh)) < 0)
        return -EFAULT;

    serial_puts("[wm] map_window id=");
    serial_puthex((uint64_t)id);
    serial_puts(" pid=");
    serial_puthex(pid);
    serial_puts(" pages=");
    serial_puthex((uint64_t)n);
    serial_puts(" va=");
    serial_puthex(va);
    serial_puts("\n");
    return 0;
}

long wm_unmap_window(uint64_t pid, int32_t id)
{
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    if (w->user_pages_n == 0) return 0;     /* idempotent */
    if (!w->user_as) {
        /* Defensive: caller raced an AS teardown.  Just drop
         * our half. */
        wm_drop_user_pages(w);
        return 0;
    }

    /* Uninstall the AS descriptors first \u2014 once those are
     * gone, no user code can race a free against a still-live
     * mapping. */
    if (address_space_uninstall_wm_window(w->user_as,
                                          w->user_va,
                                          w->user_pages_n) != 0) {
        /* AS layer disagrees about our mapping; refuse to
         * proceed so we don't free pages someone else might
         * believe they still own. */
        return -EINVAL;
    }
    wm_drop_user_pages(w);
    return 0;
}

long wm_damage(uint64_t pid, int32_t id,
               uint32_t x, uint32_t y, uint32_t rw, uint32_t rh)
{
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    if (w->user_pages_n == 0) return -ENOENT;
    if (rw == 0 || rh == 0) return -EINVAL;

    /* Clip the damage rect to the window.  Apps that pass an
     * over-large rect (e.g. "damage everything") get a quiet
     * clip rather than an error so the typical
     * gui_window_damage_full() helper Just Works. */
    if (x >= w->w || y >= w->h) return 0;
    if (x + rw > w->w) rw = w->w - x;
    if (y + rh > w->h) rh = w->h - y;

    /* Copy row-by-row from the user-visible pages into the
     * compositor's authoritative buffer.  We walk the rect in
     * row order; for each row, the byte offset into the flat
     * payload is `(y+row) * stride + x*4` and the length is
     * `rw*4`.  We then translate offsets to (page index, byte
     * offset within page) since the user-visible storage is a
     * non-contiguous run of 4 KiB frames.  No copy_from_user
     * here \u2014 the WM-owned pages live in the kernel identity
     * map, so accessing them via PA directly is safe. */
    uint32_t stride = w->w * 4u;
    for (uint32_t row = 0; row < rh; row++) {
        size_t off = (size_t)(y + row) * stride + (size_t)x * 4u;
        uint8_t *dst = w->pixels + off;
        size_t remaining = (size_t)rw * 4u;
        while (remaining) {
            uint32_t page_idx = (uint32_t)(off / PAGE_SIZE);
            uint32_t in_page  = (uint32_t)(off % PAGE_SIZE);
            uint32_t chunk    = (uint32_t)((PAGE_SIZE - in_page < remaining)
                                         ? (PAGE_SIZE - in_page)
                                         : remaining);
            if (page_idx >= w->user_pages_n) break;     /* defensive */
            const uint8_t *src = (const uint8_t *)(uintptr_t)
                                  w->user_pages_pa[page_idx] + in_page;
            for (uint32_t k = 0; k < chunk; k++) dst[k] = src[k];
            dst       += chunk;
            off       += chunk;
            remaining -= chunk;
        }
    }

    compose_all();
    return 0;
}

/* ---------------------------------------------------------------
 * chapter 108e -- userspace decorations + cursor (wsd takes over)
 *
 * The three helpers below are the kernel API surface that wsd
 * leans on once it owns title-bar paint, drag, close-button paint,
 * and cursor-sprite paint.  Each one is intentionally tiny and
 * idempotent: this is the kernel's input/window contract with the
 * userspace compositor, nothing more.
 * --------------------------------------------------------------- */

/* Return the current pointer state (x, y in scanout coords;
 * button bitmap).  Any of the output pointers may be NULL --
 * wsd's poller only needs (x, y, buttons) every frame so it
 * passes all three, but a future debug tool that just wants
 * buttons can pass two NULLs. */
long wm_pointer_state(int32_t *out_x_user, int32_t *out_y_user,
                      uint32_t *out_btn_user)
{
    int32_t  x = g_pointer_x;
    int32_t  y = g_pointer_y;
    uint32_t b = g_buttons;
    if (out_x_user) {
        if (copy_to_user((uint64_t)(uintptr_t)out_x_user,
                         &x, sizeof(x)) < 0) return -EFAULT;
    }
    if (out_y_user) {
        if (copy_to_user((uint64_t)(uintptr_t)out_y_user,
                         &y, sizeof(y)) < 0) return -EFAULT;
    }
    if (out_btn_user) {
        if (copy_to_user((uint64_t)(uintptr_t)out_btn_user,
                         &b, sizeof(b)) < 0) return -EFAULT;
    }
    return 0;
}

/* Move a kernel-WM window (typically a wsd "input shadow") to a
 * new scanout position.  No clipping, no event delivery, no
 * recompose -- the kernel WM doesn't paint any more in
 * chapter 108d; the only reason this exists is so that hit-testing for
 * body clicks lines up after wsd drags the window.  Caller
 * (wsd) is responsible for sending any GUI_EVENT_MOVE-equivalent
 * to the app via wm_deliver_event if it cares. */
long wm_move_window(int32_t id, int32_t x, int32_t y)
{
    if (id < 0 || id >= WM_MAX_WINDOWS) return -EINVAL;
    struct wm_window *w = &g_wins[id];
    if (!w->in_use) return -ENOENT;
    w->x = x;
    w->y = y;
    return 0;
}

/* Push a synthesised gui_event into the per-window event ring
 * so the owning app's next wm_poll_event returns it.  The
 * window_id field in the user-supplied event is overwritten
 * with `id` so the app can trust that field unconditionally;
 * everything else is copied verbatim.
 *
 * Used by wsd to deliver GUI_EVENT_CLOSE when the user clicks
 * the close button; the same syscall will be used later for
 * synthesised pointer events that hit the title bar but are
 * meant to reach the app (e.g. a context-menu click on the
 * title that the app wants to handle). */
long wm_deliver_event(int32_t id, const struct gui_event *ev_user)
{
    if (id < 0 || id >= WM_MAX_WINDOWS) return -EINVAL;
    struct wm_window *w = &g_wins[id];
    if (!w->in_use) return -ENOENT;
    struct gui_event ev;
    if (copy_from_user(&ev, (uint64_t)(uintptr_t)ev_user,
                       sizeof(ev)) < 0) return -EFAULT;
    ev.window_id = w->id;
    if (!ring_push(&w->events, &ev)) return -ENOSPC;
    return 0;
}

/* chapter 108e -- toggle the wsd-routed pointer-passthrough flag
 * on one shadow.  Idempotent.  See struct wm_window's
 * input_passthrough comment for the routing semantics. */
long wm_set_input_passthrough(int32_t id, int on)
{
    if (id < 0 || id >= WM_MAX_WINDOWS) return -EINVAL;
    struct wm_window *w = &g_wins[id];
    if (!w->in_use) return -ENOENT;
    w->input_passthrough = on ? 1 : 0;
    return 0;
}

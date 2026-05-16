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
static int              g_wm_painted_wallpaper = 0;

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
static int deliver_key(struct wm_window *w, uint32_t key)
{
    struct gui_event ev = (struct gui_event){
        .type = GUI_EVENT_KEY,
        .window_id = w->id,
        .arg0 = key,
        .arg1 = 0, .arg2 = 0, .arg3 = 0,
    };
    ring_push(&w->events, &ev);
    return 1;
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

/* ---- painting ---- */
static const struct fb_color WALLPAPER     = FB_COLOR(0x10, 0x14, 0x28);
static const struct fb_color WALLPAPER_TOP = FB_COLOR(0x18, 0x20, 0x40);
static const struct fb_color DECO_BG   = FB_COLOR(0x20, 0x30, 0x60);
static const struct fb_color DECO_BG_F = FB_COLOR(0x40, 0x60, 0xC0);
static const struct fb_color DECO_FG   = FB_COLOR(0xFF, 0xFF, 0xFF);
static const struct fb_color BORDER    = FB_COLOR(0x60, 0x80, 0xC0);

static void paint_wallpaper(void)
{
    if (!fb_is_ready()) return;
    const struct fb_info *fb = fb_get_info();

    /* Vertical gradient: slightly lighter near the top fading to
     * the existing dark navy at the bottom.  Done in 16-row bands
     * so we don't pay per-pixel cost in the kernel.
     *
     * This is intentionally a kernel-level FALLBACK only.  As
     * soon as init spawns /bin/desktop, the desktop process
     * creates a PIN_TO_BOTTOM window covering the screen and
     * blits the real wallpaper image into it.  Wallpaper
     * ownership belongs in userspace, not in the kernel. */
    const uint32_t bands = 16;
    const uint32_t band_h = fb->height / bands;
    for (uint32_t i = 0; i < bands; i++) {
        /* Linear blend WALLPAPER_TOP -> WALLPAPER as i: 0 -> bands-1. */
        uint32_t num = i, den = bands - 1;
        uint8_t r = (uint8_t)((WALLPAPER_TOP.r * (den - num) + WALLPAPER.r * num) / den);
        uint8_t g = (uint8_t)((WALLPAPER_TOP.g * (den - num) + WALLPAPER.g * num) / den);
        uint8_t b = (uint8_t)((WALLPAPER_TOP.b * (den - num) + WALLPAPER.b * num) / den);
        struct fb_color c = FB_COLOR(r, g, b);
        uint32_t y = i * band_h;
        uint32_t h = (i == bands - 1) ? (fb->height - y) : band_h;
        fb_fill_rect(0, y, fb->width, h, c);
    }

    /* Subtle horizontal bar at the bottom — placeholder for a
     * future taskbar so the wallpaper doesn't look totally empty. */
    fb_fill_rect(0, fb->height - 28, fb->width, 28, FB_COLOR(0x18, 0x1C, 0x32));
    fb_fill_rect(0, fb->height - 28, fb->width, 1,  BORDER);
}

/* Blit a window's content + decorations into the framebuffer back
 * buffer.  Caller is responsible for fb_present()ing the affected
 * region. */
static void blit_window(const struct wm_window *w)
{
    if (!fb_is_ready()) return;
    const struct fb_info *fb = fb_get_info();
    if (!fb) return;

    /* Milestone 47: NO_DECORATION windows have no title bar, no
     * border, and no close button.  Their content rectangle starts
     * at (w->x, w->y) and is exactly w->w * w->h pixels. */
    if (w->flags & GUI_WIN_FLAG_NO_DECORATION) {
        int32_t cx = w->x;
        int32_t cy = w->y;
        for (uint32_t row = 0; row < w->h; row++) {
            int32_t fy = cy + (int32_t)row;
            if (fy < 0 || fy >= (int32_t)fb->height) continue;
            for (uint32_t col = 0; col < w->w; col++) {
                int32_t fx = cx + (int32_t)col;
                if (fx < 0 || fx >= (int32_t)fb->width) continue;
                uint32_t v = ((uint32_t *)w->pixels)[row * w->w + col];
                fb_draw_pixel((uint32_t)fx, (uint32_t)fy, bgra_unpack(v));
            }
        }
        return;
    }

    int32_t deco_x = w->x;
    int32_t deco_y = w->y;
    int32_t deco_w = (int32_t)w->w + 2 * WM_BORDER;
    int32_t deco_h = (int32_t)w->h + WM_TITLE_HEIGHT + WM_BORDER;
    if (deco_x < 0) deco_x = 0;
    if (deco_y < 0) deco_y = 0;
    if (deco_x >= (int32_t)fb->width)  return;
    if (deco_y >= (int32_t)fb->height) return;

    /* Title bar — colour reflects focus. */
    int focused = (w->id == g_focus_id);
    fb_fill_rect((uint32_t)deco_x, (uint32_t)deco_y,
                 (uint32_t)deco_w, WM_TITLE_HEIGHT,
                 focused ? DECO_BG_F : DECO_BG);
    /* Title text. */
    const struct bitmap_font *font = font_get_default();
    text_draw_string(font,
                     (uint32_t)deco_x + 8, (uint32_t)deco_y + 4,
                     fb->width, fb->height,
                     w->title,
                     DECO_FG, DECO_BG, 1, NULL, NULL);

    /* Close button — a small red square with a white X on the
     * right side of the title bar. */
    if (deco_w >= WM_CLOSE_BTN_W + 4) {
        uint32_t bx = (uint32_t)deco_x + (uint32_t)deco_w - WM_CLOSE_BTN_W - 2;
        uint32_t by = (uint32_t)deco_y + 2;
        uint32_t bw = WM_CLOSE_BTN_W;
        uint32_t bh = WM_TITLE_HEIGHT - 4;
        fb_fill_rect(bx, by, bw, bh, FB_COLOR(0xC0, 0x30, 0x30));
        /* Draw an X.  Hand-rolled so we don't depend on a font glyph
         * for this 20x20 button. */
        for (uint32_t i = 4; i + 4 < bw && i + 4 < bh; i++) {
            uint32_t lx = bx + i;
            uint32_t rx = bx + bw - 1 - i;
            uint32_t y  = by + i;
            if (lx < fb->width && y < fb->height)
                fb_draw_pixel(lx, y, FB_COLOR(0xFF, 0xFF, 0xFF));
            if (rx < fb->width && y < fb->height)
                fb_draw_pixel(rx, y, FB_COLOR(0xFF, 0xFF, 0xFF));
        }
    }

    /* Minimize button — grey square with a white underscore,
     * placed immediately to the left of the close button.  Only
     * drawn if the title bar is wide enough to hold both buttons
     * AND a few pixels of breathing room for the title text. */
    if (deco_w >= WM_CLOSE_BTN_W + WM_MIN_BTN_W + WM_BTN_GAP + 8) {
        uint32_t bx = (uint32_t)deco_x + (uint32_t)deco_w
                    - WM_CLOSE_BTN_W - 2 - WM_BTN_GAP - WM_MIN_BTN_W;
        uint32_t by = (uint32_t)deco_y + 2;
        uint32_t bw = WM_MIN_BTN_W;
        uint32_t bh = WM_TITLE_HEIGHT - 4;
        fb_fill_rect(bx, by, bw, bh, FB_COLOR(0x60, 0x60, 0x60));
        /* Underscore: a 2-pixel-tall horizontal bar near the bottom
         * of the button, leaving 4 px of margin on each side. */
        if (bw > 8 && bh > 6) {
            uint32_t ly0 = by + bh - 4;
            for (uint32_t i = 4; i + 4 < bw; i++) {
                uint32_t px = bx + i;
                if (px < fb->width) {
                    if (ly0     < fb->height)
                        fb_draw_pixel(px, ly0,     FB_COLOR(0xFF, 0xFF, 0xFF));
                    if (ly0 + 1 < fb->height)
                        fb_draw_pixel(px, ly0 + 1, FB_COLOR(0xFF, 0xFF, 0xFF));
                }
            }
        }
    }

    /* Border around the whole window. */
    fb_draw_rect((uint32_t)deco_x, (uint32_t)deco_y,
                 (uint32_t)deco_w, (uint32_t)deco_h, BORDER);

    /* Content area: copy the window's BGRA pixel buffer into the
     * framebuffer, row by row, clipping to screen bounds. */
    int32_t cx = deco_x + WM_BORDER;
    int32_t cy = deco_y + WM_TITLE_HEIGHT;
    uint32_t cw = w->w;
    uint32_t ch = w->h;
    for (uint32_t row = 0; row < ch; row++) {
        int32_t fy = cy + (int32_t)row;
        if (fy < 0 || fy >= (int32_t)fb->height) continue;
        for (uint32_t col = 0; col < cw; col++) {
            int32_t fx = cx + (int32_t)col;
            if (fx < 0 || fx >= (int32_t)fb->width) continue;
            uint32_t v = ((uint32_t *)w->pixels)[row * cw + col];
            fb_draw_pixel((uint32_t)fx, (uint32_t)fy, bgra_unpack(v));
        }
    }

    /* Resize grip — painted last so it sits visually on top of the
     * content's bottom-right corner.  Three diagonal stripes of
     * white pixels across a WM_GRIP_SIZE square anchored to the
     * window's bottom-right interior corner.  Visual cue is small
     * but unmistakable; matches the macOS / classic-X11 idiom. */
    if ((w->flags & GUI_WIN_FLAG_RESIZABLE) &&
        deco_w >= WM_GRIP_SIZE + 2 &&
        deco_h >= WM_GRIP_SIZE + WM_TITLE_HEIGHT + 2) {
        int32_t gx0 = deco_x + deco_w - WM_GRIP_SIZE - WM_BORDER;
        int32_t gy0 = deco_y + deco_h - WM_GRIP_SIZE - WM_BORDER;
        for (int32_t i = 0; i < WM_GRIP_SIZE; i++) {
            for (int32_t j = 0; j < WM_GRIP_SIZE; j++) {
                /* Three diagonal lines: i+j == GRIP-2, GRIP-6, GRIP-10 */
                int diag = i + j;
                int draw = (diag == WM_GRIP_SIZE - 2 ||
                            diag == WM_GRIP_SIZE - 6 ||
                            diag == WM_GRIP_SIZE - 10);
                if (!draw) continue;
                int32_t px = gx0 + j;
                int32_t py = gy0 + i;
                if (px < 0 || px >= (int32_t)fb->width)  continue;
                if (py < 0 || py >= (int32_t)fb->height) continue;
                fb_draw_pixel((uint32_t)px, (uint32_t)py,
                              FB_COLOR(0xFF, 0xFF, 0xFF));
            }
        }
    }
}

/* Tiny 12-row monochrome cursor sprite ('1' = white, '2' = black,
 * '.' = transparent).  Inspired by the classic X11 left_ptr. */
#define CURSOR_W 12
#define CURSOR_H 19
static const char *const CURSOR_BITMAP[CURSOR_H] = {
    "2...........",
    "22..........",
    "212.........",
    "2112........",
    "21112.......",
    "211112......",
    "2111112.....",
    "21111112....",
    "211111112...",
    "2111111112..",
    "21111111112.",
    "211111122222",
    "2111121.....",
    "211221......",
    "21221.......",
    "2122........",
    "1221........",
    ".22.........",
    "............",
};

static void blit_cursor(void)
{
    if (g_pointer_x < 0 || g_pointer_y < 0) return;
    const struct fb_info *fb = fb_get_info();
    if (!fb) return;
    for (int32_t row = 0; row < CURSOR_H; row++) {
        for (int32_t col = 0; col < CURSOR_W; col++) {
            char ch = CURSOR_BITMAP[row][col];
            if (ch == '.' || ch == 0) continue;
            int32_t fx = g_pointer_x + col;
            int32_t fy = g_pointer_y + row;
            if (fx < 0 || fy < 0)                     continue;
            if (fx >= (int32_t)fb->width)             continue;
            if (fy >= (int32_t)fb->height)            continue;
            struct fb_color c = (ch == '1')
                ? (struct fb_color)FB_COLOR(0xFF, 0xFF, 0xFF)
                : (struct fb_color)FB_COLOR(0x00, 0x00, 0x00);
            fb_draw_pixel((uint32_t)fx, (uint32_t)fy, c);
        }
    }
}

static void compose_all(void)
{
    if (!fb_is_ready()) return;
    /* Lazy cursor seed.  blit_cursor early-returns while
     * g_pointer_x/y are -1, which means the sprite stays invisible
     * until the FIRST EV_ABS arrives from the host.  Under HVF the
     * QEMU window doesn't generate any tablet events until the user
     * physically waves the host pointer over it — so freshly-booted
     * systems and "all windows closed" states could sit there for
     * many seconds with no cursor at all.  Drop a sane default at
     * the centre of the screen the first time we have a usable
     * framebuffer; the very next motion event overwrites it. */
    if (g_pointer_x < 0 || g_pointer_y < 0) {
        const struct fb_info *fb = fb_get_info();
        if (fb) {
            g_pointer_x = (int32_t)fb->width  / 2;
            g_pointer_y = (int32_t)fb->height / 2;
        }
    }
    paint_wallpaper();
    /* Painter's algorithm: walk every in-use window in ascending z.
     *
     * The previous implementation tied the outer pass counter to z
     * directly (`if (g_wins[i].z <= pass) continue;`).  That worked
     * only while z values stayed packed in [1..WM_MAX_WINDOWS], which
     * is NOT true after focus-raises: every left-down does
     * `w->z = ++g_next_z`, so after a few clicks z values fan out
     * arbitrarily and:
     *   (a) windows with the same z would be painted multiple times
     *       and others skipped entirely, and
     *   (b) windows whose z exceeded WM_MAX_WINDOWS would never be
     *       painted at all.
     *
     * The straightforward fix is to track which windows have been
     * emitted with a per-pass mask instead of conflating z with the
     * pass index.  N <= 16 so O(N^2) is fine. */
    uint32_t painted = 0;  /* bit i set => g_wins[i] already drawn */
    /* Pass 0: pin-to-bottom windows (the desktop wallpaper).  We
     * paint these FIRST in z order so they sit underneath every
     * other window.  hit_test ignores them entirely so clicks
     * fall through to apps as if the wallpaper weren't there. */
    for (uint32_t pass = 0; pass < WM_MAX_WINDOWS; pass++) {
        int32_t  pick_id = -1;
        uint32_t pick_z  = 0;
        for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!g_wins[i].in_use) continue;
            if (painted & (1u << i)) continue;
            if (g_wins[i].minimized) continue;
            if (!(g_wins[i].flags & GUI_WIN_FLAG_PIN_TO_BOTTOM)) continue;
            if (pick_id < 0 || g_wins[i].z < pick_z) {
                pick_id = i;
                pick_z  = g_wins[i].z;
            }
        }
        if (pick_id < 0) break;
        blit_window(&g_wins[pick_id]);
        painted |= (1u << pick_id);
    }
    /* Pass 1: every regular (non-always-on-top, non-pin-to-bottom)
     * window in z order. */
    for (uint32_t pass = 0; pass < WM_MAX_WINDOWS; pass++) {
        int32_t  pick_id = -1;
        uint32_t pick_z  = 0;
        for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!g_wins[i].in_use) continue;
            if (painted & (1u << i)) continue;
            if (g_wins[i].minimized) continue;
            if (g_wins[i].flags & GUI_WIN_FLAG_ALWAYS_ON_TOP) continue;
            if (g_wins[i].flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
            if (pick_id < 0 || g_wins[i].z < pick_z) {
                pick_id = i;
                pick_z  = g_wins[i].z;
            }
        }
        if (pick_id < 0) break;
        blit_window(&g_wins[pick_id]);
        painted |= (1u << pick_id);
    }
    /* Pass 2: always-on-top windows, also in z order so multiple
     * pinned panels stack predictably. */
    for (uint32_t pass = 0; pass < WM_MAX_WINDOWS; pass++) {
        int32_t  pick_id = -1;
        uint32_t pick_z  = 0;
        for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!g_wins[i].in_use) continue;
            if (painted & (1u << i)) continue;
            if (g_wins[i].minimized) continue;
            if (!(g_wins[i].flags & GUI_WIN_FLAG_ALWAYS_ON_TOP)) continue;
            if (pick_id < 0 || g_wins[i].z < pick_z) {
                pick_id = i;
                pick_z  = g_wins[i].z;
            }
        }
        if (pick_id < 0) break;
        blit_window(&g_wins[pick_id]);
        painted |= (1u << pick_id);
    }
    blit_cursor();
    fb_present(0, 0, 0, 0);
    g_wm_painted_wallpaper = 1;
}

/* ---- public API ---- */
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
            /* Clicked the wallpaper — defocus all. */
            g_focus_id = -1;
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
        g_focus_id = w->id;

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
        g_focus_id = id;
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
    kfree(w->pixels);
    w->pixels = NULL;
    w->in_use = 0;
    if (g_focus_id  == id) g_focus_id  = topmost_id();
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
            kfree(g_wins[i].pixels);
            g_wins[i].pixels = NULL;
            g_wins[i].in_use = 0;
            if (g_drag_id   == i) g_drag_id   = -1;
            if (g_resize_id == i) g_resize_id = -1;
            any = 1;
        }
    }
    if (!any) return;
    if (g_focus_id >= 0 && !win_by_id(g_focus_id))
        g_focus_id = topmost_id();
    compose_all();
}

long wm_present(uint64_t pid, int32_t id,
                uint32_t x, uint32_t y, uint32_t rw, uint32_t rh,
                const uint8_t *src_user)
{
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    if (rw == 0 || rh == 0) return 0;
    if (x >= w->w || y >= w->h) return -EINVAL;
    if (x + rw > w->w || y + rh > w->h) return -EINVAL;

    /* Copy user pixels row-by-row into the window buffer.  The
     * source is assumed tightly packed (rw*4 bytes per row, no
     * padding) — same convention as VibeOS. */
    for (uint32_t row = 0; row < rh; row++) {
        uint8_t *dst = w->pixels + ((y + row) * w->w + x) * 4u;
        uint64_t s   = (uint64_t)(uintptr_t)src_user + (uint64_t)row * rw * 4u;
        if (copy_from_user(dst, s, (size_t)rw * 4u) < 0)
            return -EFAULT;
    }
    return 0;
}

long wm_fill_rect(uint64_t pid, int32_t id,
                  uint32_t x, uint32_t y,
                  uint32_t rw, uint32_t rh,
                  uint32_t bgra)
{
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    if (rw == 0 || rh == 0) return 0;
    if (x >= w->w || y >= w->h) return -EINVAL;
    if (x + rw > w->w || y + rh > w->h) return -EINVAL;

    for (uint32_t row = 0; row < rh; row++) {
        uint32_t *dst = (uint32_t *)w->pixels + (y + row) * w->w + x;
        for (uint32_t col = 0; col < rw; col++)
            dst[col] = bgra;
    }
    return 0;
}

/* Per-pixel alpha-blend BGRA into a window buffer pixel.
 * Chapter 102: TTF glyphs carry per-pixel alpha (0..255). For 0
 * (transparent) we skip; for 255 (opaque) we write fg directly;
 * otherwise we blend fg over the existing pixel using the same
 * (a*src + (255-a)*dst) / 255 formula text_alpha_blend uses. */
static inline void wm_blend_pixel(uint32_t *p, uint32_t fg, uint8_t a)
{
    if (a == 0) return;
    if (a == 0xFF) { *p = fg; return; }
    uint32_t dst = *p;
    uint8_t fr = (fg >> 16) & 0xFF, fg_ = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    uint8_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint16_t inv = (uint16_t)(255 - a);
    uint8_t orr = (uint8_t)(((uint16_t)fr  * a + (uint16_t)dr * inv) / 255);
    uint8_t org = (uint8_t)(((uint16_t)fg_ * a + (uint16_t)dg * inv) / 255);
    uint8_t orb = (uint8_t)(((uint16_t)fb  * a + (uint16_t)db * inv) / 255);
    *p = ((uint32_t)0xFFu << 24) | ((uint32_t)orr << 16)
       | ((uint32_t)org  <<  8) |  (uint32_t)orb;
}

/* Chapter 102 -- pixel-accurate text measurement.
 *
 * Sums the per-glyph advance widths for `s_user` using the same
 * default font that wm_draw_text uses. Mirrors the loop in
 * wm_draw_text exactly so callers get a width that matches what
 * they'll actually paint. Stops at '\n' (callers wanting multi-line
 * measure should split first). Returns the pixel width as a
 * non-negative long, or -EFAULT if `s_user` isn't readable. */
long wm_measure_text(const char *s_user)
{
    char buf[256];
    long got = copy_string_from_user(buf, (uint64_t)(uintptr_t)s_user,
                                     sizeof(buf));
    if (got < 0) return -EFAULT;

    const struct bitmap_font *font = font_get_default();
    if (!font) return 0;

    uint32_t w = 0;
    for (size_t i = 0; buf[i]; i++) {
        char ch = buf[i];
        if (ch == '\n') break;
        struct glyph_info gi;
        if (font_get_glyph(font, (uint32_t)(uint8_t)ch, &gi) != 0) continue;
        uint32_t adv = gi.advance ? gi.advance : font->cell_width;
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
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;

    char buf[256];
    long got = copy_string_from_user(buf, (uint64_t)(uintptr_t)s_user,
                                     sizeof(buf));
    if (got < 0) return -EFAULT;

    const struct bitmap_font *font = font_get_default();
    if (!font) return -EINVAL;

    /* Chapter 102: render glyph-by-glyph using the new font_get_glyph
     * API. Bitmap-kind fonts produce alpha values of 0 or 255 so the
     * blend collapses to the old fast path; TTF-kind fonts produce
     * full grayscale AA. Per-glyph advance gives proportional spacing
     * with no caller change.
     *
     * Pen origin (cx, y) is the cell's top-left; the baseline sits
     * at (y + cell_height - 4) for TTF and at (y + cell_height) for
     * the bitmap font -- matching what text.c::text_draw_glyph does. */
    uint32_t cx = x;
    uint32_t baseline_off = (font->kind == BITMAP_FONT_KIND_TTF)
                                ? (uint32_t)font->cell_height - 4u
                                : (uint32_t)font->cell_height;

    for (size_t i = 0; buf[i]; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            y += font->cell_height + font->line_spacing;
            cx = x;
            continue;
        }
        struct glyph_info gi;
        if (font_get_glyph(font, (uint32_t)(uint8_t)ch, &gi) != 0) continue;

        uint32_t adv = gi.advance ? gi.advance : font->cell_width;

        /* Wrap if this glyph won't fit on the line. */
        if (cx + adv > w->w) {
            cx = x;
            y += font->cell_height + font->line_spacing;
        }
        if (y + font->cell_height > w->h) break;

        int32_t bx = (int32_t)cx + gi.left_bearing;
        int32_t by = (int32_t)(y + baseline_off) - gi.top_bearing;

        for (int row = 0; row < gi.bitmap_h; row++) {
            for (int col = 0; col < gi.bitmap_w; col++) {
                int32_t px = bx + col;
                int32_t py = by + row;
                if (px < 0 || py < 0) continue;
                if ((uint32_t)px >= w->w || (uint32_t)py >= w->h) continue;
                uint8_t a = gi.pixels ? gi.pixels[row * gi.bitmap_w + col] : 0;
                uint32_t *slot = &((uint32_t *)w->pixels)[py * w->w + px];
                if (a == 0) {
                    if (!transparent) *slot = bg_bgra;
                    continue;
                }
                if (transparent) {
                    wm_blend_pixel(slot, fg_bgra, a);
                } else {
                    /* Blend fg over bg (deterministic, no fb readback). */
                    uint32_t tmp = bg_bgra;
                    wm_blend_pixel(&tmp, fg_bgra, a);
                    *slot = tmp;
                }
            }
        }

        cx += adv;
    }
    return 0;
}

long wm_flush(uint64_t pid, int32_t id)
{
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    compose_all();
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
    g_focus_id = w->id;
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
            g_focus_id = (best >= 0) ? g_wins[best].id : -1;
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
        g_focus_id = w->id;
    }
    compose_all();
    return 0;
}

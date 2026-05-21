/*
 * userspace/libgui/draw.h — chapter 108c software rasteriser.
 *
 * What this file is
 * -----------------
 *
 * The "GUI SDK" of a mid-1990s desktop OS used to be a thin C
 * library that turned high-level calls ("fill this rect with
 * this colour", "draw this string at this pixel") into byte
 * writes against a back-buffer the kernel handed the process.
 * Compositing was the kernel's job; rasterisation was the
 * app's.  We had everything but that split — chapter 108a gave
 * apps a `gui_window_fb` pointer, chapter 108b moved fonts to
 * /srv/font, but every GUI app was still calling
 * `gui_fill_rect` and `gui_draw_text` syscalls.  This chapter
 * finishes the move.
 *
 * Programming model
 * -----------------
 *
 *   1. `gui_create_window(w, h, title)` as before.
 *   2. `gui_window_fb(id, &fb)` once at startup.  `fb` now
 *      carries the window id (chapter 108c extension to the
 *      struct) so every subsequent draw_* call can take just
 *      the fb pointer and still issue damage.
 *   3. `draw_fill_rect`, `draw_text`, `draw_blit_bgra` etc.
 *      Every one writes BGRA bytes into `fb->pixels` directly
 *      — zero syscalls per primitive.  The cost is exactly the
 *      bytes touched.
 *   4. `gui_window_dirty(&fb, x, y, w, h)` once per paint to
 *      tell the WM the rect changed and needs compositing.
 *      A whole-window damage is fine if the per-rect bookkeeping
 *      isn't worth it; the WM clips internally.
 *
 * Text rendering
 * --------------
 *
 * `draw_text` and `draw_measure_text` are the userspace
 * counterparts of the kernel's `wm_draw_text` / `wm_measure_text`
 * (which still serve `/bin/notify` and panic dumps — see
 * "Why notify stays on the syscall path" in the chapter
 * prose).  They talk to /srv/font directly via the chapter-107
 * IPC bus; each process owns its own glyph cache.  See draw.c
 * for cache shape and reconnect policy.
 *
 * What this file is NOT
 * ---------------------
 *
 * Not a widget toolkit.  Buttons, scrollbars, text fields are
 * each app's own code today (and the dialog code lives in
 * libgui/save_dialog.h).  draw.h is the lowest layer — just
 * pixels and glyphs.
 *
 * Header-only inlines for the pure-pixel primitives; the
 * font-client functions are in draw.c because they hold state
 * (cache + persistent IPC fd) that has to live in one
 * translation unit.
 */

#ifndef LIBGUI_DRAW_H
#define LIBGUI_DRAW_H

#include "../libc/syscall.h"

/* ── Damage helper ────────────────────────────────────────────
 *
 * Apps that mapped a window typically pass the gui_fb pointer
 * around everywhere and forget the id.  `gui_window_dirty`
 * takes the fb directly — it's the only damage path 108c apps
 * should use.  The underlying gui_window_damage stays exported
 * for legacy callers (notify, etc.). */
static inline int gui_window_dirty(struct gui_fb *fb,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h)
{
    if (!fb) return -1;
    return gui_window_damage(fb->id, x, y, w, h);
}

/* ── Pure-pixel primitives ───────────────────────────────────
 *
 * No IPC; just bounds-checked stores into fb->pixels.  All
 * coordinates are window-content-relative.  Out-of-bounds
 * pixels are silently skipped (so callers can pass slightly-
 * negative or oversize rects without bothering to clip up
 * front — matches the kernel's wm_fill_rect ergonomics).
 *
 * BGRA byte order matches the framebuffer (chapter 26): the
 * uint32_t is laid out as B in bits 0..7, G in 8..15, R in
 * 16..23, A in 24..31. */
static inline void draw_fill_rect(struct gui_fb *fb,
                                  int32_t x, int32_t y,
                                  uint32_t w, uint32_t h,
                                  uint32_t bgra)
{
    if (!fb || !fb->pixels) return;
    if (w == 0 || h == 0) return;
    int32_t x0 = x, y0 = y;
    int32_t x1 = x + (int32_t)w;
    int32_t y1 = y + (int32_t)h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)fb->w) x1 = (int32_t)fb->w;
    if (y1 > (int32_t)fb->h) y1 = (int32_t)fb->h;
    if (x1 <= x0 || y1 <= y0) return;

    for (int32_t row = y0; row < y1; row++) {
        uint32_t *dst = (uint32_t *)(fb->pixels + (size_t)row * fb->stride);
        for (int32_t col = x0; col < x1; col++) dst[col] = bgra;
    }
}

static inline void draw_hline(struct gui_fb *fb,
                              int32_t x, int32_t y,
                              uint32_t w, uint32_t bgra)
{
    draw_fill_rect(fb, x, y, w, 1, bgra);
}

static inline void draw_vline(struct gui_fb *fb,
                              int32_t x, int32_t y,
                              uint32_t h, uint32_t bgra)
{
    draw_fill_rect(fb, x, y, 1, h, bgra);
}

/* ── Shared control chrome ───────────────────────────────
 *
 * Small, reusable primitives for classic desktop controls.
 * Apps can keep their own layout/event logic while sharing
 * a consistent visual treatment for buttons and text fields.
 */
static inline void draw_bevel_box(struct gui_fb *fb,
                                  int32_t x, int32_t y,
                                  uint32_t w, uint32_t h,
                                  uint32_t fill_bgra,
                                  uint32_t outer_bgra,
                                  uint32_t hi_bgra,
                                  uint32_t lo_bgra)
{
    if (!fb || !fb->pixels) return;
    if (w < 4 || h < 4) return;

    draw_fill_rect(fb, x + 2, y + 2, w - 4, h - 4, fill_bgra);

    /* Outer frame */
    draw_fill_rect(fb, x, y, w, 1u, outer_bgra);
    draw_fill_rect(fb, x, y, 1u, h, outer_bgra);
    draw_fill_rect(fb, x, y + (int32_t)h - 1, w, 1u, outer_bgra);
    draw_fill_rect(fb, x + (int32_t)w - 1, y, 1u, h, outer_bgra);

    /* Inner bevel */
    draw_fill_rect(fb, x + 1, y + 1, w - 2, 1u, hi_bgra);
    draw_fill_rect(fb, x + 1, y + 1, 1u, h - 2, hi_bgra);
    draw_fill_rect(fb, x + 1, y + (int32_t)h - 2, w - 2, 1u, lo_bgra);
    draw_fill_rect(fb, x + (int32_t)w - 2, y + 1, 1u, h - 2, lo_bgra);
}

static inline void draw_button_chrome(struct gui_fb *fb,
                                      int32_t x, int32_t y,
                                      uint32_t w, uint32_t h)
{
    draw_bevel_box(fb, x, y, w, h,
                   GUI_BGRA(236, 236, 236),
                   GUI_BGRA(60, 60, 60),
                   GUI_BGRA(255, 255, 255),
                   GUI_BGRA(144, 144, 144));
}

static inline void draw_textbox_chrome(struct gui_fb *fb,
                                       int32_t x, int32_t y,
                                       uint32_t w, uint32_t h)
{
    draw_bevel_box(fb, x, y, w, h,
                   GUI_BGRA(255, 255, 255),
                   GUI_BGRA(60, 60, 60),
                   GUI_BGRA(180, 180, 180),
                   GUI_BGRA(248, 248, 248));
}

/* Blit a BGRA source buffer of size (w, h) at row stride
 * `src_stride` (in 32-bit pixels) to (x, y) in fb.  Used by
 * /bin/desktop to copy the wallpaper in via memory-style
 * memcpy instead of N gui_fill_rect calls. */
static inline void draw_blit_bgra(struct gui_fb *fb,
                                  int32_t x, int32_t y,
                                  uint32_t w, uint32_t h,
                                  const uint32_t *src,
                                  uint32_t src_stride)
{
    if (!fb || !fb->pixels || !src) return;
    if (w == 0 || h == 0) return;
    int32_t x0 = x, y0 = y;
    int32_t x1 = x + (int32_t)w;
    int32_t y1 = y + (int32_t)h;
    int32_t sx0 = 0, sy0 = 0;
    if (x0 < 0) { sx0 = -x0; x0 = 0; }
    if (y0 < 0) { sy0 = -y0; y0 = 0; }
    if (x1 > (int32_t)fb->w) x1 = (int32_t)fb->w;
    if (y1 > (int32_t)fb->h) y1 = (int32_t)fb->h;
    if (x1 <= x0 || y1 <= y0) return;

    for (int32_t row = y0; row < y1; row++) {
        uint32_t *dst = (uint32_t *)(fb->pixels + (size_t)row * fb->stride);
        const uint32_t *s = src + (size_t)(sy0 + (row - y0)) * src_stride + sx0;
        int32_t cols = x1 - x0;
        for (int32_t col = 0; col < cols; col++) dst[x0 + col] = s[col];
    }
}

/* ── Text path (fontd client; cache state in draw.c) ────────
 *
 * draw_text:
 *   Render `s` at (x, y) in fb with foreground `fg_bgra`.
 *   If `transparent` is 0, blank pixels of every glyph cell
 *   are filled with `bg_bgra` (so callers don't have to clear
 *   the background rect first); if non-zero, the background
 *   is left untouched and partial-alpha pixels are blended
 *   against whatever was there already.  '\n' wraps to the
 *   next 16-px line at the original x.  Stops on out-of-bounds.
 *
 *   Connects to /srv/font lazily on the first call.  On
 *   subsequent calls, cached glyphs come back with zero IPC.
 *
 * draw_measure_text:
 *   Returns the pixel width `s` would occupy if drawn — sum
 *   of the per-glyph advance values, the same way draw_text
 *   would lay them out.  Stops at '\n'.  Used by widgets that
 *   want to centre or right-align text without first drawing
 *   it.  Same cache as draw_text.
 *
 * Both fall back gracefully when /srv/font is unreachable:
 * draw_text becomes a no-op for unknown glyphs (the rect is
 * left at the background colour) and draw_measure_text uses
 * an 8-px-per-character estimate so layout doesn't collapse.
 * This mirrors the kernel WM's behaviour during fontd respawn
 * and means a single missing /bin/fontd doesn't take the
 * desktop with it. */
void draw_text(struct gui_fb *fb,
               int32_t x, int32_t y,
               const char *s,
               uint32_t fg_bgra, uint32_t bg_bgra,
               int transparent);

/* draw_text_clipped:
 *   Same as draw_text but discards any pixel store that falls
 *   outside the rect (cl_x, cl_y, cl_w, cl_h) in fb coords.
 *   Glyph layout (advance, wrap, baseline) is unchanged — the
 *   clip only affects which already-laid-out pixels reach the
 *   buffer.  This lets a caller paint just the visible slice
 *   of a string into a partial-repaint region (e.g. a window
 *   title bar intersected with a small cursor-sweep rect)
 *   without losing the glyphs whose bounding boxes happen to
 *   straddle the clip edge.  Pass cl_x=0, cl_y=0, cl_w=fb->w,
 *   cl_h=fb->h for the unclipped behaviour (draw_text does
 *   exactly that). */
void draw_text_clipped(struct gui_fb *fb,
                       int32_t x, int32_t y,
                       const char *s,
                       uint32_t fg_bgra, uint32_t bg_bgra,
                       int transparent,
                       int32_t cl_x, int32_t cl_y,
                       int32_t cl_w, int32_t cl_h);
int  draw_measure_text(const char *s);

/* Cell height the text helpers assume.  Useful for callers
 * that want to centre text vertically inside a button (the
 * "y" they pass is the cell top, not the baseline). */
#define DRAW_TEXT_CELL_H   16u
#define DRAW_TEXT_BASELINE 12u

#endif /* LIBGUI_DRAW_H */

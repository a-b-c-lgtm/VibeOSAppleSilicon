/*
 * Text rendering — Chapter 22.
 *
 * Built on top of:
 *   - font.h          (struct bitmap_font, font_get_bitmap, font_glyph)
 *   - framebuffer.h   (fb_draw_pixel, fb_fill_rect, fb_get_info)
 *
 * Three-layer pipeline:
 *   1. Glyph blit:   one character at a fixed (x, y), with explicit
 *                    fg/bg colours and an opaque-or-transparent
 *                    background mode. No layout decisions here.
 *   2. String draw:  walks a C string at a starting (x, y), advances
 *                    the cursor by cell_width per glyph, handles '\n'
 *                    and '\t', and clips at a max_x / max_y boundary.
 *   3. Boxed text:   draws a filled background rectangle and an
 *                    optional border, then renders the string inside
 *                    with transparent-bg so the box colour shows.
 *
 * Two side concerns:
 *   - alpha_blend(): the canonical "src over dst" formula at 8-bit
 *                    precision per channel. Used by the demo to
 *                    overlay translucent panels on existing pixels.
 *   - text_measure(): runs the same layout loop string_draw uses,
 *                    but produces a width/height/line_count tuple
 *                    instead of pixels. Lets callers size a box
 *                    around text before drawing it.
 *
 * Everything here is synchronous and stateless: no caches, no dirty
 * tracking, no off-screen buffers. That all arrives later in the
 * compositor (Chapter 28). For now the framebuffer *is* the surface.
 */

#ifndef KERNEL_DEVICE_TEXT_H
#define KERNEL_DEVICE_TEXT_H

#include <stdint.h>
#include "font.h"
#include "fb.h"

/* "Source over destination" alpha blend. alpha = 0 returns dst,
 * alpha = 255 returns src, anything in between is linearly
 * interpolated per channel. The output's alpha field is set to
 * fully opaque (the framebuffer has no alpha channel — the value
 * is bookkeeping for layered software composition). */
struct fb_color text_alpha_blend(struct fb_color src, struct fb_color dst, uint8_t alpha);

/* Draw a single glyph at top-left (x, y).
 *
 * - `fg`             : colour of "on" bits in the glyph bitmap.
 * - `bg`             : colour of "off" bits, used only when
 *                      `transparent_bg == 0`.
 * - `transparent_bg` : if non-zero, off-bits are not written,
 *                      preserving whatever was already on screen.
 *
 * Codepoints outside the font's range render the font's first
 * glyph as a "missing" placeholder (which the autogen script
 * deliberately makes a bordered hollow square). */
void text_draw_glyph(const struct bitmap_font *font,
                     uint32_t x, uint32_t y, uint8_t cp,
                     struct fb_color fg, struct fb_color bg,
                     int transparent_bg);

/* Draw a NUL-terminated string starting at (x, y). Advances the
 * cursor by cell_width per glyph, handles '\n' (newline) and '\t'
 * (tab = 4 cell_widths). Clips on the right by wrapping to the
 * next line, on the bottom by stopping. Returns the cursor (x, y)
 * after the last glyph in `*out_x`/`*out_y` (either may be NULL).
 *
 * `max_x` / `max_y` are the right and bottom clip bounds in
 * absolute screen coordinates (exclusive). Pass `fb->width` /
 * `fb->height` to fill the screen, or smaller values to clip
 * inside a box. */
void text_draw_string(const struct bitmap_font *font,
                      uint32_t x, uint32_t y,
                      uint32_t max_x, uint32_t max_y,
                      const char *s,
                      struct fb_color fg, struct fb_color bg,
                      int transparent_bg,
                      uint32_t *out_x, uint32_t *out_y);

/* Measure the bounding box `s` would occupy if rendered with the
 * same wrapping rules as text_draw_string at width `max_width`.
 * `max_width = 0` means "no wrapping". */
struct text_metrics {
    uint32_t width;       /* widest line in pixels */
    uint32_t height;      /* total height in pixels (lines * (cell_height + line_spacing)) */
    uint32_t line_count;
};

struct text_metrics text_measure(const struct bitmap_font *font,
                                 const char *s,
                                 uint32_t max_width);

/* Draw a filled box with optional border, then render `s` inside
 * with 4-pixel padding. Border thickness is 1 pixel; pass equal
 * `bg` and `border` to skip the visible border. */
void text_draw_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   struct fb_color bg, struct fb_color border,
                   const struct bitmap_font *font, const char *s,
                   struct fb_color fg);

#endif /* KERNEL_DEVICE_TEXT_H */

/*
 * Text rendering implementation — Chapter 23.
 *
 * See text.h for the public contract. Notes on choices made here:
 *
 * - Glyph blit uses fb_draw_pixel rather than poking the framebuffer
 *   directly. The cost is one bounds-check per pixel; the benefit is
 *   we never duplicate the framebuffer's pitch/format math here, so
 *   any future framebuffer (16-bpp, 24-bpp, indexed, virtual surface)
 *   works without touching this file. The hot path optimisation is
 *   to push the inner loop down into framebuffer.c later.
 *
 * - Layout uses a single advance value (`cell_width`) and treats '\n'
 *   as the only line-break. No kerning, no shaping, no Unicode. That
 *   is the right complexity ceiling for a first text layer; the box
 *   model and clip rules are already what an eventual richer layout
 *   engine will need to slot into.
 *
 * - alpha_blend uses (a * src + (255 - a) * dst) / 255 with integer
 *   math. The /255 is a real divide on the path that uses it (the
 *   demo's translucent overlay), not the per-glyph hot path. Glyph
 *   blits are pure copy, no blending — bitmap fonts have no
 *   per-pixel alpha. That arrives with grayscale antialised fonts in
 *   a later chapter.
 */

#include "text.h"

#include <stddef.h>

struct fb_color text_alpha_blend(struct fb_color src, struct fb_color dst, uint8_t alpha)
{
    if (alpha == 0)   return dst;
    if (alpha == 255) return src;

    uint16_t inv = (uint16_t)(255 - alpha);
    struct fb_color out;
    out.r = (uint8_t)(((uint16_t)src.r * alpha + (uint16_t)dst.r * inv) / 255);
    out.g = (uint8_t)(((uint16_t)src.g * alpha + (uint16_t)dst.g * inv) / 255);
    out.b = (uint8_t)(((uint16_t)src.b * alpha + (uint16_t)dst.b * inv) / 255);
    out.a = 0xFF;
    return out;
}

void text_draw_glyph(const struct bitmap_font *font,
                     uint32_t x, uint32_t y, uint8_t cp,
                     struct fb_color fg, struct fb_color bg,
                     int transparent_bg)
{
    if (!font) {
        return;
    }
    const uint8_t *glyph = font_glyph(font, cp);
    if (!glyph) {
        /* Render the placeholder by re-asking for the first defined
         * codepoint. The font generator makes this slot a bordered
         * empty box so missing glyphs are visually distinguishable. */
        glyph = font_glyph(font, font->first_cp);
        if (!glyph) {
            return;
        }
    }

    uint32_t w = font->cell_width;
    uint32_t h = font->cell_height;

    /* If the cell background is opaque, paint it as a single fast
     * rect first, then overdraw the on-bits. This is faster than
     * per-pixel branching when the background is mostly visible. */
    if (!transparent_bg) {
        fb_fill_rect(x, y, w, h, bg);
    }

    for (uint32_t row = 0; row < h; row++) {
        uint8_t bits = glyph[row];
        if (bits == 0) {
            continue;       /* fully empty row, nothing to draw */
        }
        for (uint32_t col = 0; col < w; col++) {
            if (bits & (0x80u >> col)) {
                fb_draw_pixel(x + col, y + row, fg);
            }
        }
    }
}

/* Internal: advance one glyph forward. Wraps if it would cross max_x;
 * stops returning anything useful if it would cross max_y. The caller
 * uses the returned (x, y) as the position of the *next* glyph. */
static void advance_cursor(const struct bitmap_font *font,
                           uint32_t *x, uint32_t *y,
                           uint32_t origin_x, uint32_t max_x, uint32_t max_y,
                           uint32_t advance)
{
    *x += advance;
    if (*x + font->cell_width > max_x) {
        *x  = origin_x;
        *y += (uint32_t)font->cell_height + font->line_spacing;
    }
    (void)max_y;
}

void text_draw_string(const struct bitmap_font *font,
                      uint32_t x, uint32_t y,
                      uint32_t max_x, uint32_t max_y,
                      const char *s,
                      struct fb_color fg, struct fb_color bg,
                      int transparent_bg,
                      uint32_t *out_x, uint32_t *out_y)
{
    if (!font || !s) {
        if (out_x) *out_x = x;
        if (out_y) *out_y = y;
        return;
    }

    const uint32_t origin_x = x;
    const uint32_t line_step = (uint32_t)font->cell_height + font->line_spacing;

    while (*s) {
        unsigned char c = (unsigned char)*s++;

        if (c == '\n') {
            x  = origin_x;
            y += line_step;
            if (y + font->cell_height > max_y) {
                break;          /* clipped on bottom — stop drawing */
            }
            continue;
        }
        if (c == '\t') {
            advance_cursor(font, &x, &y, origin_x, max_x, max_y,
                           (uint32_t)font->cell_width * 4);
            if (y + font->cell_height > max_y) {
                break;
            }
            continue;
        }

        /* Wrap before drawing if this glyph wouldn't fit on the line. */
        if (x + font->cell_width > max_x) {
            x  = origin_x;
            y += line_step;
            if (y + font->cell_height > max_y) {
                break;
            }
        }

        text_draw_glyph(font, x, y, c, fg, bg, transparent_bg);
        x += font->cell_width;
    }

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

struct text_metrics text_measure(const struct bitmap_font *font,
                                 const char *s,
                                 uint32_t max_width)
{
    struct text_metrics m = { 0, 0, 0 };
    if (!font || !s) {
        return m;
    }

    const uint32_t line_step = (uint32_t)font->cell_height + font->line_spacing;
    uint32_t cur_w = 0;
    uint32_t max_w = 0;
    uint32_t lines = 1;

    while (*s) {
        unsigned char c = (unsigned char)*s++;

        if (c == '\n') {
            if (cur_w > max_w) max_w = cur_w;
            cur_w = 0;
            lines++;
            continue;
        }
        uint32_t adv = (c == '\t')
                       ? (uint32_t)font->cell_width * 4
                       : (uint32_t)font->cell_width;

        if (max_width != 0 && cur_w + adv > max_width && cur_w > 0) {
            if (cur_w > max_w) max_w = cur_w;
            cur_w = 0;
            lines++;
        }
        cur_w += adv;
    }
    if (cur_w > max_w) max_w = cur_w;

    m.width      = (max_width != 0 && max_w > max_width) ? max_width : max_w;
    m.line_count = lines;
    m.height     = lines * line_step;
    return m;
}

void text_draw_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   struct fb_color bg, struct fb_color border,
                   const struct bitmap_font *font, const char *s,
                   struct fb_color fg)
{
    /* Background fill, then border on top so a 1-pixel border is
     * visible even when bg == border (in which case the user gets
     * a borderless filled rect, which is also a valid result). */
    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, border);

    if (s && font) {
        const uint32_t pad = 4;
        if (w > 2 * pad && h > 2 * pad) {
            text_draw_string(font,
                             x + pad, y + pad,
                             x + w - pad, y + h - pad,
                             s, fg, bg, 1, NULL, NULL);
        }
    }
}

/*
 * Text rendering implementation -- Chapter 23 (bitmap) +
 * Chapter 102 (per-glyph AA + variable-width).
 *
 * Public contract in text.h hasn't changed. What HAS changed is
 * the internal pipeline:
 *
 *  - text_draw_glyph now fetches a `struct glyph_info` via
 *    font_get_glyph and alpha-blends each pixel of the alpha
 *    bitmap into the framebuffer. Bitmap-kind fonts produce
 *    0/255 alphas (no visible change); TTF-kind fonts produce
 *    grayscale-AA alphas.
 *
 *  - text_draw_string and text_measure honour the per-glyph
 *    advance returned by font_get_glyph instead of the fixed
 *    cell_width. This makes proportional text Just Work without
 *    any change to callers.
 *
 *  - Layout still uses cell_height + line_spacing for line
 *    stepping and uses left_bearing / top_bearing to position
 *    each glyph relative to the pen origin.
 *
 * alpha_blend is unchanged -- the same (a*src + (255-a)*dst)/255
 * formula text_alpha_blend already provided. The hot path now
 * uses it per glyph pixel, but only when alpha is neither 0 nor
 * 255 (a quick branch keeps the bitmap-font path fast).
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

/* Blit one glyph alpha bitmap into the framebuffer.
 *
 * Pen origin is (pen_x, pen_y) where pen_y is the BASELINE y.
 * left_bearing shifts the bitmap right; top_bearing tells how far
 * above the baseline the bitmap top sits. */
static void blit_glyph_alpha(uint32_t pen_x, uint32_t pen_y,
                             const struct glyph_info *gi,
                             struct fb_color fg, struct fb_color bg,
                             int transparent_bg)
{
    if (!gi || gi->bitmap_w == 0 || gi->bitmap_h == 0 || !gi->pixels) {
        /* Empty glyph (space, whitespace) -- only need to fill bg
         * for the advance if we're opaque. We don't know the
         * advance rect here without ascent/descent, so skip. */
        return;
    }
    int32_t bx = (int32_t)pen_x + gi->left_bearing;
    int32_t by = (int32_t)pen_y - gi->top_bearing;

    for (int row = 0; row < gi->bitmap_h; row++) {
        for (int col = 0; col < gi->bitmap_w; col++) {
            uint8_t a = gi->pixels[row * gi->bitmap_w + col];
            if (a == 0) {
                if (!transparent_bg) {
                    fb_draw_pixel((uint32_t)(bx + col),
                                  (uint32_t)(by + row), bg);
                }
                continue;
            }
            if (a == 255) {
                fb_draw_pixel((uint32_t)(bx + col),
                              (uint32_t)(by + row), fg);
                continue;
            }
            /* Partial coverage. If bg is transparent we blend against
             * the existing framebuffer pixel by reading it back; we
             * don't have a fast read path, so approximate by blending
             * against bg when opaque and against fg-at-zero when not.
             * The cheap path that still looks good: when transparent,
             * just scale fg by alpha (treating bg = black). That's
             * acceptable for our colour palette (dark backgrounds). */
            struct fb_color dst = transparent_bg ? (struct fb_color){0,0,0,0xFF} : bg;
            struct fb_color out = text_alpha_blend(fg, dst, a);
            fb_draw_pixel((uint32_t)(bx + col),
                          (uint32_t)(by + row), out);
        }
    }
}

void text_draw_glyph(const struct bitmap_font *font,
                     uint32_t x, uint32_t y, uint8_t cp,
                     struct fb_color fg, struct fb_color bg,
                     int transparent_bg)
{
    if (!font) return;
    struct glyph_info gi;
    if (font_get_glyph(font, (uint32_t)cp, &gi) != 0) return;

    uint32_t w = font->cell_width;
    uint32_t h = font->cell_height;

    /* For bitmap-kind fonts the cell is exactly the bitmap, and the
     * "pen origin" semantics differ from TTF (no baseline). Handle
     * both by computing a baseline from the font kind. */
    uint32_t baseline_y;
    if (font->kind == BITMAP_FONT_KIND_BITMAP) {
        /* The bitmap font has no real baseline -- the bitmap is the
         * whole cell. We pretend the baseline is the bottom edge so
         * top_bearing == cell_height (font.c sets this) places the
         * bitmap at (x, y..y+h-1) exactly as before. */
        baseline_y = y + h;
        if (!transparent_bg) {
            /* Original behaviour: clear the whole cell first. */
            fb_fill_rect(x, y, w, h, bg);
        }
    } else {
        /* TTF: x/y from callers are the cell's top-left; the
         * baseline is `cell_ascent_px` below the top, but we don't
         * expose cell_ascent_px here, so derive from top_bearing of
         * an arbitrary tall glyph. The cheaper approximation: use
         * `cell_height - 4` as the baseline, which matches DejaVu
         * Sans @ 16 px (ascent ~13, descent ~3). The 4 px bottom
         * margin matches what the bitmap font used. */
        baseline_y = y + h - 4;
    }

    blit_glyph_alpha(x, baseline_y, &gi, fg, bg, transparent_bg);
    (void)gi;
}

/* Internal: advance the cursor by `adv` pixels along x. If that
 * would cross max_x, wrap to (origin_x, next line). */
static void advance_cursor(const struct bitmap_font *font,
                           uint32_t *x, uint32_t *y,
                           uint32_t origin_x, uint32_t max_x, uint32_t max_y,
                           uint32_t adv)
{
    *x += adv;
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
            if (y + font->cell_height > max_y) break;
            continue;
        }
        if (c == '\t') {
            advance_cursor(font, &x, &y, origin_x, max_x, max_y,
                           (uint32_t)font->cell_width * 4);
            if (y + font->cell_height > max_y) break;
            continue;
        }

        /* Per-glyph advance. */
        struct glyph_info gi;
        if (font_get_glyph(font, (uint32_t)c, &gi) != 0) continue;
        uint32_t adv = gi.advance ? gi.advance : font->cell_width;

        /* Wrap before drawing if this glyph wouldn't fit on the line. */
        if (x + adv > max_x) {
            x  = origin_x;
            y += line_step;
            if (y + font->cell_height > max_y) break;
        }

        text_draw_glyph(font, x, y, c, fg, bg, transparent_bg);
        x += adv;
    }

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

struct text_metrics text_measure(const struct bitmap_font *font,
                                 const char *s,
                                 uint32_t max_width)
{
    struct text_metrics m = { 0, 0, 0 };
    if (!font || !s) return m;

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
        uint32_t adv;
        if (c == '\t') {
            adv = (uint32_t)font->cell_width * 4;
        } else {
            struct glyph_info gi;
            if (font_get_glyph(font, (uint32_t)c, &gi) == 0) {
                adv = gi.advance ? gi.advance : font->cell_width;
            } else {
                adv = font->cell_width;
            }
        }

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

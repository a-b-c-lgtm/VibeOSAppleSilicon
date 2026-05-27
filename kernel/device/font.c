/*
 * Font registration -- Chapter 22 (bitmap) + Chapter 104 (TTF) +
 * Chapter 115 (TTF moved to userspace fontd).
 *
 * Pre-chapter-108b this file owned both a bitmap font and a TTF
 * font, dispatching font_get_glyph on font->kind.  Chapter 115
 * moved the TTF rasteriser into /bin/fontd; the kernel-side TTF
 * code is gone.  This file is back to the chapter-23 shape:
 * one always-available bitmap font, exposed via font_get_bitmap()
 * and font_get_glyph().
 *
 * The bitmap font is still used by:
 *   - early-boot splash (before any userspace runs)
 *   - panic / diagnostic paths
 *   - window title bars and the close-button decoration
 *   - wm_draw_text's fallback when fontd is briefly unreachable
 *     (boot window or during respawn after a crash)
 */

#include "font.h"
#include "font_8x16.h"

#include <stddef.h>

static const struct bitmap_font g_bitmap_font = {
    .cell_width   = FONT_8X16_WIDTH,
    .cell_height  = FONT_8X16_HEIGHT,
    .line_spacing = 2,
    .data         = (const uint8_t *)font_8x16_data,
    .glyph_count  = FONT_8X16_GLYPH_COUNT,
    .first_cp     = FONT_8X16_FIRST_CP,
    .last_cp      = FONT_8X16_LAST_CP,
};

/* Scratch buffer for the bitmap font's font_get_glyph synthesis.
 * Sized for the largest bitmap glyph we ever produce (8x16 = 128 B).
 * The synthesis is single-threaded inside any one render call;
 * callers may not retain the pointer beyond the call. The text
 * renderers don't retain it. */
#define BITMAP_GLYPH_SCRATCH_BYTES  (8u * 16u)
static uint8_t g_bitmap_glyph_scratch[BITMAP_GLYPH_SCRATCH_BYTES];

const struct bitmap_font *font_get_bitmap(void)
{
    return &g_bitmap_font;
}

const uint8_t *font_glyph(const struct bitmap_font *font, uint8_t cp)
{
    if (!font) return NULL;
    if (cp < font->first_cp || cp > font->last_cp) {
        return NULL;
    }
    uint32_t index = (uint32_t)(cp - font->first_cp);
    return font->data + (index * (uint32_t)font->cell_height);
}

/* Synthesise a row-major alpha bitmap from the 1-bpp font data for
 * codepoint `cp`. Writes into g_bitmap_glyph_scratch and fills `out`. */
int font_get_glyph(const struct bitmap_font *font, uint32_t cp,
                   struct glyph_info *out)
{
    if (!font || !out) return -1;

    const uint8_t *bits = font_glyph(font, (uint8_t)(cp & 0xFFu));
    if (!bits) bits = font_glyph(font, font->first_cp);

    uint32_t w = font->cell_width;
    uint32_t h = font->cell_height;

    for (uint32_t row = 0; row < h; row++) {
        uint8_t byte = bits ? bits[row] : 0;
        for (uint32_t col = 0; col < w; col++) {
            uint8_t on = (byte & (0x80u >> col)) ? 0xFFu : 0x00u;
            g_bitmap_glyph_scratch[row * w + col] = on;
        }
    }

    out->pixels       = g_bitmap_glyph_scratch;
    out->bitmap_w     = (uint16_t)w;
    out->bitmap_h     = (uint16_t)h;
    out->left_bearing = 0;
    out->top_bearing  = (int16_t)h;          /* baseline = bottom of cell */
    out->advance      = (uint16_t)w;
    return 0;
}

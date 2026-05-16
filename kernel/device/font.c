/*
 * Font registration -- Chapter 23 (bitmap) + Chapter 102 (TTF).
 *
 * Owns two struct bitmap_font instances:
 *
 *   g_bitmap_font  -- the original 8x16 grid in font_8x16.h.
 *                    Always available (no init, no allocation).
 *                    Returned by font_get_bitmap(); used by panic
 *                    paths and the early-boot splash before kmalloc
 *                    is up.
 *
 *   g_ttf_font     -- DejaVu Sans @ 16 px. Initialised lazily by
 *                    font_init_ttf() at boot, after kmalloc is up.
 *                    Returned by font_get_default() once initialised;
 *                    before then font_get_default() falls back to the
 *                    bitmap font so callers that run during early
 *                    boot keep working.
 *
 * font_get_glyph() is the single entry point for renderers; it
 * dispatches on `font->kind` and either synthesises a 0/255 alpha
 * bitmap from the bitmap-font data or returns a pointer into the
 * TTF glyph cache.
 */

#include "font.h"
#include "font_8x16.h"
#include "ttf.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Bitmap font                                                        */
/* ------------------------------------------------------------------ */

static const struct bitmap_font g_bitmap_font = {
    .kind         = BITMAP_FONT_KIND_BITMAP,
    .cell_width   = FONT_8X16_WIDTH,
    .cell_height  = FONT_8X16_HEIGHT,
    .line_spacing = 2,
    .data         = (const uint8_t *)font_8x16_data,
    .glyph_count  = FONT_8X16_GLYPH_COUNT,
    .first_cp     = FONT_8X16_FIRST_CP,
    .last_cp      = FONT_8X16_LAST_CP,
    .priv         = NULL,
};

/* Scratch buffer for the bitmap kind's font_get_glyph synthesis.
 * Sized for the largest bitmap glyph we ever produce (8x16 = 128 B).
 * The synthesis is single-threaded inside any one render call;
 * callers may not retain the pointer beyond the call. The text
 * renderers don't retain it. */
#define BITMAP_GLYPH_SCRATCH_BYTES  (8u * 16u)
static uint8_t g_bitmap_glyph_scratch[BITMAP_GLYPH_SCRATCH_BYTES];

/* ------------------------------------------------------------------ */
/* TTF font                                                           */
/* ------------------------------------------------------------------ */

/* Filled in by font_init_ttf(). Until then `.kind` is BITMAP so
 * font_get_default() never returns a half-initialised TTF face. */
static struct bitmap_font g_ttf_font = {
    .kind = BITMAP_FONT_KIND_BITMAP,
};

static int g_ttf_ready = 0;

const struct bitmap_font *font_get_bitmap(void)
{
    return &g_bitmap_font;
}

const struct bitmap_font *font_get_default(void)
{
    return g_ttf_ready ? &g_ttf_font : &g_bitmap_font;
}

void font_init_ttf(void)
{
    if (g_ttf_ready) {
        return;
    }
    if (ttf_init_default(&g_ttf_font) == 0) {
        g_ttf_ready = 1;
    }
    /* On failure g_ttf_font stays BITMAP-kind and font_get_default
     * continues to return g_bitmap_font. The kernel still boots
     * with text; we just don't get TTF until the next boot. */
}

const uint8_t *font_glyph(const struct bitmap_font *font, uint8_t cp)
{
    if (!font || font->kind != BITMAP_FONT_KIND_BITMAP) {
        return NULL;
    }
    if (cp < font->first_cp || cp > font->last_cp) {
        return NULL;
    }
    uint32_t index = (uint32_t)(cp - font->first_cp);
    return font->data + (index * (uint32_t)font->cell_height);
}

/* Synthesise a row-major alpha bitmap from the 1-bpp font data for
 * codepoint `cp`. Writes into g_bitmap_glyph_scratch and fills `out`. */
static void bitmap_synth_glyph(const struct bitmap_font *font, uint8_t cp,
                               struct glyph_info *out)
{
    const uint8_t *bits = font_glyph(font, cp);
    if (!bits) {
        bits = font_glyph(font, font->first_cp);
    }
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
}

int font_get_glyph(const struct bitmap_font *font, uint32_t cp,
                   struct glyph_info *out)
{
    if (!font || !out) {
        return -1;
    }
    if (font->kind == BITMAP_FONT_KIND_TTF) {
        return ttf_get_glyph(font, cp, out);
    }
    /* Bitmap kind. The 8x16 font is ASCII-only, so anything outside
     * 0x20..0x7E renders as the placeholder. */
    bitmap_synth_glyph(font, (uint8_t)(cp & 0xFFu), out);
    return 0;
}

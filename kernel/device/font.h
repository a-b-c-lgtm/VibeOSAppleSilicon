/*
 * Bitmap font abstraction — Chapter 23.
 *
 * A single struct describes a fixed-cell monospaced bitmap font:
 * the per-glyph byte layout, the cell dimensions, the codepoint
 * range covered, and a pointer to the raw pixel data.
 *
 * The kernel ships with one built-in font (the 8x16 grid in
 * font_8x16.h, generated from scripts/gen_font_8x16.py). This
 * module exposes that font through a generic struct so that text.c
 * never #includes the font data directly — it always works
 * through `struct bitmap_font`. A second font (e.g. a smaller 8x8
 * status-bar font, or a runtime-loaded PSF file) plugs in by
 * filling in the same struct shape.
 *
 * Glyph data layout (the only layout this struct supports today):
 * one byte per scan row, MSB = leftmost pixel, `cell_height` rows
 * per glyph, glyphs stored consecutively in codepoint order
 * starting at `first_cp`. Variable-width fonts are explicitly
 * out of scope for the first text layer.
 */

#ifndef KERNEL_DEVICE_FONT_H
#define KERNEL_DEVICE_FONT_H

#include <stdint.h>

struct bitmap_font {
    const uint8_t *data;        /* glyph_count * cell_height bytes */
    uint16_t       glyph_count;
    uint8_t        cell_width;  /* must be <= 8 in this implementation */
    uint8_t        cell_height;
    uint8_t        first_cp;    /* lowest codepoint with a glyph */
    uint8_t        last_cp;     /* highest codepoint with a glyph */
    uint8_t        line_spacing;/* extra pixels between rendered lines */
};

/* Returns the kernel's default font (8x16). Always non-NULL. */
const struct bitmap_font *font_get_default(void);

/* Returns a pointer to the first byte of the glyph for `cp`, or
 * NULL if `cp` is outside the font's covered range. The returned
 * buffer is `font->cell_height` bytes long. */
const uint8_t *font_glyph(const struct bitmap_font *font, uint8_t cp);

#endif /* KERNEL_DEVICE_FONT_H */

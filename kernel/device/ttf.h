/*
 * TrueType font support -- Chapter 102.
 *
 * The kernel embeds DejaVu Sans (via the objcopy -I binary rule
 * in the Makefile that exposes _binary_DejaVuSans_ttf_start/_end)
 * and rasterises it at boot to a fixed pixel size (16 px). Glyphs
 * are cached on first use; the cache is a simple per-codepoint
 * table keyed on the BMP codepoint range (0..0xFFFF).
 *
 * Scope (kept deliberately small for chapter 102):
 *  - One face (DejaVu Sans), one pixel size (16).
 *  - Tables: head, maxp, cmap (format 4), hhea, hmtx, loca, glyf.
 *  - Simple glyphs only (compound glyphs render as a placeholder).
 *  - Quadratic Bezier flattening, scanline rasterisation, 4x4
 *    supersampled grayscale anti-aliasing.
 *  - No hinting (no bytecode interpreter).
 *  - Lazy caching: ttf_get_glyph allocates and rasterises on
 *    first miss; cache lives in kmalloc'd memory and is never
 *    evicted.
 *
 * Everything more ambitious -- subpixel positioning, multiple
 * sizes, multiple faces, the move to userspace -- is explicitly
 * deferred to later chapters (102b/c, 108a/b).
 */

#ifndef KERNEL_DEVICE_TTF_H
#define KERNEL_DEVICE_TTF_H

#include <stdint.h>
#include "font.h"

/* Initialise the default TTF font. Parses the embedded DejaVu Sans
 * blob, fills in `*out_font` (kind, cell_width, cell_height,
 * line_spacing, priv pointer to the parsed face) and returns 0 on
 * success. On failure leaves *out_font untouched and returns -1.
 *
 * Must be called once at boot, after kmalloc is up. Calling more
 * than once is a no-op as far as the returned font is concerned;
 * the second call allocates and parses again, leaking the first
 * face. Callers are expected to invoke this exactly once. */
int ttf_init_default(struct bitmap_font *out_font);

/* Resolve a codepoint to its rasterised glyph, populating `*out`.
 * On first request for a codepoint this rasterises and caches; on
 * subsequent requests it returns the cached glyph in O(1).
 *
 * Returns 0 on success. Returns -1 only on impossible inputs
 * (`font` not a TTF-kind font, `out` NULL). For unmapped
 * codepoints or rasterisation failures, returns the face's
 * placeholder (.notdef) glyph and returns 0 -- callers always
 * get something drawable. */
int ttf_get_glyph(const struct bitmap_font *font, uint32_t cp,
                  struct glyph_info *out);

#endif /* KERNEL_DEVICE_TTF_H */

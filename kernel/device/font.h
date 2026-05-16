/*
 * Bitmap font abstraction — Chapter 23, generalised in Chapter 102.
 *
 * Originally (chapter 23) this described a fixed-cell monospaced
 * bitmap font: one byte per scan row, MSB = leftmost pixel,
 * `cell_height` rows per glyph, every glyph the same width.
 *
 * Chapter 102 generalises the struct to also wrap a TrueType
 * outline font. The two kinds share enough surface (`cell_height`
 * for line spacing, `line_spacing` for inter-line gap, a way to
 * look up a glyph by codepoint) that callers can stay font-kind-
 * agnostic. The new front door for rendering is `font_get_glyph`
 * which returns a `struct glyph_info` (per-glyph advance, bearings,
 * and an alpha bitmap). Bitmap-font glyphs synthesise alpha values
 * of 0 or 255 from the original 1-bpp data; TTF glyphs return the
 * cached grayscale-AA bitmap produced by ttf.c.
 *
 * Three callers exist today:
 *   - kernel/device/text.c     — draws into the framebuffer
 *   - kernel/core/wm.c         — draws into window pixel buffers
 *   - kernel/core/main.c       — early-boot splash
 *
 * For the early-boot/panic paths we keep `font_get_bitmap()` as
 * an always-available, never-faults, no-allocation fallback. The
 * TTF rasteriser may allocate (kmalloc for the cache); the bitmap
 * font cannot. Anyone drawing before kmalloc is ready, or from a
 * panic context, must use the bitmap fallback.
 */

#ifndef KERNEL_DEVICE_FONT_H
#define KERNEL_DEVICE_FONT_H

#include <stdint.h>

enum bitmap_font_kind {
    BITMAP_FONT_KIND_BITMAP = 0,    /* original 1-bpp 8×16 path */
    BITMAP_FONT_KIND_TTF    = 1,    /* chapter 102: TrueType outlines */
};

struct bitmap_font {
    enum bitmap_font_kind kind;

    /* For the bitmap kind these are the actual glyph dimensions.
     * For the TTF kind `cell_width` is the FALLBACK advance used
     * for codepoints with no entry in `hmtx`; the per-glyph advance
     * comes from `glyph_info::advance` instead. `cell_height` is
     * the line height for line stepping in both kinds. */
    uint8_t  cell_width;
    uint8_t  cell_height;
    uint8_t  line_spacing;          /* extra pixels between rendered lines */

    /* Bitmap-kind fields (unused for TTF). */
    const uint8_t *data;            /* glyph_count * cell_height bytes */
    uint16_t       glyph_count;
    uint8_t        first_cp;        /* lowest covered codepoint */
    uint8_t        last_cp;         /* highest covered codepoint */

    /* Kind-specific private state. For TTF this points at the
     * parsed face + cache table (see ttf.c). */
    void *priv;
};

/* Per-glyph rendering info. The pixels array is `bitmap_w * bitmap_h`
 * bytes, one alpha value per pixel (0 = transparent, 255 = opaque).
 * `left_bearing` is the horizontal offset from the glyph's pen
 * position to the left edge of the bitmap; `top_bearing` is the
 * positive distance from the baseline to the top edge.
 *
 * `advance` is how far to move the pen to draw the next glyph.
 *
 * For bitmap-kind fonts: `pixels` points at a tiny on-stack/static
 * synthesised buffer with alpha 0 or 255 per cell pixel,
 * `bitmap_w = cell_width`, `bitmap_h = cell_height`, both bearings
 * = 0, advance = cell_width. (Callers must NOT cache the pointer
 * across calls — a bitmap-kind synthesis may reuse the same scratch.)
 *
 * For TTF-kind fonts: `pixels` points into the kernel-side glyph
 * cache; pointer is stable across calls (until the font is torn
 * down, which never happens). */
struct glyph_info {
    const uint8_t *pixels;          /* alpha bitmap, row-major */
    uint16_t bitmap_w;
    uint16_t bitmap_h;
    int16_t  left_bearing;          /* px from pen origin to bitmap left */
    int16_t  top_bearing;           /* px from baseline up to bitmap top */
    uint16_t advance;               /* px to advance pen after this glyph */
};

/* Returns the kernel's default font. Today that's the TTF font
 * (DejaVu Sans @ 16px); pre-chapter-102 it was the bitmap font. */
const struct bitmap_font *font_get_default(void);

/* Returns the always-safe bitmap fallback. Used by early-boot
 * splashes, panic dumps, and any other path that runs before
 * kmalloc is ready or that must not fault. Always non-NULL,
 * never allocates. */
const struct bitmap_font *font_get_bitmap(void);

/* Initialise the TTF font (DejaVu Sans @ 16 px). After this call
 * font_get_default() returns the TTF font; before it (or if it
 * fails) font_get_default() returns the bitmap fallback. Must be
 * called once at boot, after kmalloc is up. */
void font_init_ttf(void);

/* Resolve a codepoint to renderable glyph info. Fills `*out` and
 * returns 0 on success; returns -1 if `font` is NULL or `out` is
 * NULL. Always succeeds with some glyph if the inputs are valid:
 * unmapped codepoints fall back to a placeholder (the font's
 * “missing” glyph, or the bitmap-kind first-glyph slot).
 *
 * For the TTF kind this may rasterise on first use and grow the
 * cache; it must NOT be called from a context that can't allocate
 * (panic, IRQ). */
int font_get_glyph(const struct bitmap_font *font, uint32_t cp,
                   struct glyph_info *out);

/* Returns a pointer to the first byte of the bitmap-kind glyph
 * data for `cp`, or NULL if `cp` is outside the bitmap font's
 * covered range or `font` is not a bitmap-kind font. Kept for
 * legacy callers (kernel/core/wm.c::wm_draw_text used to call this
 * directly; chapter 102 migrated it to font_get_glyph). New code
 * should prefer font_get_glyph. */
const uint8_t *font_glyph(const struct bitmap_font *font, uint8_t cp);

#endif /* KERNEL_DEVICE_FONT_H */


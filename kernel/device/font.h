/*
 * Bitmap font abstraction — Chapter 23.
 *
 * Originally (chapter 23) this described a fixed-cell monospaced
 * bitmap font: one byte per scan row, MSB = leftmost pixel,
 * `cell_height` rows per glyph, every glyph the same width.
 *
 * Chapter 102 had grown this into a discriminated union (bitmap or
 * TTF) so the in-kernel renderer could speak to both faces.
 * Chapter 108b moved the TrueType rasteriser out of the kernel
 * (it now lives in /bin/fontd, talking to clients over chapter-107
 * IPC) and the kernel-side struct collapsed back to its chapter-23
 * shape: one always-available bitmap font for early boot, panic
 * dumps, window title bars, and the wm_draw_text fallback when
 * fontd is briefly unreachable.
 *
 * Three callers exist today:
 *   - kernel/device/text.c     — draws into the framebuffer
 *   - kernel/core/wm.c         — fallback when wm_font_get_glyph
 *                                 (the fontd client) can't reach
 *                                 the daemon yet
 *   - kernel/core/main.c       — early-boot splash
 *
 * font_get_bitmap() is always-available, never-faults, no
 * allocation. It's what panic and early-boot paths use, and what
 * wm.c falls back to when fontd isn't online.
 */

#ifndef KERNEL_DEVICE_FONT_H
#define KERNEL_DEVICE_FONT_H

#include <stdint.h>

struct bitmap_font {
    /* Actual glyph dimensions. */
    uint8_t  cell_width;
    uint8_t  cell_height;
    uint8_t  line_spacing;          /* extra pixels between rendered lines */

    const uint8_t *data;            /* glyph_count * cell_height bytes */
    uint16_t       glyph_count;
    uint8_t        first_cp;        /* lowest covered codepoint */
    uint8_t        last_cp;         /* highest covered codepoint */
};

/* Per-glyph rendering info. The pixels array is `bitmap_w * bitmap_h`
 * bytes, one alpha value per pixel (0 = transparent, 255 = opaque).
 * `left_bearing` is the horizontal offset from the glyph's pen
 * position to the left edge of the bitmap; `top_bearing` is the
 * positive distance from the baseline to the top edge.
 *
 * `advance` is how far to move the pen to draw the next glyph.
 *
 * For the bitmap font: `pixels` points at a static synthesised
 * buffer with alpha 0 or 255 per cell pixel, bearings = 0,
 * advance = cell_width. (Callers must NOT cache the pointer
 * across calls — a bitmap synthesis may reuse the same scratch.)
 *
 * For glyphs returned by the fontd-backed WM cache (see
 * kernel/core/wm_font.c): `pixels` points into the WM-resident
 * alpha cache populated from IPC replies; pointer is stable for
 * the kernel's lifetime. */
struct glyph_info {
    const uint8_t *pixels;          /* alpha bitmap, row-major */
    uint16_t bitmap_w;
    uint16_t bitmap_h;
    int16_t  left_bearing;          /* px from pen origin to bitmap left */
    int16_t  top_bearing;           /* px from baseline up to bitmap top */
    uint16_t advance;               /* px to advance pen after this glyph */
};

/* Returns the always-safe bitmap font. Used by early-boot
 * splashes, panic dumps, window title bars, and the
 * wm_draw_text fallback when fontd isn't yet reachable.
 * Always non-NULL, never allocates. */
const struct bitmap_font *font_get_bitmap(void);

/* Resolve a codepoint to renderable glyph info from the bitmap
 * font. Fills `*out` and returns 0 on success; returns -1 if
 * `font` is NULL or `out` is NULL. Always succeeds with some
 * glyph if the inputs are valid: unmapped codepoints fall back
 * to the font's first slot as a placeholder.
 *
 * For TTF-quality glyphs, callers reach for wm_font_get_glyph
 * (kernel/core/wm_font.h) instead — that one talks to /bin/fontd
 * over IPC and returns AA bitmaps. */
int font_get_glyph(const struct bitmap_font *font, uint32_t cp,
                   struct glyph_info *out);

/* Returns a pointer to the first byte of the bitmap font's glyph
 * data for `cp`, or NULL if `cp` is outside the bitmap font's
 * covered range. Kept for legacy callers; new code should prefer
 * font_get_glyph. */
const uint8_t *font_glyph(const struct bitmap_font *font, uint8_t cp);

#endif /* KERNEL_DEVICE_FONT_H */


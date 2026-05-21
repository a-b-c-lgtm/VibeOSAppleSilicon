/*
 * userspace/fontd/ttf.h — chapter 108b.
 *
 * Userspace port of the TTF rasteriser that previously lived in
 * kernel/device/ttf.{c,h} (chapter 102).  Same algorithms, same
 * pixel output — only the headers and the heap allocator differ.
 *
 * The kernel version had to share `struct bitmap_font` and
 * `struct glyph_info` with the kernel's framebuffer text path.
 * fontd is the ONLY caller of this module now, so we drop the
 * outer abstraction and expose the raw types directly.
 *
 * One face per process: fontd parses DejaVu Sans at startup,
 * caches glyphs at multiple pixel sizes on demand, and answers
 * IPC requests from the WM (and, in chapter 108c, from
 * userspace libgui clients).
 */

#ifndef FONTD_TTF_H
#define FONTD_TTF_H

#include <stdint.h>
#include <stddef.h>

/* Per-glyph rendering info.  Identical layout to the kernel's
 * legacy `struct glyph_info`; kept as its own type here so the
 * userspace daemon doesn't pretend to be the kernel renderer.
 *
 * `pixels` is a row-major alpha bitmap, `bitmap_w * bitmap_h`
 * bytes (0 = transparent, 255 = opaque).  Owned by the ttf
 * module's per-size cache; callers must NOT free it.  Stable
 * across calls until the face is torn down. */
struct font_glyph {
    const uint8_t *pixels;
    uint16_t bitmap_w;
    uint16_t bitmap_h;
    int16_t  left_bearing;
    int16_t  top_bearing;
    uint16_t advance;
};

/* Opaque face handle.  Allocated by ttf_init_face, owns the
 * parsed tables and per-(cp,size) glyph cache. */
struct ttf_face;

/* Parse a TTF blob and return an opaque face handle.  The
 * blob must remain readable for the face's lifetime — fontd
 * keeps the embedded DejaVu blob in .rodata, so we just stash
 * a pointer.  Returns NULL on parse failure. */
struct ttf_face *ttf_init_face(const uint8_t *blob, uint32_t blob_size);

/* Resolve `cp` at `size_px` to a rasterised glyph.  First
 * lookup at a new size rasterises the ASCII printable range
 * eagerly (matches the chapter-stub "pre-rasterise on warm
 * cache" promise); subsequent calls hit the cache.  Returns
 * 0 on success and fills `*out`; -1 on a hard failure (NULL
 * face, NULL out, ridiculous size).  Always renders SOMETHING
 * drawable for valid inputs — unknown codepoints fall back to
 * the face's .notdef placeholder. */
int ttf_get_glyph(struct ttf_face *face, uint32_t cp, uint16_t size_px,
                  struct font_glyph *out);

/* Same as ttf_get_glyph but skips bitmap rasterisation —
 * returns metrics only (advance, bearings, bitmap_w/h).
 * `pixels` is left NULL.  Cheaper for callers that only need
 * to measure text. */
int ttf_get_metrics(struct ttf_face *face, uint32_t cp, uint16_t size_px,
                    struct font_glyph *out);

/* Pre-rasterise U+0020..U+007E at `size_px` and add to the
 * cache.  Used at boot so the first wm_draw_text call doesn't
 * block on a string of cache misses.  Idempotent. */
void ttf_warm_ascii(struct ttf_face *face, uint16_t size_px);

#endif /* FONTD_TTF_H */

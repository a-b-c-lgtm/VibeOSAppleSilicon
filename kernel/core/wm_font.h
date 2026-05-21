/*
 * kernel/core/wm_font.h — chapter 108b WM-side font client.
 *
 * The kernel WM is now a client of /bin/fontd (chapter 108b
 * moved the TTF rasteriser into a userspace daemon).  This
 * module owns:
 *
 *   - One persistent IPC connection to /srv/font, opened
 *     lazily on first use and reconnected on EPIPE.
 *   - A per-codepoint glyph cache holding metrics + alpha
 *     bitmaps fetched from fontd.  Same shape as the kernel-
 *     resident cache that ttf.c maintained in chapter 102,
 *     just populated by IPC reply instead of by an in-process
 *     rasteriser.
 *
 * Callers use `wm_font_get_glyph(cp, &out)` exactly the way
 * the WM's text-drawing code used to call `font_get_glyph` —
 * the only difference is the source of the bytes.
 *
 * Fallback path: when fontd is unreachable (during the boot
 * window before init has spawned it, or while it's being
 * respawned after a crash), we transparently fall back to the
 * kernel bitmap font.  That keeps text rendering working at
 * every moment of the boot lifetime; the only visible effect
 * of fontd not being up is that letters look chunkier until it
 * comes back.
 *
 * Concurrency: per-conn serialisation is a coarse spin-yield
 * lock.  Cache reads bypass the lock (entries are added
 * atomically, never mutated, never freed).  Two threads
 * missing the cache on the same cp race harmlessly — the
 * "loser" populates a duplicate entry, the original is leaked
 * for the kernel's lifetime; this is once-ever per cp so the
 * leak is bounded by 256 entries (the flat cache range).  A
 * future chapter can add a "loading" sentinel if it ever
 * matters.
 */

#ifndef KERNEL_CORE_WM_FONT_H
#define KERNEL_CORE_WM_FONT_H

#include <stdint.h>
#include "../device/font.h"   /* struct glyph_info — wire compatible */

/* Look up `cp` at the default WM text size (16 px).  On
 * success populates *out and returns 0.  On failure returns
 * -1; callers should fall back to the bitmap font themselves
 * (the WM does — see wm_draw_text).
 *
 * Internally this:
 *   - hits the in-WM cache if available
 *   - else does one IPC round-trip to fontd, populates the
 *     cache, returns
 *   - else (fontd unreachable) returns -1 so caller can fall
 *     back to bitmap rendering.
 *
 * Safe to call from any kernel context that can block
 * (which today is anything inside a syscall handler).  Must
 * NOT be called from IRQ context, panic paths, or before
 * thread_current() returns non-NULL. */
int wm_font_get_glyph(uint32_t cp, struct glyph_info *out);

/* Returns the per-line cell height of the WM's TTF font in
 * pixels (16 today).  Stable across the kernel's lifetime —
 * the size is baked into the request fontd answers, and
 * fontd is configured at compile time. */
uint32_t wm_font_cell_height(void);

/* Returns the WM's TTF baseline offset (px from the top of
 * the cell to the baseline).  Used by wm_draw_text when
 * placing glyphs.  cell_height - baseline = descent. */
uint32_t wm_font_baseline_offset(void);

#endif /* KERNEL_CORE_WM_FONT_H */

# Chapter 107 — TrueType fonts and sub-pixel rendering

**Status:** Stub. Tracking milestone 87.

The bitmap font shipping in the WM has carried the system
through chapters 46–84. Real text rendering needs scalable
fonts. This chapter implements a subset of TrueType
sufficient to render the system UI in something like Inter
or Source Code Pro.

## What this chapter adds

- A TTF parser: `cmap`, `glyf`, `loca`, `head`, `hhea`,
  `hmtx`, `maxp` tables.
- A simple rasteriser: scanline fill with quadratic Bezier
  flattening; no hinting (skip the bytecode interpreter).
- A glyph cache keyed on `(font, codepoint, size)`.
- Subpixel positioning: store glyphs at three horizontal
  offsets (1/3, 2/3, 0) and pick the closest at render time.
- WM glyph cache lives in shared memory, populated by the
  first user that draws a given glyph.

## Prerequisites

- Chapter 48 — WM
- Chapter 89 — mmap (for shared glyph cache)

## Plan

- Pick a single open-source font; ship it in `/data/fonts/`.
- The rasteriser is the bulk of the chapter; ~600 lines.
- Performance budget: glyph render cost is a one-time
  per-glyph hit; cached forever.

## What you'll learn

- The "what is a glyph really" answer at the byte level.
- Bezier flattening done correctly.
- Why grayscale antialiasing was a step forward and
  subpixel was the next.

## What this unlocks

- A UI that doesn't look like 1985.
- Multiple font sizes in one window (notepad heading vs body,
  browser headings).

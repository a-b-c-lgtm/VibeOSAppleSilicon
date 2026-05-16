# Chapter 108b — Moving font rendering into userspace

**Status:** Stub. Tracking milestone 90e.

Chapter 102 added a TrueType rasteriser to the kernel.
That was the smallest possible diff that put scalable
text on screen: drop a parser + rasteriser behind the
existing `text_draw_glyph` and `wm_draw_text`
internals, leaving every userspace caller untouched.
The price was ~1500 lines of Bezier-flattening,
scanline-filling, glyph-caching code added to the
*kernel*. That's the wrong place for it.

This chapter moves all of it out. After it lands the
kernel grows back down by ~1500 lines, gains zero
syscalls, and the font work runs as a normal userspace
daemon. The lesson is exactly the one chapter 108
told for the clipboard: features whose only
prerequisite is "be there for everyone to talk to" do
not need to live in the kernel.

## Why not just leave it in the kernel?

Three reasons that only become visible *after* the
in-kernel version exists:

1. **Bezier-flattening + scanline rasterisation is
   pure computation, no privilege required.** Every
   line of `kernel/device/ttf.c` could run
   verbatim at EL0. Code that doesn't need EL1
   doesn't belong at EL1.
2. **It's a memory hog.** DejaVu Sans is ~750 KB
   on disk; parsed, ~1 MB resident; with a cache
   of rasterised glyphs at the sizes we use, easily
   2–3 MB. The kernel heap is shared by everyone
   for everything; growing it by 3 MB for *one*
   feature is bad smell. Userspace can be paged out
   or restarted.
3. **Fonts evolve.** Today DejaVu. Tomorrow a user
   wants a different system font, or per-app fonts,
   or to add support for OpenType features. Every
   one of those is a kernel rebuild today; in
   userspace it's `pkill -HUP fontd` plus a config
   edit.

The kernel rasteriser shipped in chapter 102 was the
right *first* step — it forced us to actually solve
the parser, the flattener, the cache, and the
glyph-blit math. Now that those exist as working
code, moving them across the EL boundary is mechanical.

## What this chapter adds

* **`/bin/fontd`** — the font daemon. Binds
  `/srv/font` at boot (chapter 107). Owns the parser,
  rasteriser, and glyph cache from chapter 102.
  Listens for two message kinds:

  | Request | Meaning | Reply |
  |---|---|---|
  | `RENDER {font_id, codepoint, size}` | rasterise a glyph | glyph metrics (advance, bearing, w, h) plus a page-aligned mmap handle for the alpha bitmap |
  | `METRICS {font_id, codepoint, size}` | measure only, no bitmap | metrics struct |

* **Shared glyph pages.** Reply bitmaps are returned
  as `mmap` handles (using the page-alignment +
  `SYS_GUI_MAP_*` machinery from chapter 108a, but
  applied to fontd's heap pages instead of window
  buffers). Each glyph is a fixed-stride alpha
  bitmap; multiple clients see the same bytes; the
  cache hit cost is one fd dup plus one mmap.

* **`libgui/text.h`** — the client API. Same
  signatures the kernel rasteriser exposed in
  chapter 102 (`gui_draw_text`, `gui_text_width`),
  but the implementation now talks to `/srv/font`
  and writes pixels into the window buffer
  (mapped via chapter 108a) directly. Apps recompile,
  don't change.

* **Kernel removals.** Delete `kernel/device/ttf.c`,
  the TTF parsing in `font.c`, the in-kernel glyph
  cache, the `wm_draw_text` rasterisation loop. The
  bitmap font header stays for boot-time messages
  (the WM still draws its own title bars before
  fontd starts).

## Prerequisites

* Chapter 102 — the in-kernel TTF rasteriser. We're
  moving its body, not rewriting it. Diff is
  largely "find each function, move it from
  `kernel/device/ttf.c` to `userspace/fontd/`."
* Chapter 107 — IPC. The font protocol is a
  textbook chapter-107 service.
* Chapter 108a — userspace window-buffer access.
  Without it, the client library can't blit
  rasterised glyphs into windows; we'd have to add
  a `SYS_GUI_BLIT_ALPHA` syscall and we'd be back
  to the same problem.
* Chapter 91 — userspace threads. `fontd` uses one
  worker thread per active size so a slow first
  render doesn't block reads from other clients.

## Design decisions

### Pre-rasterise common ranges at boot

`fontd` rasterises U+0020..U+007E (ASCII printables)
for the default size at startup, before binding
`/srv/font`. First-text-on-screen latency stays
zero. The full Latin-1 supplement and any other
ranges fault in on demand.

### One server, multiple fonts

The chapter-102 codepath only loaded DejaVu Sans
(plus Mono). `fontd` learns to multiplex: a
`{font_id, ...}` tuple selects between Sans, Mono,
and any later additions. `font_id` is allocated by
`fontd` on first `OPEN(path)` request; clients
cache it.

### Bitmap-font fallback for boot

The WM still ships the kernel bitmap font for
two pre-fontd scenarios: (a) early-boot splash
messages and panic dumps; (b) the first window
title bar drawn before fontd answers. Once
fontd's accept loop is running, the WM cuts
over to it for everything but panic output.

## Why we didn't do this in chapter 102

Same three reasons we don't ship every chapter as
its final form: prerequisites. Chapter 102 came
before chapter 107 (IPC) and chapter 108a
(userspace window buffers). Without those we
couldn't have built fontd at all. Building the
kernel version first gave us:

* working rasterisation code to *move*, not
  invent;
* a regression test (`test_truetype.py`) that
  would catch any pixel-level regression caused
  by the move;
* concrete numbers for cache size, glyph render
  time, memory cost — useful for designing the
  IPC protocol's chunk sizes and the shared-page
  strategy.

The book's general principle: ship the smallest
thing that works, then move it to its right home
once the right home exists. The clipboard story
(chapter 108) is the canonical example; this is
the second.

## Test plan

* Existing `test_truetype.py` (from chapter 102)
  is the regression. Same screendumps, same pixel
  comparison; the rendering path being kernel or
  userspace must be invisible to the test.
* New `test_fontd.py` — boot, then `kill` fontd
  and verify it respawns (chapter-107 supervisor),
  the WM redraws using the bitmap fallback during
  the gap, and text renders normally afterward.
* Memory test — `cat /proc/meminfo` (chapter 99)
  before and after the move shows kernel heap
  ~3 MB smaller.

## What you'll learn

* The *second* time a feature ships, you know what
  it really needs. The kernel version of TTF didn't
  know it wanted a multi-thread server; the
  userspace version does.
* How a working in-kernel feature graduates to
  userspace, and how to keep regression tests
  honest across the move.
* Why every mainstream OS draws fonts in
  userspace (fontconfig, CoreText's font server,
  DirectWrite) — and what they all learned the
  hard way first.

## What this unlocks

* Per-app font selection. `notepad` and `browser`
  open different `font_id`s; no kernel rebuild.
* Future font *features*: hinting, complex shaping
  (Arabic, Devanagari), colour glyphs (emoji) all
  land as fontd patches.
* OS-wide font config (`/etc/fonts.conf`) the same
  way every Linux desktop does it.

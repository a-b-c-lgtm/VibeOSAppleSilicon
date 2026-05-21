# Chapter 108b — Moving font rendering into userspace

Chapter 102 added a TrueType rasteriser to the kernel.
That was the smallest possible diff that put scalable
text on screen: drop a parser + rasteriser behind the
existing `text_draw_glyph` and `wm_draw_text`
internals, leaving every userspace caller untouched.
The price was ~1500 lines of Bezier-flattening,
scanline-filling, glyph-caching code added to the
*kernel*, plus an embedded ~750 KB DejaVuSans.ttf blob
linked into the kernel image. That's the wrong place
for any of it.

This chapter moves all of it out. After it lands the
kernel image is ~750 KB smaller (the font blob is
gone), `kernel/device/ttf.c` and its 888 lines have
been deleted, and the rasteriser runs as a normal
userspace daemon. The lesson is exactly the one
chapter 108 told for the clipboard: features whose
only prerequisite is "be there for everyone to talk
to" do not need to live in the kernel.

## Why not just leave it in the kernel?

Three reasons that only become visible *after* the
in-kernel version exists:

1. **Bezier-flattening + scanline rasterisation is
   pure computation, no privilege required.** Every
   line of `kernel/device/ttf.c` could run verbatim
   at EL0. Code that doesn't need EL1 doesn't belong
   at EL1.
2. **It's a memory hog.** DejaVu Sans is ~750 KB
   on disk; parsed, ~1 MB resident; with a cache
   of rasterised glyphs at the sizes we use, easily
   2–3 MB. The kernel heap is shared by everyone
   for everything; growing it by 3 MB for *one*
   feature is bad smell. Userspace can be paged
   out or restarted.
3. **Fonts evolve.** Today DejaVu. Tomorrow a
   different system font, or per-app fonts, or
   OpenType features. Every one of those is a
   kernel rebuild today; in userspace it's
   "respawn fontd" plus a config edit.

The kernel rasteriser shipped in chapter 102 was the
right *first* step — it forced us to actually solve
the parser, the flattener, the cache, and the
glyph-blit math. Now that those exist as working
code, moving them across the EL boundary is
mechanical.

## What this chapter adds

* **`/bin/fontd`** — the font daemon.
  ([userspace/fontd/fontd.c](userspace/fontd/fontd.c),
  [userspace/fontd/ttf.c](userspace/fontd/ttf.c).)
  Binds `/srv/font` at boot (chapter 107). Owns the
  parser, rasteriser, and glyph cache moved out of
  the kernel. Per-message-per-connection shape: each
  client `srv_connect` is one request, one reply,
  close. Same shape as `/bin/clipboardd`.
* **`userspace/libc/font_proto.h`** — the wire
  protocol.
  ([font_proto.h](userspace/libc/font_proto.h).)
  A 32-byte `struct font_msg` header (op, font id,
  codepoint, size, status, metrics) plus, on a
  successful `FONT_OP_GLYPH` reply, the alpha
  bitmap inline after the header.
* **`kernel/core/wm_font.c`** — the kernel-side
  IPC client.
  ([wm_font.c](kernel/core/wm_font.c),
  [wm_font.h](kernel/core/wm_font.h).) Holds one
  persistent `struct srv_conn` to `/srv/font`, plus
  a per-codepoint glyph cache. `wm_draw_text` calls
  `wm_font_get_glyph(cp, &gi)` first; on success
  the WM blits an AA bitmap exactly as before. On
  fontd unreachable (boot window or respawn) the
  call returns -1 and the WM falls back to the
  always-available kernel bitmap font.
* **`/bin/fontd` supervision in init.** Same
  pattern as chapter 108's `/bin/clipboardd`:
  one line in [init.c](userspace/init/init.c)
  registers fontd with the supervisor table so
  init respawns it if it dies.
* **Kernel removals.** Deleted
  `kernel/device/ttf.c` (888 lines). Simplified
  `kernel/device/font.c` from a bitmap/TTF
  discriminated union back to the chapter-23
  bitmap-only shape (143 → 88 lines). Removed
  `font_init_ttf()` and the boot-time TTF init
  call. Moved the DejaVuSans.ttf embed from the
  kernel image to the fontd ELF.

## Prerequisites

* Chapter 102 — the in-kernel TTF rasteriser. We're
  moving its body, not rewriting it. Diff is
  largely "find each function, copy it from
  `kernel/device/ttf.c` to `userspace/fontd/ttf.c`,
  swap `kmalloc/kfree` for `malloc/free`."
* Chapter 107 — IPC. The font protocol is a
  textbook chapter-107 service.
* Chapter 108 — the supervisor pattern in init.

## The architecture we landed on (and the one we didn't)

The chapter stub sketched a design where reply
bitmaps were returned as shared-memory mmap
handles — the client maps fontd's glyph pages
directly into its address space and reads alpha
bytes without a copy. That's a fine design in
steady state but it requires a new shared-memory
IPC primitive and makes cache invalidation messy
(when does fontd get to evict?).

What we built instead is simpler:

* **Inline bytes in the reply.** Every
  `FONT_OP_GLYPH` reply is one IPC datagram:
  32-byte header followed by `bmp_w * bmp_h`
  alpha bytes inline. A worst-case 16×16 glyph
  is 256 B of payload. The IPC cap is 64 KiB so
  we could go up to 256×256 (the rasteriser's
  hard limit) without re-architecting.
* **Kernel-side cache.** The kernel WM caches
  per-codepoint alpha bitmaps in its own heap
  via [wm_font.c](kernel/core/wm_font.c). One
  IPC round-trip per *cold* codepoint, ever; the
  hot path is a pointer load. With the ASCII
  range warmed at fontd startup and the WM cache
  filled in by ordinary use, almost every text
  draw misses the IPC path entirely.

The kernel-as-IPC-client design also means
**userspace apps need zero changes** for this
chapter. `gui_draw_text` is still a kernel
syscall, the syscall handler still calls
`wm_draw_text`, `wm_draw_text` now happens to
fetch glyphs over IPC. Notepad, browser,
gui_term, launcher, taskbar — none of them
recompiled, none of them changed a line. That's
the whole point: a chapter-102-era binary still
renders TTF text on a chapter-108b kernel.

Chapter 108c then takes the next step — adds a
`libgui/text.h` so userspace can talk to fontd
directly and skip the kernel round-trip for apps
that need many sizes or want to do their own
glyph caching.

## Design decisions

### Spin-yield lock, not blocking mutex

The WM-side cache + connection state is guarded
by [wm_font.c](kernel/core/wm_font.c)'s
`g_lock_busy`, taken with inline LL/SC and
released with `stlr`. Contended waiters call
`yield()` between attempts. The kernel doesn't
ship a blocking mutex API today — but holding a
spinning lock across an IPC round-trip that can
block on `srv_read` would burn a CPU core (or
livelock on UP) while we wait for fontd's reply.
Yielding gives the holder the CPU to make progress.

The leak-on-race acceptance: two threads missing
the same codepoint will both populate the cache;
the loser's allocation leaks for the kernel's
lifetime. Bounded at 256 entries (the flat cache
range), which is at most a few KB. Documented
trade-off in the header.

### Bitmap-font fallback for boot and respawn

The kernel still ships the chapter-23 bitmap font
([kernel/device/font.c](kernel/device/font.c)).
It's used for:

* the early-boot splash (before init runs and
  spawns fontd);
* window title bars (drawn by the WM into the
  framebuffer, not into a userspace window — no
  IPC round-trip in the compositor);
* panic / diagnostic paths;
* the `wm_draw_text` fallback when
  `wm_font_get_glyph` returns -1 because fontd
  isn't reachable.

That last one is what makes respawn safe: if fontd
crashes, the next text-draw call sees the conn
torn down, returns -1, and the WM transparently
falls back to bitmap text for one frame. Init's
reap loop respawns fontd. Within a few hundred
milliseconds the next cache miss re-establishes
the conn and TTF text returns.

### Pre-rasterise common ranges at boot

fontd rasterises U+0020..U+007E (ASCII printables)
at the default size (16 px) at startup via
`ttf_warm_ascii`, before binding `/srv/font`. That
means the very first `wm_font_get_glyph` request
hits a warm fontd cache. First-text-on-screen
latency stays close to what it was when the
rasteriser lived in the kernel.

## Test plan

The existing
[test_truetype.py](scripts/test_truetype.py) from
chapter 102 is the regression. It boots,
screen-dumps the framebuffer, and asserts (a)
intermediate-alpha pixels in launcher button
labels (proves AA is on), (b) glyph transitions
off the 8-pixel grid (proves proportional
spacing). Both checks fail loudly if the kernel
silently fell back to the bitmap font.
**Same script, no edits**; passing it proves the
userspace move is invisible to the rendering
pipeline.

[scripts/test_fontd.py](scripts/test_fontd.py) is
a new, more focused regression:

1. Asserts `[fontd] ready on /srv/font` appears
   in serial — proves init spawned the daemon
   and srv_bind succeeded.
2. Asserts the shell prompt is reached — proves
   the supervisor didn't block init.
3. Asserts ≥4 intermediate-alpha pixels in a
   button label band — proves the WM successfully
   reached fontd over IPC.

The respawn-on-crash path is exercised by
chapter 108's clipboard supervisor regression
([test_clipboard.py](scripts/test_clipboard.py)),
which uses the same `supervise()` code; no need
to re-test it here.

## What you'll learn

* The *second* time a feature ships, you know what
  it really needs. The kernel version of TTF didn't
  know it wanted a separate cache layer and a
  reconnect-on-EPIPE story; the userspace version
  does.
* How a working in-kernel feature graduates to
  userspace while keeping the syscall surface
  unchanged. The kernel becomes an IPC *client*,
  not the IPC server — a pattern that recurs
  every time a kernel feature graduates.
* Why every mainstream OS draws fonts in
  userspace (fontconfig, CoreText's font server,
  DirectWrite) — and what they all learned the
  hard way first.

## What this unlocks

* Chapter 108c adds `libgui/text.h` for apps that
  want to talk to fontd directly (skip the kernel
  round-trip, do their own caching, render at
  arbitrary sizes the WM doesn't know about).
* Per-app font selection. `notepad` and `browser`
  can open different `font_id`s; no kernel rebuild
  required.
* Future font *features*: hinting, complex shaping,
  colour glyphs — all land as fontd patches.

## Applied to

* Existing apps modified: **none.** The kernel WM
  bridges via `wm_font_get_glyph` so every existing
  GUI app picks up the userspace rasteriser for
  free.
* New apps added: `/bin/fontd`
  ([userspace/fontd/](userspace/fontd/)).
* Existing test scripts upgraded: none —
  [test_truetype.py](scripts/test_truetype.py)
  passes unchanged.
* New test scripts:
  [test_fontd.py](scripts/test_fontd.py).

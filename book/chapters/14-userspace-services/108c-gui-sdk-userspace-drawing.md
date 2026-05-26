# Chapter 108c — Moving the GUI SDK into userspace

> **Milestone in this chapter:** 90f — reimplement
> `gui_fill_rect` / `gui_draw_text` / `gui_present` as a
> userspace library that draws into the mapped pixel buffer
> from chapter 108a.
> **Code referenced:**
> - [userspace/libgui/](../../../userspace/libgui/) (the new
>   userspace SDK)
> - [userspace/launcher/](../../../userspace/launcher/),
>   [userspace/taskbar/](../../../userspace/taskbar/),
>   [userspace/notepad/](../../../userspace/notepad/),
>   [userspace/browser/](../../../userspace/browser/),
>   [userspace/gui_term/](../../../userspace/gui_term/),
>   [userspace/paint/](../../../userspace/paint/) (every GUI
>   app re-ported to the new SDK)
>
> **At the end of this chapter** you will have every GUI app
> drawing entirely in userspace, with the kernel WM reduced
> to compositing damaged rects. Builds on chapter 108a
> (mapped window buffers) and chapter 108b (userspace font
> server).

Chapter 108a gave userspace direct access to a window's pixel
buffer, but only the chapter-108a demo (`pixapp`) used it. Every
other GUI app — `launcher`, `taskbar`, `notepad`, `browser`,
`gui_term`, `paint`, `hellogui`, `desktop` — still drew via
`gui_fill_rect` / `gui_draw_text` / `gui_present` syscalls.
Each call crossed the EL0/EL1 boundary, the kernel WM
rasterised into `w->pixels`, the compositor ran. With map +
damage in place from 108a and a userspace font server in place
from 108b, software rasterisation is just "write bytes into a
pointer." It belongs at EL0.

This chapter finishes the move:

* A new `userspace/libgui/draw.{h,c}` library does the
  rasterisation in-process.
* Six apps were ported off the syscall path:
  `pixapp` (already used the mapping), `hellogui`, `launcher`,
  `taskbar`, `paint`, `desktop`.
* `fontd` (chapter 108b) became multi-client so the WM's
  in-kernel font connection and the apps' new client
  connections can coexist.
* The kernel `wm_fill_rect` / `wm_draw_text` / `wm_present`
  syscalls now return `-EBUSY` if invoked on a window that
  installed a mapping. Apps that mapped don't get to mix
  paths.
* `notify` is deliberately left on the legacy syscall path as
  the in-tree exhibit.

Three apps are deferred to chapter 108d: `notepad`, `gui_term`,
and `browser`. All three are resizable, and chapter 108a's
`wm_map_window` returns `-EINVAL` for resizable windows because
the resize-grip drag path `realloc`s `w->pixels`. The
remap-on-resize protocol is its own chapter.

## What the kernel still owns

After 108c the kernel WM keeps:

* The compositor (`wm_compose_screen` and friends — z-order,
  blending, the cursor).
* The framebuffer.
* `wm_damage` (chapter 108a): copy the user-VA pages back into
  `w->pixels` for a given rect, mark dirty, kick the
  compositor.
* `wm_present`, `wm_fill_rect`, `wm_draw_text` — but only for
  windows that did **not** call `gui_window_fb`. The first
  thing every one of those entry points now does is

  ```c
  if (w->user_pages_n != 0) return -EBUSY;
  ```

  So they remain live for `notify` (the canonical legacy
  caller) and any future kernel-side or unmapped-window
  caller, and they refuse to scribble on a mapped window
  behind its owner's back.

The kernel does **not** lose any syscalls in this chapter.
`SYS_GUI_PRESENT`, `SYS_GUI_FILL_RECT`, and `SYS_GUI_DRAW_TEXT`
keep their numbers (42, 43, 44). The whole `userspace/libc/syscall.h`
header is API-stable across the upgrade. The shrinkage is
behavioural, not numeric.

## Why we kept notify on the syscall path

User directive from the chapter discussion, recorded verbatim:

> Let's keep notify's rect drawn in the kernel as an explicit
> example of how to use the kernel-side rendering.

Reasons that make notify the right exhibit:

* **It's tiny.** Five `gui_fill_rect` calls plus two
  `gui_draw_text` calls. The full source is ~150 lines.
  Mapping a pixel buffer just to paint 360x80 pixels for 3
  seconds is overhead, not insight.
* **It's a real app, not a demo.** Every toast notification on
  the desktop goes through it. If `wm_fill_rect` ever broke,
  the regression would land in `test_notify.py` immediately.
* **It's short-lived.** A toast pops, sits for a few seconds,
  exits. The pmem cost of pages-and-mapping per window applies
  hardest to apps with notify's shape.
* **The contrast teaches.** Without a kernel-draw exhibit the
  chapter is a one-sided pitch.

`test_notify_legacy.py` (new in this chapter) is the codified
form of that directive: it boots, fires a toast, and asserts
the body BG (32, 40, 64) and accent (64, 128, 255) pixels are
still painted. The implicit claim is "notify is still going
through `wm_fill_rect`, and `wm_fill_rect` still works for
unmapped windows." If chapter 108c had broken either half, the
test would fail.

## The new library: `userspace/libgui/draw.h`

Header surface:

```c
struct gui_fb { uint32_t *pixels; uint32_t w, h, stride; int32_t id; };

static inline int gui_window_dirty(const struct gui_fb *fb,
                                   int32_t x, int32_t y,
                                   uint32_t w, uint32_t h);
static inline void draw_fill_rect(struct gui_fb *fb,
                                  int32_t x, int32_t y,
                                  uint32_t w, uint32_t h,
                                  uint32_t bgra);
static inline void draw_hline(struct gui_fb *fb, int32_t x, int32_t y,
                              uint32_t w, uint32_t bgra);
static inline void draw_vline(struct gui_fb *fb, int32_t x, int32_t y,
                              uint32_t h, uint32_t bgra);
static inline void draw_blit_bgra(struct gui_fb *fb,
                                  int32_t x, int32_t y,
                                  uint32_t w, uint32_t h,
                                  const uint32_t *src,
                                  uint32_t src_stride);

void draw_text(struct gui_fb *fb, int32_t x, int32_t y,
               const char *s, uint32_t fg_bgra, uint32_t bg_bgra,
               int transparent);
uint32_t draw_measure_text(const char *s);
```

The fill / hline / vline / blit primitives are header-only
inlines — they're just clipped writes into `fb->pixels`. The
text primitives are out-of-line in `draw.c` because they
maintain a per-process glyph cache and a persistent connection
to `fontd`.

### `struct gui_fb` gained an `id` field

Chapter 108a's `gui_window_fb` returned a struct with
`pixels`, `w`, `h`, `stride`. The new `id` field lets the
draw primitives call `gui_window_damage` without forcing every
caller to keep the window id alongside the `gui_fb` in a
separate variable. The kernel-side `SYS_GUI_MAP_WINDOW`
implementation already knew the id (it's the input); 108c just
stamps it into the returned struct.

Backward compatibility: `gui_fb` is constructed by the kernel,
never by the caller, so adding a field is wire-compatible. The
one place where a userspace caller built a `gui_fb` literal by
hand (`userspace/pixapp/pixapp.c`'s pre-108c init) leaves
`id = 0` implicitly, and the rewrite in this chapter replaces
it with the `gui_window_fb` call that fills the field.

### `draw.c`: fontd client + glyph cache + blender

The text path is where the interesting cross-process work
happens. Inside one process there is:

```c
struct draw_glyph {
    uint8_t  have, negative;        /* cache state */
    int16_t  left_bearing, top_bearing;
    uint16_t advance, bmp_w, bmp_h;
    uint8_t *alpha;                 /* malloc'd, owned by cache */
};

static struct draw_glyph g_cache[0x100];   /* ASCII */
static int   g_conn = -1;                  /* persistent srv_connect */
static int   g_down = 0;                   /* sticky disable on EOF */
static uint8_t g_reply_buf[4096];          /* one shared in-flight reply */
```

`fetch_glyph(cp, *out)` writes a `FONT_OP_GLYPH` request to
`g_conn`, reads the reply, copies the alpha bitmap into a
fresh malloc'd buffer. `get_glyph(cp)` is the cached fast path.
`draw_text` walks the string, per-glyph: fill background cell,
blend foreground per-pixel via the kernel's same
`(fg * a + dst * (255 - a)) / 255` formula mirrored to
userspace.

When `fontd` is unavailable (early boot, or `g_down == 1` after
a connection failure) `draw_text` advances by half a cell per
codepoint and `draw_measure_text` returns the same width
estimate. That keeps callers usable in degraded mode rather
than crashing.

### `gui_window_dirty` is the new damage call

It's a one-line wrapper over `gui_window_damage(fb->id, x, y, w, h)`.
The whole point is that callers stop having to keep `win_id`
and `fb` in sync. Every paint ends with one `gui_window_dirty`
covering the rect that was touched.

## fontd became multi-client

This was the biggest unscheduled scope creep in the chapter,
and worth recording in detail. Before 108c, `fontd` (chapter
108b) was a single-threaded `accept` → `serve_conn` → `close`
loop. The WM held a persistent connection for its in-kernel
title-bar rendering; that meant any second client could
`srv_connect` but their requests would queue behind the WM's
next request. Worse, the WM's connection was created at WM
startup and never closed, so the second client would block
indefinitely.

The chapter 108c fix in `userspace/fontd/fontd.c`:

* The whole accept loop now spawns a per-client worker via
  `thread_spawn_files(serve_thread, (void *)(long)cfd, -1)`,
  passing the accepted fd into the worker.
  `thread_spawn_files` does `CLONE_FILES` so the worker shares
  the parent's file table (chapter 93).
* Before each `accept`, the parent drains any zombie workers
  with `while (waitpid(-1, NULL, WNOHANG) > 0) {}`. WNOHANG
  keeps the accept latency at zero.
* The shared TTF state (`ttf_get_glyph`, `ttf_get_metrics`,
  `g_served`) is now protected by a single mutex,
  `g_face_lock`. ASCII is warmed at startup so steady-state
  rendering hits the cache and never takes the lock.
* The old file-scope `g_reply_buf` is gone; each worker
  allocates `uint8_t scratch[REPLY_BUF_BYTES]` on its stack
  (8 KB, sized to hold a 16 px glyph header + bitmap with
  comfortable margin). The smaller stack pressure matters
  because each worker runs on a 64 KB user stack.
* If `thread_spawn_files` fails (out of memory, etc.) the
  parent falls back to inline serve on its own connection,
  same as pre-108c behaviour.

A test in `scripts/test_fontd.py` rendered 220 intermediate-
alpha pixels in 16 px Sans (confirming AA glyphs) under the
new concurrent server. The chapter is also implicitly tested
by every per-app port — they all use fontd while the WM is
also using it.

## Per-app diffs

For each app, the same shape: replace static pixel buffers
with the mapped framebuffer, replace `gui_fill_rect` /
`gui_draw_text` / `gui_present` with `draw_*`, end each paint
with one `gui_window_dirty`.

### `hellogui` — port complete

Before: `static uint32_t pixels[640*960];` (614 400 bytes
static), six `gui_draw_text` calls per redraw, three
`gui_fill_rect` calls.

After:

* No static pixel buffer. The 614 KB savings on `.bss` is
  not negligible for a hello-world app.
* `paint_gradient(fb)` writes directly into the mapped fb.
* All text goes through `draw_text`, with `draw_measure_text`
  for the input-row alignment.
* One `gui_window_dirty` per paint.

Source: [userspace/hellogui/hellogui.c](../../../userspace/hellogui/hellogui.c).

### `launcher` — port complete

Before: ~5 `gui_fill_rect` calls per button (body + 4 border
edges), one `gui_draw_text`, repeated per redraw cycle.

After:

* `draw_button(i)` uses `draw_fill_rect` for the body and
  `draw_hline` / `draw_vline` for the four edges.
* The button label is centred via
  `draw_measure_text` + `draw_text`.
* `render()` does the bg fill, the button loop, and one
  whole-window `gui_window_dirty`.

Source: [userspace/launcher/launcher.c](../../../userspace/launcher/launcher.c).

### `taskbar` — port complete

Before: hot path. Every clock tick redrew the bar (60+ syscalls
per second on a desktop with 3 windows open).

After:

* `draw_cell()` uses `draw_fill_rect` + `draw_text` to render
  one taskbar slot.
* `render()` damages only `(0, 0)..(g_clock_x, BAR_H)` — the
  area excluding the clock — so the clock-tick redraw doesn't
  re-blit the whole bar.
* `draw_clock()` is the per-second hot path: draws then
  damages only the clock rect. Zero syscalls inside the tick
  except for the one damage call.

Source: [userspace/taskbar/taskbar.c](../../../userspace/taskbar/taskbar.c).

### `paint` — port complete

Before: brush stamps went through `gui_fill_rect` per pixel
group. Every mouse-drag motion produced N syscall round trips.

After:

* `clear_canvas`, `stamp_at`, `stamp`, `stamp_line` all take
  a `struct gui_fb *fb` and write directly through the
  mapping.
* `stamp()` damages just the brush rect (small).
* `stamp_line()` damages the bounding box of the line
  segment.
* Color swatch is one `draw_fill_rect` + one
  `gui_window_dirty`.

Source: [userspace/paint/paint.c](../../../userspace/paint/paint.c).

### `desktop` — port complete

This one had the biggest data movement to save. The desktop
process owns the wallpaper: a 1920x1080 BGRA blob on disk
(~8 MB) streamed in 16-row chunks. Before 108c, each chunk
read from disk was followed by a `gui_present` syscall that
memcpy'd the chunk into kernel-owned window memory; the user
buffer was reused for the next read.

After 108c the wallpaper window is mapped, so the streaming
loop does `read()` then `draw_blit_bgra(&fb, off_x, off_y+y,
...)` directly into the mapped pixels. The compositor sees
one `gui_window_dirty(0, 0, screen_w, screen_h)` at the end
of the load. The 8 MB of wallpaper data crosses the EL0/EL1
boundary exactly once — when the disk driver does its DMA
copy — instead of twice.

Source: [userspace/desktop/desktop.c](../../../userspace/desktop/desktop.c).

### `pixapp` — already on the mapped path

Chapter 108a's reference app. The chapter-108c work here was
cosmetic: replace the hand-rolled rect-fill loops with
`draw_fill_rect`, and replace the raw `gui_window_damage`
calls with `gui_window_dirty`. Same wire behaviour.

Source: [userspace/pixapp/pixapp.c](../../../userspace/pixapp/pixapp.c).

### `notify` — unchanged, the legacy-path exhibit

Source unchanged. Calls `gui_fill_rect` × 5 and
`gui_draw_text` × 2 per toast. After 108c, those calls land
in the kernel WM and rasterise into `w->pixels` exactly as
before. The reader who wants to know what kernel-side
drawing looks like reads `userspace/notify/notify.c`.

Source: [userspace/notify/notify.c](../../../userspace/notify/notify.c).

### Deferred to chapter 108d

| App | Reason |
|---|---|
| `notepad`  | Resizable. Resize-grip drag `realloc`s `w->pixels`, invalidating the mapping. |
| `gui_term` | Resizable, same reason. |
| `browser`  | Resizable, same reason. |

Chapter 108d will add a remap-on-resize protocol (the WM
signals the app, the app re-calls `gui_window_fb`, the WM
holds the old mapping live until ack) and then port all three
to the mapped path in one go.

## The `-EBUSY` enforcement

`kernel/core/wm.c` got one new error code and three identical
guards:

```c
#define EBUSY  16

long wm_present(uint64_t pid, int32_t id, ...) {
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    if (w->user_pages_n != 0) return -EBUSY;
    ...
}

long wm_fill_rect(uint64_t pid, int32_t id, ...) {
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    if (w->user_pages_n != 0) return -EBUSY;
    ...
}

long wm_draw_text(uint64_t pid, int32_t id, ...) {
    struct wm_window *w = win_owned_by(id, pid);
    if (!w) return -EPERM;
    if (w->user_pages_n != 0) return -EBUSY;
    ...
}
```

`w->user_pages_n` is the chapter-108a field that's nonzero
iff a userspace mapping is installed for the window. The
guard fires before any pixel writes, so the syscall is a
no-op on the kernel side when refused.

The contract:

* An app that called `gui_window_fb` owns its window's
  pixels outright. Any kernel-side write would race with
  the app's direct writes and produce torn output the next
  time the compositor ran. `-EBUSY` makes the violation
  visible at the syscall edge instead of as visual
  corruption.
* An app that never calls `gui_window_fb` (notify, plus
  anything pre-108c that hasn't been ported) keeps the
  full kernel-rasterisation path. No regression.

There is no flag to opt out of the enforcement. Either map
the window and own the pixels, or don't map it and let the
kernel paint.

## Tests

Two new scripts in `scripts/`, both kept under the regular
sweep:

* [`scripts/test_busy_on_mix.py`](../../../scripts/test_busy_on_mix.py) —
  runs `/bin/mixtest`, a binary added in this chapter at
  [`userspace/mixtest/mixtest.c`](../../../userspace/mixtest/mixtest.c).
  mixtest does two things: it creates an unmapped window and
  asserts `gui_fill_rect` / `gui_draw_text` / `gui_present`
  all return 0 on it (positive case for the legacy path);
  then it creates a mapped window and asserts the same three
  calls return `-EBUSY`. The harness greps for
  `[mixtest] all checks passed` on the serial console.
* [`scripts/test_notify_legacy.py`](../../../scripts/test_notify_legacy.py) —
  a near-twin of `scripts/test_notify.py` framed as the
  legacy-path regression. Boots, fires `/bin/notify
  Chapter108c`, screenshots, asserts the body BG and accent
  pixels are still where they should be. The implicit claim
  it codifies: chapter 108c did not delete or break the
  kernel-draw path that `notify` depends on.

Every pre-existing test
(`test_launcher`, `test_taskbar`, `test_clock`,
`test_paint_drag`, `test_wallpaper`, `test_boot_to_desktop`,
`test_notify`, `test_wm`, `test_fontd`, `test_truetype`)
still passes against the ported binaries with no script
changes. Pixel comparison against the pre-108c baseline is
the regression bar; behavioural equivalence is the
correctness bar.

## Design decisions, recorded

### Header-only fill, out-of-line text

The fill / blit / hline / vline primitives are header-only
inlines because they have no per-process state: they're
clipped writes. Inlining them into each call site costs ~30
bytes per call but saves the function-call setup. The text
primitives have a per-process glyph cache and a persistent
`fontd` connection, so they must be out-of-line — every
inlined copy of `draw_text` would carry its own duplicate
`g_cache`, `g_conn`, and reply scratch.

### `draw.c` private memset

GCC emits an implicit `memset` call for `static struct
draw_glyph g_cache[0x100]`'s zero-init. Userspace doesn't
have a libc memset for freestanding builds; pre-existing
trap, recorded in `/memories/freestanding-c-memset-trap.md`.
`draw.c` defines a tiny private `static memset` to satisfy
the implicit call.

### `gui_window_dirty` takes the fb, not the id

Apps that map a window typically forget the id and pass the
`gui_fb` everywhere. The wrapper matches that usage. The
chapter-108a primitive `gui_window_damage(id, ...)` is still
exported; the wrapper is sugar.

### Sticky `g_down` on fontd EOF

If the persistent fontd connection ever sees EOF or a short
read, `draw.c` sets `g_down = 1` and stops retrying.
`draw_text` and `draw_measure_text` fall back to half-cell
estimates. Without the sticky flag, every subsequent
`draw_text` call would attempt a fresh connect, and a missing
fontd would turn into a per-keystroke connect storm.

### `MAX_BITMAP_BYTES` in fontd

The per-worker scratch is 8 KB. A 16 px AA glyph is at most
~32x32 = 1 024 bytes; the header is small; 8 KB has a
comfortable margin. The worker explicitly checks and falls
back to metrics-only on oversized glyphs (a future scaled
font might trigger this). Without the cap the worker could
recurse on a malformed font and blow its stack.

## Applied to

* **Apps modified:** `hellogui`, `launcher`, `taskbar`,
  `paint`, `desktop`, `pixapp` (cosmetic — already used
  the mapping).
* **Apps unchanged on purpose:** `notify` (legacy-syscall
  exhibit).
* **Apps deferred to chapter 108d:** `notepad`, `gui_term`,
  `browser` (all resizable; need 108d's remap-on-resize).
* **New library:** [`userspace/libgui/draw.h`](../../../userspace/libgui/draw.h) +
  [`userspace/libgui/draw.c`](../../../userspace/libgui/draw.c).
* **Library extended:** [`userspace/libc/syscall.h`](../../../userspace/libc/syscall.h) —
  `struct gui_fb` gained `id`; `gui_window_fb` stamps it.
* **Service extended:** [`userspace/fontd/fontd.c`](../../../userspace/fontd/fontd.c) —
  multi-client via per-worker `thread_spawn_files` + mutex
  around TTF state.
* **Kernel extended:** [`kernel/core/wm.c`](../../../kernel/core/wm.c) —
  `-EBUSY` guard at the top of `wm_present`, `wm_fill_rect`,
  `wm_draw_text` when the window has a mapping installed.
  `EBUSY = 16` added to the local errno mirror.
* **Tests added:**
  [`scripts/test_busy_on_mix.py`](../../../scripts/test_busy_on_mix.py),
  [`scripts/test_notify_legacy.py`](../../../scripts/test_notify_legacy.py).
* **Test binary added:**
  [`userspace/mixtest/mixtest.c`](../../../userspace/mixtest/mixtest.c) —
  the in-process assertion engine that
  `test_busy_on_mix.py` drives.

## What this unlocks

* **Chapter 108d — resize coherence for mapped windows.** Now
  that the rest of the GUI has shown the per-app ergonomics
  are good, the case for the WM-app remap handshake is
  concrete. notepad / gui_term / browser are the targets.
* **Userspace UI primitives that the kernel doesn't know
  about.** SVG, video, off-screen canvases, alpha-blended
  shadows, custom shader-style effects — all of these are
  now userspace library code, not syscall negotiations with
  the kernel.
* **A measurable reduction in EL0/EL1 traffic per repaint.**
  The taskbar clock tick was the worst pre-108c offender
  (5+ syscalls per second per visible app). After the port
  it's one `gui_window_dirty` per second.
* **The first chapter where the kernel's syscall surface
  shrinks behaviourally without shrinking numerically.**
  Two of the three render syscalls (`SYS_GUI_PRESENT`,
  `SYS_GUI_FILL_RECT`) now refuse the most common usage and
  serve only a single in-tree caller (`notify`). Future
  chapters can revisit whether to keep them at all.

## Open question for the future

The kernel still does title-bar rendering through `wm_font.c`,
which talks to fontd over its own persistent connection. After
108c that's the WM's only fontd client; every userspace app
has its own. If a future chapter lets the WM render decorations
client-side too (e.g. via a 108d-style "decoration app" that
the WM merely composes), `wm_font.c` and the kernel's fontd
client could go away entirely. That is not chapter 108c's
fight.

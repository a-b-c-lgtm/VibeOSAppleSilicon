# Chapter 108a — Userspace access to window pixel buffers

**Status:** Stub. Tracking milestone 90d.

Every window's pixel buffer (`w->pixels`, allocated by
the WM) lives in kernel memory. The only way userspace
can put a pixel on screen today is via syscalls — and
the WM is the one that does the work. `SYS_GUI_FILL_RECT`,
`SYS_GUI_DRAW_TEXT`, `SYS_GUI_BLIT_BGRA` — every visual
primitive is implemented in `kernel/core/wm.c`, blitting
into a kernel-side buffer that userspace can never see.

That has been fine for everything chapters 46–106 ship.
But it has a structural cost we are about to start paying:
**anything graphical that wants to live in userspace —
a TTF rasteriser (chapter 108b), a vector graphics
library, a hardware-accelerated path, a video decoder —
needs a way to write bytes into a window without
round-tripping every pixel through a syscall.**

This chapter introduces that path. After it lands, any
userspace program can `mmap` its own window's pixel
buffer and treat it as a flat BGRA framebuffer. No more
"one syscall per primitive" — the app rasterises locally
and tells the WM "redraw rect (x,y,w,h)" only once it's
done.

## Why this isn't just "expose `w->pixels` via mmap"

It almost is, but three subtleties prevent a one-line
fix:

1. **Page alignment.** `w->pixels` is allocated by
   `kmalloc` and isn't page-aligned; mapping a partial
   page to userspace would either leak adjacent kernel
   data (a security bug) or require a copy. Window
   buffers need to move to a page-aligned allocator
   (or be allocated as anonymous pages with the WM
   keeping the kernel-side mapping).

2. **Resize coherence.** Today the WM `kfree`s and
   `kmalloc`s `w->pixels` on `SYS_GUI_RESIZE`. If
   userspace has the old buffer mmapped, the new
   buffer has to land at the same VA — otherwise
   every redraw after a resize touches stale memory.
   The fix is a stable per-window VA region with
   different physical pages mapped underneath it
   (essentially what `mremap` does on Linux).

3. **Write-flush semantics.** The WM composites all
   windows into the framebuffer on a 60 Hz tick. If
   userspace writes into its window buffer at frame
   N+0.5, the WM needs to know to pull from it on
   the next composite. We add a single
   `SYS_GUI_PRESENT(window_id, rect)` syscall that
   says "I'm done writing this rect; please composite
   it on the next pass." Without that the WM would
   either composite torn frames or have to
   `mprotect`-trap every write.

The chapter is mostly about getting these three
details right; the actual "expose the bytes" is the
easy part.

## What this chapter adds

* **Page-aligned window buffer allocator.** Replace
  the `kmalloc(w*h*4)` in `wm_window_create` with
  `pmem_alloc_pages_aligned(N)` for the appropriate
  `N`. The kernel mapping stays so the WM can still
  composite.
* **`SYS_GUI_MAP_WINDOW(window_id)`** — returns a
  user VA pointing at the same physical pages, in the
  caller's address space. Lazily faulted via the same
  vma machinery chapter 90 introduced.
* **`SYS_GUI_PRESENT(window_id, x, y, w, h)`** —
  marks a region of the window dirty. The WM's next
  composite pass picks it up.
* **`SYS_GUI_UNMAP_WINDOW(window_id)`** — symmetric
  cleanup. Implicit on exit.
* **`libgui/window_fb.h`** — `gui_window_fb()` returns
  a `struct gui_fb` (base, stride, w, h) and
  `gui_window_present(rect)` is a one-line wrapper.
  Apps that want pixel-level control use this; apps
  that just want primitives keep calling
  `SYS_GUI_FILL_RECT` etc.

## Prerequisites

* Chapter 90 — mmap + the vma table. We need
  user-VA → physical-page mapping with proper page
  tracking on AS teardown.
* Chapter 91 — userspace threads. The "rasterise in
  the background, present when done" idiom benefits
  from a worker thread, though it isn't required.
* Chapter 73 — fork. Whatever we map has to survive
  fork the same way regular anon pages do (chapter
  75 COW probably *doesn't* apply — each forked
  child should get a fresh blank window, not a COW
  share of the parent's pixels).
* Chapter 107 — IPC. Not strictly needed for the
  mmap part, but the next chapter (108b) uses
  108a + 107 together to build the font server.

## Design decisions

### Mmap, not "DMA into a userspace ring buffer"

A ring-buffer-of-blits would also work — the GPU
driver model. We pick mmap because (a) it makes
the existing software-rendered code in browsers /
notepad / paint trivially portable: replace
`fill_rect` calls with direct pixel writes; (b) it
matches what every real OS does for software
windows (X SHM, Wayland wl_shm, Win32 DIB sections);
(c) we already have the vma machinery from chapter
90.

### Present, not "auto-detect dirty pages"

Auto-detection (e.g. mprotect-trap-on-write) would
spare userspace one syscall per redraw at the cost
of a fault per pixel-rect-of-pages. The math is
clear: a couple of `SYS_GUI_PRESENT` calls per frame
is cheaper than even one fault. Linux's
fbdev-shadow path made the same call.

### One mapping per window, no double-buffer yet

Apps that want double-buffering can `malloc` their
own back buffer and `memcpy` into the window
buffer on present. Native double-buffering (the
WM owns front/back and atomically swaps) is a
later chapter once we have an actual tear measured
on the screen.

## Test plan

* A new test app `userspace/pixapp/pixapp.c` opens a
  300×200 window, maps it, draws a smooth red→blue
  gradient by writing pixels directly, calls
  `gui_window_present(0,0,300,200)`, and exits.
* `scripts/test_pixapp.py` boots, runs the app,
  screendumps, samples three pixels at expected
  gradient positions and asserts the colours match
  within tolerance.
* Regression: the existing GUI tests
  (`test_paint_drag`, `test_notepad`, `test_browser_*`)
  all keep passing — proving the old
  syscall-per-primitive path still works for apps
  that don't opt into the mmap path.

## What you'll learn

* Why "just mmap it" is three subtleties, not one:
  page alignment, resize coherence, present
  semantics.
* The exact reason Linux's X SHM was a watershed
  in 1995 — it's the same reason we ship it here.
* How `SYS_GUI_PRESENT`'s "explicit invalidate"
  model maps onto every real compositor (Wayland's
  `wl_surface.damage`, X's `Damage` extension,
  Quartz's `setNeedsDisplay`).

## What this unlocks

* Chapter 108b — userspace font rendering, the
  motivating use case. The font server rasterises
  glyphs into shared pages; clients
  `gui_window_present` the rect after blitting.
* Any future "rich-content" rendering: SVG, video
  frames, off-screen canvases, plotting libraries.
* A path to GPU acceleration: a future
  `SYS_GUI_MAP_WINDOW_GPU` returns a writeable GPU
  surface in user VA. The `present` syscall stays
  the same.

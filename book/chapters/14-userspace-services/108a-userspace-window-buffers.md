# Chapter 108a — Userspace access to window pixel buffers

**Status:** Done. Tracking milestone 90d.

Every window's pixel buffer (`w->pixels`, allocated by
the WM) lives in kernel memory. Up until this chapter the
only way userspace could put a pixel on screen was via
syscalls — and the WM was the one that did the work.
`SYS_GUI_FILL_RECT`, `SYS_GUI_DRAW_TEXT`,
`SYS_GUI_BLIT_BGRA` — every visual primitive is
implemented in `kernel/core/wm.c`, blitting into a
kernel-side buffer that userspace can't see.

That has been fine for everything chapters 46–106 ship.
But it has a structural cost we're about to start paying:
**anything graphical that wants to live in userspace —
a TTF rasteriser (chapter 108b), a vector graphics
library, a hardware-accelerated path, a video decoder —
needs a way to write bytes into a window without
round-tripping every pixel through a syscall.**

This chapter introduces that path. After it lands, any
userspace program can ask the WM to map its window's
pixel buffer into the address space and treat it as a
flat BGRA framebuffer. No more "one syscall per
primitive" — the app rasterises locally and tells the
WM "redraw rect (x,y,w,h)" only once it's done.

## Why this isn't just "expose `w->pixels` via mmap"

It almost is, but three subtleties make a one-line
fix wrong:

1. **Page alignment.** `w->pixels` is allocated by
   `kmalloc` and isn't page-aligned; mapping a
   partial page to userspace would leak adjacent
   kernel data (a security bug) or require a copy on
   every access.
2. **Resize coherence.** The WM `kfree`s and
   `kmalloc`s `w->pixels` on `SYS_GUI_RESIZE`. If
   userspace has the old buffer mapped, the new
   buffer has to land at the same VA — otherwise
   every redraw after a resize touches stale memory.
3. **Write-flush semantics.** The WM composites all
   windows into the framebuffer on a 60 Hz tick. If
   userspace writes into its window buffer at frame
   N+0.5, the WM needs to know to pull from it on
   the next composite — without that we'd either
   composite torn frames or have to `mprotect`-trap
   every write.

The implementation we shipped sidesteps all three by
keeping `w->pixels` exactly as it is (kheap, contiguous,
the compositor's authoritative copy) and giving each
mapping a parallel page-aligned set of pmem frames in
the caller's address space. This is the same trick X
SHM (the MIT-SHM extension that landed in 1995) plays:
a userspace-visible shadow buffer + a kernel-mediated
"this rect is dirty" call. See the design-decisions
section below for why we converged on it.

## What this chapter adds

* **A new software-only descriptor bit
  `DESC_SW_WM_WINDOW` (bit 58)** marking
  WM-owned pages. The address-space layer skips
  these on teardown (the WM, not the AS, owns the
  physical frames) and on fork (each child gets a
  fresh blank window, not a COW share of the
  parent's pixels). See
  [kernel/arch/address_space.h](../../../kernel/arch/address_space.h)
  and
  [kernel/arch/address_space.c](../../../kernel/arch/address_space.c).
* **`address_space_install_wm_window` /
  `address_space_uninstall_wm_window`** — two new
  AS helpers that map / unmap a run of pmem frames
  into the caller's VA with the right permission
  bits and the new SW tag.
* **`SYS_GUI_MAP_WINDOW`** (#53) — returns a user
  VA pointing at a fresh page-aligned BGRA buffer
  seeded with the current `w->pixels` content, plus
  stride and dimensions. The mapping persists
  across syscalls and survives shutdown of any
  other GUI activity.
* **`SYS_GUI_DAMAGE`** (#55) — marks a rect of a
  mapped window dirty. The WM copies that rect from
  the user-visible pages back into `w->pixels` and
  triggers a recompose. The name matches Wayland's
  `wl_surface.damage` and X11's `Damage` extension.
* **`SYS_GUI_UNMAP_WINDOW`** (#54) — symmetric
  cleanup. Implicit on `gui_destroy_window` and
  process exit.
* **`libc/syscall.h`** — `gui_window_fb()` returns a
  `struct gui_fb { uint8_t *pixels; uint32_t stride;
  uint32_t w, h; }`. `gui_window_damage(rect)` is a
  one-line wrapper. Apps that want pixel-level
  control use these; apps that just want primitives
  keep calling `gui_fill_rect` etc. unchanged.

## How the mapping actually works

The flow on the kernel side, when an app calls
`gui_window_fb(win, &fb)`:

```
EL0:  gui_window_fb(win, &fb)
                |
                v
EL1:  sys_gui_map_window
                |
                v
        wm_map_window(pid, id, &va, &stride, &w, &h)
                |
                +--> own check, RESIZABLE check
                |
                +--> alloc N = ceil(w*h*4 / 4096) frames
                |    via pmem_alloc_page() each
                |
                +--> seed each frame from w->pixels
                |    (so the app sees the current screen,
                |    not garbage)
                |
                +--> address_space_install_wm_window(
                |        thread.as, frames[], N, &va)
                |    walks L3 entries; writes
                |    ATTR_NORMAL | DESC_AF | DESC_AP_RW_EL0
                |    | DESC_PXN | DESC_UXN
                |    | DESC_SW_WM_WINDOW | DESC_VALID
                |
                +--> stash bookkeeping on the wm_window:
                |       user_pages_pa[], user_pages_n,
                |       user_va, user_as
                |
                +--> copy_to_user the four out params
```

And on `gui_window_damage(win, x, y, rw, rh)`:

```
EL0:  gui_window_damage(win, x, y, rw, rh)
                |
                v
EL1:  sys_gui_damage -> wm_damage
                |
                +--> clip (x, y, rw, rh) to (w->w, w->h)
                |
                +--> for each row in [y, y+rh):
                |       byte offset into flat payload
                |       = row * stride + x*4
                |     translate to (page_idx, in_page_off)
                |     memcpy chunk-by-chunk from
                |       user_pages_pa[page_idx] (kernel
                |       identity-mapped, so PA == VA
                |       on the kernel side)
                |     into w->pixels + offset
                |
                +--> compose_all()
```

Two things to notice:

* **`w->pixels` stays as the authoritative buffer.**
  Every existing render site (titles, decorations,
  the compositor, screenshot capture, `gui_present`,
  `gui_fill_rect`) keeps writing into it. Zero
  refactor risk to a large surface of existing code.
* **No `copy_from_user` in `wm_damage`.** The
  WM-owned frames live in the kernel identity map
  (a PA returned by `pmem_alloc_page` is also a
  valid kernel VA on this architecture), so the
  damage copy is just two `memcpy`s and a page-walk.

## Why the new SW bit matters

`DESC_SW_WM_WINDOW` slot 58 sits next to the other
chapter-bit SW tags (COW=55, PAGECACHE=56, GUARD=57)
and gets read by two AS code paths:

* `teardown_user_range` — the per-AS unmap helper
  that runs on `address_space_destroy` and inside
  `sys_munmap`. Without the skip, AS teardown
  would `pmem_free_page` our WM frames out from
  under the WM, then the next compositor pass
  would double-free or scribble random memory.
* `address_space_clone` and `address_space_clone_cow`
  (eager + COW fork paths). Without the skip,
  a forked child would inherit the parent's
  WM-window mapping. That's wrong on two counts:
  the child can't damage a window it doesn't own
  (see `win_owned_by` — `wm_damage` returns
  `-EPERM`), and even if it could, two processes
  writing to the same pages with no synchronisation
  is a classic data race.

The skip is one line in each of three places. The
choice to use a SW bit rather than a side table
(`struct vma` with a type field, say) is a
deliberate echo of how COW and the page cache
already work — keep the metadata on the L3
descriptor where the page walk already touches it,
add no new lookup cost.

## Design decisions

### Map, not "DMA into a userspace ring buffer"

A ring-buffer-of-blits would also work — the GPU
driver model. We picked mapped pages because (a) it
makes the existing software-rendered code in apps
trivially portable: replace `fill_rect` calls with
direct pixel writes; (b) it matches what every real
OS does for software windows (X SHM, Wayland wl_shm,
Win32 DIB sections); (c) we already have the AS
infrastructure to make it cheap.

### Damage, not "auto-detect dirty pages"

Auto-detection (e.g. `mprotect`-trap-on-write) would
spare userspace one syscall per redraw at the cost
of a fault per first-touched page. The math is
clear: a couple of `SYS_GUI_DAMAGE` calls per frame
are cheaper than even one fault. Linux's
fbdev-shadow path made the same call.

### Parallel buffer, not "expose `w->pixels` directly"

Tempting because it would avoid the seed-copy at
map time and the rect-copy at damage time. We
rejected it because (a) `w->pixels` isn't page-
aligned, (b) every existing call site that touches
`w->pixels` would have to be audited for atomicity
(the compositor reads while user code writes), and
(c) resize would invalidate the user mapping with
no clean recovery. The parallel-buffer design pays
one memcpy per damage but is correct by
construction.

### One mapping per window, RESIZABLE rejected

Apps that want double-buffering can `malloc` their
own back buffer and `memcpy` into the mapped
buffer on present. Apps that need a resizable
window can't yet map: `wm_map_window` returns
`-EINVAL` if the window has `GUI_WIN_FLAG_RESIZABLE`
set. The reason is that the resize-grip drag path
inside the WM `realloc`s `w->pixels`, and the
mapped pages would have no story for the new
dimensions. Lifting this restriction is the work of
a future chapter; until then, every existing
RESIZABLE window (notepad today) sticks with the
per-primitive syscalls it already uses.

## Prerequisites

* Chapter 90 — mmap + the bump-pointer allocator
  we reuse for the user VA range.
* Chapter 75 — COW. The skip-on-fork mechanism
  here echoes the one chapter 75 added for COW.
* Chapter 73 — fork's eager-copy path, which we
  also teach to skip the new bit.

## Test plan

* A new userspace app
  [userspace/pixapp/pixapp.c](../../../userspace/pixapp/pixapp.c)
  opens a 300×200 window, maps its pixel buffer
  via `gui_window_fb`, draws a smooth red→blue
  gradient by writing BGRA bytes directly, calls
  `gui_window_damage(0,0,300,200)`, then waits for
  a CLOSE event in a tight `gui_poll_event` loop.
* [scripts/test_pixapp.py](../../../scripts/test_pixapp.py)
  boots, runs the app, screendumps via QMP, and
  samples three pixels on the gradient at row 100:
  column 5 must be red-dominant, column 150 must
  be magenta, column 295 must be blue-dominant.
* Regression: every existing GUI test
  (`test_paint_drag`, `test_notepad`,
  `test_browser_*`, `test_gui_term`, `test_taskbar`,
  …) keeps passing — proving the
  syscall-per-primitive path still works
  unchanged for the apps that don't opt in.

## What you'll learn

* Why "just expose the bytes" is three subtleties,
  not one: page alignment, resize coherence,
  damage semantics.
* The exact reason X SHM was a watershed in 1995 —
  it's the same reason we shipped it here.
* How an "explicit damage" model maps onto every
  real compositor (Wayland's `wl_surface.damage`,
  X11's `Damage` extension, Quartz's
  `setNeedsDisplay`).
* How a single descriptor SW bit can keep two
  cross-cutting AS code paths (teardown + fork)
  honest with no extra side-table lookups.

## Applied to

* **New app:**
  [userspace/pixapp/pixapp.c](../../../userspace/pixapp/pixapp.c)
  — 300×200 window painting a gradient with zero
  syscalls in the hot loop. The chapter's
  demonstrate-the-feature app per the
  "apps must use the features" rule.
* **New test:**
  [scripts/test_pixapp.py](../../../scripts/test_pixapp.py)
  — boots, runs pixapp, samples three pixels off
  the gradient and asserts the colours.
* **New debug helper:**
  [scripts/_dbg_pixapp.py](../../../scripts/_dbg_pixapp.py)
  — full serial dump for chapter-108a bring-up.
* **Existing apps unchanged.** No existing GUI app
  was rewritten in this chapter on purpose: the
  syscall-per-primitive path is correct for what
  hellogui / paint / notepad / browser do today.
  Chapter 108b (userspace font rendering) is the
  first chapter that *requires* this feature and
  will rewrite the text-heavy paths to use it.

## What this unlocks

* **Chapter 108b — userspace font rendering**, the
  motivating use case. The font server rasterises
  glyphs into its own buffer; clients copy the
  glyph into the mapped window with one `memcpy`
  per glyph and one `gui_window_damage` per text
  run.
* Any future "rich-content" rendering: SVG, video
  frames, off-screen canvases, plotting libraries.
* A path to GPU acceleration: a future
  `SYS_GUI_MAP_WINDOW_GPU` could return a writeable
  GPU surface in user VA. The `damage` syscall
  stays the same.
* Resizable mapped windows (a future "108c"): the
  WM would need a swap protocol — old mapping
  stays valid until the app acknowledges the new
  one. Until then, RESIZABLE windows can't map.

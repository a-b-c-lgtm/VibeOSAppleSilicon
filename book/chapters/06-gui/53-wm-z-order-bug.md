# Chapter 53 — A WM rendering bug, surfaced by the launcher

> **Milestone in this chapter:** 45 — z-order correctness in the
> kernel compositor.
> **Code referenced:**
> - [kernel/core/wm.c](../../../kernel/core/wm.c) (`wm_create_window`,
>   `wm_pointer_button`, the historical `compose_all`)
> - [scripts/test_launcher.py](../../../scripts/test_launcher.py)
>
> **At the end of this chapter** you will have a window manager whose
> painter is robust to non-contiguous z-stack values, a z-allocator
> that is monotonic by construction, and a launcher smoke test that
> asserts on pixels rather than log lines.
>
> **Heads-up:** this chapter teaches the kernel-side painter as it
> existed at milestone 45. Chapter 108d later moves compositing into
> userspace and retires `compose_all` to a no-op; the historical
> source lines this chapter quotes still survive in
> [kernel/core/wm.c](../../../kernel/core/wm.c) for reference, but
> the live paint path now belongs to wsd.

## Why the previous chapter's success was incomplete

Chapter 52 wired the launcher to spawn a new `gui_term` whenever the
user clicked one of its buttons, and ended with a passing test. That
test counted `[wm] window created` lines on the serial log — so it
proved the child process started and that the WM accepted the new
`gui_create_window`. It did *not* prove anything about pixels.

A screendump told a different story: only the launcher was visible.
The freshly created `gui_term` window had a kernel log line confirming
its existence, but it sat nowhere on screen. This chapter is the
fix, packaged as milestone 45.

## Symptom

After running the launcher smoke test:

```
$ python3 scripts/test_launcher.py
PASS: shell ready
PASS: launcher window opened
PASS: click spawned a new window (window-creates 1 -> 2)
   ↑ but a screendump shows only the launcher
```

The serial log was unambiguous:

```
[wm] window created id=0x1 pid=0x8 size=0x2d0x0x1b8
```

A 720×440 window had been created, owned by pid 8, with id 1. No
errors followed. But the framebuffer showed only the 240×180 launcher
in the upper-left, and the navy wallpaper everywhere else.

## Ruling out the obvious — cascade placement

If the new window were placed at the same coordinates as the
launcher, it would be hidden behind it. Open
[kernel/core/wm.c](../../../kernel/core/wm.c) and read the cascade
arithmetic in `wm_create_window`:

```c
uint32_t step = (id % 8) * 32;
win->x = 80 + step;
win->y = 60 + step;
```

Launcher (id=0) sits at (80, 60); `gui_term` (id=1) lands at
(112, 92). At 720×440 the new window extends far past the launcher's
right edge and would peek out at x ≥ 320. It doesn't. Cascade isn't
the problem.

## The real bug — painter's algorithm vs the z allocator

The historical kernel-side compositor `compose_all` walked windows in
ascending `z` and blitted them in order:

```c
for (uint32_t pass = 0; pass < WM_MAX_WINDOWS; pass++) {
    int32_t  pick_id = -1;
    uint32_t pick_z  = 0;
    for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_wins[i].in_use) continue;
        if (g_wins[i].z <= pass) continue;       /* "already painted" */
        if (pick_id < 0 || g_wins[i].z < pick_z) {
            pick_id = i;
            pick_z  = g_wins[i].z;
        }
    }
    if (pick_id < 0) break;
    blit_window(&g_wins[pick_id]);
}
```

The trick is `if (g_wins[i].z <= pass) continue;`. The pass counter
is being treated as a stand-in for "highest z already drawn", on the
assumption that z values will always be a contiguous run
`1, 2, 3, …`. As long as that holds, each pass picks exactly one
window, increments the implied watermark by one, and the loop drains.

**That assumption is false.** Two operations assign `z`:

| Site                     | Operation         | Effect          |
|--------------------------|-------------------|-----------------|
| `wm_create_window`       | `win->z = g_next_z++;` | post-increment |
| left-down focus handler  | `w->z = ++g_next_z;`   | pre-increment  |

Trace the launcher click:

| Step                          | `g_next_z` (before) | assignment        | result                               |
|-------------------------------|---------------------|-------------------|--------------------------------------|
| boot                          | 1                   | —                 | `g_next_z` = 1                       |
| launcher created              | 1                   | `win->z = 1++`    | `launcher.z = 1`, `g_next_z` = 2     |
| user clicks launcher button   | 2                   | `w->z = ++2`      | `launcher.z = 3`, `g_next_z` = 3     |
| launcher spawns `gui_term`    | 3                   | `win->z = 3++`    | `gui_term.z = 3`, `g_next_z` = 4     |

The painter then runs with `launcher.z = 3` and `gui_term.z = 3`:

- **Pass 0** — both windows have `z > 0` and both have `z = 3`. The
  picker tie-breaks on first-found-with-strictly-smaller-z; both are
  equal, so it takes the first window scanned (launcher, `i = 0`).
  Blits launcher.
- **Pass 1** — same. Launcher picked again, blitted over itself.
- **Pass 2** — same.
- **Pass 3** — `g_wins[i].z <= 3` now skips both windows. `pick_id`
  stays at -1. The loop breaks.

`gui_term` is never painted. Bug confirmed.

## Two fixes, layered

### Fix 1 — make the z allocator consistent

Both call sites should use `++g_next_z`, so every assignment produces
a strictly-increasing, unique value:

```diff
-    win->z = g_next_z++;
+    win->z = ++g_next_z;
```

After the change, the trace becomes `launcher.z = 2`, after-click
`= 3`, `gui_term.z = 4` — all distinct. The painter happens to
handle this correctly because `launcher.z = 3 ≤ pass = 3` finally
kicks the launcher out and `gui_term` (z = 4) gets drawn on pass 3.

Apply the fix and you can confirm in the live source:

```c
// kernel/core/wm.c, wm_create_window
win->z          = ++g_next_z;
```

Every other z-assignment site (`w->z = …` inside the raise/focus
handlers) already used pre-increment; only `wm_create_window` was
the outlier.

### Fix 2 — make the painter robust to sparse z values

Fix 1 is the immediate unblock, but the painter is still
**structurally wrong**. After ~16 raises, z values fan out beyond
`WM_MAX_WINDOWS`, the outer loop exhausts its passes before reaching
them, and windows silently disappear again. Track "already emitted"
with a bitmask, decoupling z from the loop counter:

```c
uint32_t painted = 0;  /* bit i set => g_wins[i] already drawn */
for (uint32_t pass = 0; pass < WM_MAX_WINDOWS; pass++) {
    int32_t  pick_id = -1;
    uint32_t pick_z  = 0;
    for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_wins[i].in_use) continue;
        if (painted & (1u << i)) continue;
        if (pick_id < 0 || g_wins[i].z < pick_z) {
            pick_id = i;
            pick_z  = g_wins[i].z;
        }
    }
    if (pick_id < 0) break;
    blit_window(&g_wins[pick_id]);
    painted |= (1u << pick_id);
}
```

Now `z` is purely a sort key. The outer loop runs at most N times
(N = `WM_MAX_WINDOWS` = 16) and emits each in-use window exactly
once, in ascending z. You keep the painter's algorithm without
over-fitting to dense z values.

## Verifying with pixels

Replace the log-counting assertion in
[scripts/test_launcher.py](../../../scripts/test_launcher.py) with
one that reads the framebuffer dump:

```python
ppm = read_ppm(DUMP_PATH)
title_pix    = pixel_at(ppm, 500, 100)   # expected: gui_term title bar
wallpaper_px = pixel_at(ppm, 1000, 700)  # expected: navy
assert title_pix != wallpaper_px
r, g, b = title_pix
assert b > r and b > 80   # the WM title-bar blue
```

Expected output:

```
PASS: spawned window rendered (title-bar pixel at (500,100) = (64, 96, 192))
MILESTONE 45: ALL TESTS PASSED
```

`(64, 96, 192)` is the WM's deliberately-chosen title-bar blue. The
spawned window is now visibly composited on top of the launcher,
cascade and all.

## What this teaches

It's tempting to stop after fix 1: the test passes, the screendump
looks right, and the chapter could end with a cute off-by-one
anecdote. The painter, however, would still be a time bomb — invisible
until enough windows opened.

The general lesson is familiar: when one piece of state encodes
two unrelated invariants — here, "position in the z stack" *and*
"loop iteration count" — the bug lives in the encoding, not in
any single call site. Decouple the invariants. The post-fix
code is barely longer and admits no parameter for which it
silently misbehaves.

## What this unlocks

With reliable z-ordered compositing, the next chapter (54) auto-spawns
the launcher at boot and paints a gradient wallpaper behind it,
giving the system a real boot-to-desktop experience. Every later GUI
chapter — taskbar, clock, toast notifications, minimise/restore —
relies on the painter being correct for arbitrary z values.

> **Forward note:** chapter 108d retires this code entirely. When the
> compositor moves to userspace, `compose_all` becomes a no-op and
> wsd does the equivalent z-sorted draw with the same algorithm
> shape, off the kernel critical path.

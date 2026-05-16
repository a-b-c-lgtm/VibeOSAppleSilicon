# Chapter 53 — A WM rendering bug, surfaced by the launcher

Milestone 44 ended on a slightly evasive note.  The launcher's smoke
test asserted that clicking a button spawned a new child window, by
counting `[wm] window created` log lines in the kernel serial.  That
proved the *child process* started and that the WM accepted its
`gui_create_window`.  But when I dumped the framebuffer, only the
launcher was visible.  The freshly created `gui_term` window sat
nowhere on screen.

Chapter 52 punted on this with a footnote.  This chapter is the
diagnosis and the fix, packaged as milestone 45.

## Symptom

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

A 720×440 window had been created, owned by pid 8, with id 1.  No
errors followed.  But the framebuffer showed only the 240×180 launcher
in the upper-left, and the navy wallpaper everywhere else.

## Hypothesis 1: cascade is broken (rejected)

If the new window were placed at the same coordinates as the launcher,
it would be hidden behind it.  But [wm.c](../../../kernel/core/wm.c)
already cascades fresh windows:

```c
uint32_t step = (id % 8) * 32;
win->x = 80 + step;
win->y = 60 + step;
```

So launcher (id=0) sits at (80,60) and `gui_term` (id=1) should land at
(112,92).  At 720×440 the new window would extend far past the
launcher's right edge, peeking out at x ≥ 320.  It doesn't.  Cascade
isn't the problem.

## Hypothesis 2: the painter's algorithm

[`compose_all`](../../../kernel/core/wm.c) walks windows in ascending
`z` and blits them in order:

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

The trick is the `if (g_wins[i].z <= pass) continue;` line.  The author
(me, two milestones ago) was treating `pass` as a stand-in for
"highest z already drawn", on the assumption that z values would always
be a contiguous run `1, 2, 3, …`  As long as that holds, each pass picks
exactly one window, increments the implied watermark by one, and the
loop drains.

**That assumption is false.**  Two operations assign `z`:

| Site | Operation                | Effect                  |
|------|--------------------------|-------------------------|
| `wm_create_window` | `win->z = g_next_z++;` | post-increment |
| left-down handler  | `w->z = ++g_next_z;`   | pre-increment  |

Trace the test:

| Step                         | g_next_z (before) | assignment        | result                                   |
|------------------------------|-------------------|-------------------|------------------------------------------|
| boot                         | 1                 | —                 | g_next_z=1                               |
| launcher created             | 1                 | `win->z = 1++`    | launcher.z = **1**, g_next_z = 2         |
| user clicks launcher button  | 2                 | `w->z = ++2`      | launcher.z = **3**, g_next_z = 3         |
| launcher spawns gui\_term    | 3                 | `win->z = 3++`    | gui\_term.z = **3**, g_next_z = 4        |

Now the painter runs with `launcher.z = 3` and `gui_term.z = 3`.

* **pass 0**: both windows have z>0; both have z=3.  Picker tie-breaks
  on first-found-with-strictly-smaller-z.  Both are equal so it picks
  the first one scanned (launcher, i=0).  Blits launcher.
* **pass 1**: same — launcher picked again, blitted again (over itself).
* **pass 2**: same.
* **pass 3**: now `g_wins[i].z <= 3` skips both windows.  `pick_id`
  remains -1.  Loop breaks.

`gui_term` is never painted.  Bug confirmed.

## Two fixes, layered

### Fix 1 — make the z allocator consistent

Both call sites should use `++g_next_z`, so every assignment produces a
strictly-increasing, unique value:

```diff
-    win->z          = g_next_z++;
+    win->z          = ++g_next_z;
```

After this, the trace becomes launcher.z=2, after-click=3, gui_term.z=4
— all distinct.  The painter happens to handle this correctly because
`launcher.z=3 ≤ pass=3` finally kicks launcher out and gui_term (z=4)
gets drawn on pass 3.

### Fix 2 — make the painter robust to sparse z values

Fix 1 is the immediate unblock, but the painter is still **structurally
wrong**.  After ~16 raises, z values fan out beyond `WM_MAX_WINDOWS`,
the outer loop exhausts its passes before reaching them, and windows
silently disappear again.  The right thing is to track "already
emitted" with a bitmask, decoupling z from the loop counter:

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

Now z is purely a sort key.  The outer loop runs at most N times
(N=`WM_MAX_WINDOWS`=16) and emits each in-use window exactly once, in
ascending z.  We get to keep "painter's algorithm" while no longer
over-fitting to dense z values.

## Verification

Re-running the launcher smoke test, with a new pixel-level assertion:

```python
ppm = read_ppm(DUMP_PATH)
title_pix    = pixel_at(ppm, 500, 100)   # expected: gui_term title bar
wallpaper_px = pixel_at(ppm, 1000, 700)  # expected: navy
assert title_pix != wallpaper_px
r, g, b = title_pix
assert b > r and b > 80   # the WM title-bar blue
```

Result:

```
PASS: spawned window rendered (title-bar pixel at (500,100) = (64, 96, 192))
MILESTONE 45: ALL TESTS PASSED
```

`(64, 96, 192)` is the WM's deliberately-chosen title-bar blue.  The
spawned window is now visibly composited on top of the launcher,
cascade and all.

## Lessons

The first fix makes the bug invisible *for now*.  It's tempting to
stop there: the test passes, the screendump looks right, and the
chapter could end with a cute "off-by-one" anecdote.  But the painter
would still be a time bomb — invisible until somebody opens enough
windows.

The general lesson is the same one we learned with the IRQ-IF bug
([chapter on the PS/2 keyboard's interrupt-flag interaction][m21]):
when one piece of state encodes two unrelated invariants ("position in
the z stack" *and* "loop iteration count"), the bug is in the
encoding, not in any one site.  Decouple the invariants.  The
post-fix code is barely longer and admits no parameter for which it
silently misbehaves.

In the next milestone we'll start using the now-correctly-composited
windows for something more interesting — a desktop shell.

[m21]: ../05-devices/../../../memories/repo/chapter-21-ps2-keyboard-IF-bug.md

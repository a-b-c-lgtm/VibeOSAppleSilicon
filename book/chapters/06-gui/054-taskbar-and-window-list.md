# Chapter 54 — A taskbar, three new GUI syscalls, and a real desktop

The system boots straight to a desktop and shows a
launcher window on a gradient wallpaper.  But "desktop" is still
generous: there is no taskbar, no way to switch between running apps,
and no visible representation of the WM's window list.  This chapter
adds all three.

## What "taskbar" actually requires

A taskbar is conceptually trivial — a strip with one button per
window, click to raise — but it forces three pieces of new infra:

1. **A window type that has no chrome.**  Title bars and close buttons
   on a 1280×28 strip pinned to the bottom of the screen would look
   absurd and waste vertical pixels.
2. **A way for one process (the taskbar) to enumerate windows owned
   by other processes.**  Until now the WM exposed only "what's the
   next event for any of *my* windows?".
3. **A way for one process to raise another process's window.**

These are the three new syscalls.  Plus a paint-pass change so a
"pinned" window always sits visually on top of regular windows
regardless of z-order.

## API additions

```c
/* userspace/libc/syscall.h */

#define GUI_WIN_FLAG_NO_DECORATION   0x1u
#define GUI_WIN_FLAG_ALWAYS_ON_TOP   0x2u
#define GUI_WIN_POS_AUTO             (-1)

struct gui_window_info {
    int32_t  id;
    uint32_t flags;
    int32_t  x, y;
    uint32_t w, h;
    uint32_t z;
    int32_t  focused;
    uint64_t owner_pid;
    char     title[64];
};

int gui_create_window_ex(uint32_t w, uint32_t h, const char *title,
                         uint32_t flags, int32_t x, int32_t y);
int gui_list_windows(struct gui_window_info *out, int max);
int gui_raise_window(int id);
```

Three new SVC numbers (47, 48, 49) added to a strict append-only
table in both `kernel/core/syscall.h` and `userspace/libc/syscall.h`.

The original `gui_create_window` is preserved as a thin wrapper —
existing callers keep working.

## Kernel changes

**`struct wm_window` gains a `flags` field.**  Two bits defined so
far:

| Flag                  | Meaning                                                                 |
|-----------------------|-------------------------------------------------------------------------|
| `NO_DECORATION = 0x1` | No title bar, no border, no close button.  Content fills exactly w*h.   |
| `ALWAYS_ON_TOP = 0x2` | Painter draws this AFTER all non-pinned windows regardless of `z`.      |

`compose_all` now does two passes, both using the same
"already painted" bitmask trick:

```c
/* Pass 1: every regular (non-always-on-top) window in z order. */
/* Pass 2: every always-on-top window, also in z order. */
```

`hit_test` mirrors the same priority — a click anywhere inside a
pinned window's rectangle wins over a regular window underneath, even
if the regular window has higher `z`.

**`blit_window` short-circuits for NO_DECORATION** windows: no title
bar, no close button, no border, just a pixel copy at `(w->x, w->y)`.

**`classify_click` returns `'B'` (body) for every point inside a
NO_DECORATION window.**  No drag zone, no close-button zone.  Every
click is forwarded to the app as a content-relative `MOUSE_DOWN/UP`.

**The minimum-size check is relaxed for NO_DECORATION** — a 28-pixel-
high taskbar would otherwise be rejected by `WM_MIN_HEIGHT = 40`:

```c
uint32_t min_h = (flags & GUI_WIN_FLAG_NO_DECORATION) ? 8u : WM_MIN_HEIGHT;
```

**A separate cascade counter.**  The previous code used the array
slot index to compute the cascade step:

```c
int32_t step = (id % 8) * 32;
win->x = 80 + step;
win->y = 60 + step;
```

This worked when every window was auto-positioned — the first window
landed at (80, 60), the second at (112, 92), and so on.  But the
moment a window is created with an explicit position (the taskbar at
(0, 772)), it consumes slot 0.  The launcher then gets slot 1 and
ends up at (112, 92) instead of (80, 60), even though it's the only
auto-positioned window.

Fix: a separate `g_next_cascade` counter, incremented only when
`GUI_WIN_POS_AUTO` is requested.  The taskbar takes a slot but doesn't
move the auto-cascade.  The launcher again lands at (80, 60).

## Userspace `taskbar` (~180 LOC)

```c
g_self_id = gui_create_window_ex(
    FB_W, BAR_H, "taskbar",
    GUI_WIN_FLAG_NO_DECORATION | GUI_WIN_FLAG_ALWAYS_ON_TOP,
    0, FB_H - BAR_H);
```

The main loop polls `gui_list_windows` ~7 Hz, and only repaints when
the visible-window count or the focused id changed:

```c
for (;;) {
    int n = gui_list_windows(infos, 16);
    if (needs_redraw(infos, n)) render(infos, n);

    struct gui_event ev;
    while (gui_poll_event(&ev)) {
        if (ev.type == GUI_EVENT_MOUSE_DOWN && (ev.arg2 & GUI_BTN_LEFT)) {
            handle_click(ev.arg0, ev.arg1);  /* -> gui_raise_window */
            g_known_count = -1;              /* force redraw */
        }
    }
    sleep_ms(150);
}
```

`render` skips two kinds of windows when emitting cells:

* the taskbar itself (`info->id == g_self_id`)
* any other always-on-top window (taskbars don't list taskbars,
  and notification popups would never want to be raised this way)

`init.c` auto-spawns it before the launcher.  The resulting boot
screen shows:

* gradient wallpaper
* launcher in the upper-left at (80, 60)
* taskbar across the bottom at (0, 772) showing one focused-blue cell
  labelled "launcher"

## Why ALWAYS_ON_TOP windows skip their click-raise z bump

In the previous milestone, every left-down on a window bumped its
`z` via `++g_next_z`.  For pinned windows this would pointlessly
inflate the counter — pinned windows are painted last *regardless*
of z.  Skipping the bump is a one-line guard:

```c
if (!(w->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP)) {
    w->z = ++g_next_z;
}
```

The corollary: `g_focus_id` does NOT auto-update when a pinned window
is created.  A taskbar that grabbed focus the moment it appeared
would steal keyboard input from whatever app the user just clicked
on — exactly the wrong behaviour.

```c
if (!(flags & GUI_WIN_FLAG_ALWAYS_ON_TOP)) {
    g_focus_id = id;   /* only non-pinned windows auto-focus */
}
```

## Verification

`scripts/test_taskbar.py` boots fully headless (no input) and asserts:

```
PASS: shell prompt reached
PASS: init auto-spawned /bin/taskbar
PASS: taskbar window created with flags=0x3
PASS: taskbar BG painted (pixel at (1100,775) = (24, 28, 50))
PASS: launcher cell painted FOCUSED (pixel = (96, 144, 224))
PASS: launcher label glyph rendered (pixel = (240, 240, 240))
PASS: launcher still visible (pixel at (200,90) = (232, 236, 240))
ALL TESTS PASSED
```

Five pixel assertions, each at a deliberately-different colour band,
catch an entire class of regressions: the taskbar isn't there, it's
there but with the wrong colour, the focused-cell colour is broken,
the label glyph isn't rendering, or the cascade-counter fix
regressed and the launcher slid off-screen.

## Lessons

- **Two bits of metadata buy a desktop.**  `NO_DECORATION` and
  `ALWAYS_ON_TOP` together are enough to express not just taskbars
  but every popup, tooltip, notification, and menu.  The same two
  flags will let us add a clock and a notification bubble in a
  later chapter without further kernel work.
- **Decouple semantically-distinct counters.**  The cascade counter
  WAS the array slot index, which is only correct when those two
  things happen to step in lockstep.  Once they don't (because some
  windows take explicit positions), the bug surfaces immediately.
  Same lesson as the z-order fix: when two invariants share state, separate them.
- **A minimum size that's right for chrome'd windows is wrong for
  borderless ones.**  Constraints should be parameterised by the
  shape of the thing being constrained.  Single global thresholds
  always end up wrong somewhere.

Next milestone: a clock in the right corner of the taskbar (turning
`SYS_UPTIME_MS` into a wall-clock-style HH:MM:SS), and a notification
popup using `NO_DECORATION` without `ALWAYS_ON_TOP`.

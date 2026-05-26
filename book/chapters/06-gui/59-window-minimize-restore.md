# Chapter 59 — Window minimize and restore

The desktop has had a working taskbar since Chapter 55, but every
window it tracks has had exactly two states: visible or destroyed.
That makes a real desktop frustrating fast.  Open three windows
to compare them, and you have to either tile-drag-cycle or close
two of them every time you want to look at the third.  Real
desktops have always solved this with *minimize* — a window that
disappears from the screen but stays alive in the taskbar so you
can flip back to it with one click.

This milestone adds that.  About 130 lines of kernel code, 50
lines of taskbar code, one new syscall, one new flag bit, one
new title-bar button, and one timing fix in the taskbar's main
loop that turned out to be the most interesting part of the
whole chapter.

## The model

A *minimized* window is alive — it owns its pixel buffer, its
event queue, its position, and its title — but it is invisible
to the user.  Specifically:

- The compositor (`compose_all`) skips it on every painter pass.
- The pointer hit-test (`hit_test`) skips it, so clicks where
  the window used to be fall through to whatever is now beneath.
- The taskbar still lists it.  That's the entire point.
- Restoring it brings it back to the top of the z stack and
  gives it focus, matching what every real desktop does when you
  click a minimized taskbar entry.

The state lives on the WM's window record:

```c
/* kernel/core/wm.c */
struct wm_window {
    int      in_use;
    int32_t  id;
    uint64_t owner_pid;
    uint32_t z;
    uint32_t flags;
    int      minimized;     /* NEW: hidden but live */
    /* ... */
};
```

We *could* have packed minimized into `flags`.  We didn't, for
two reasons.  First, the existing `GUI_WIN_FLAG_*` bits are
*intent* (the window asked for no decoration, asked to be on
top, asked to be pinned to the back) and the create syscall
explicitly rejects unknown bits — minimized is *state*, set and
cleared by the WM itself, and it would be confusing for it to
share the same word.  Second, keeping it as a separate field
makes the painter and hit-test loops literally one line each:

```c
if (g_wins[i].minimized) continue;
```

We do however expose the state to userspace via `flags` in the
`gui_window_info` returned by `gui_list_windows`, so the
taskbar can render minimized cells differently without needing
a second syscall:

```c
/* kernel/core/wm.c — wm_list_windows() */
info.flags = w->flags |
             (w->minimized ? GUI_WIN_FLAG_MINIMIZED : 0u);
```

`GUI_WIN_FLAG_MINIMIZED = 0x8` is documented as a *read-only
status bit* — `gui_create_window_ex` rejects it the same way
it rejects every other unknown bit, because the WM owns this
state, not the application.

## The syscall

```c
/* kernel/core/syscall.h */
SYS_GUI_SET_MINIMIZED = 51,  /* (int id, int on) -> 0/-errno */
```

Trivial userspace wrapper:

```c
/* userspace/libc/syscall.h */
static inline int gui_set_minimized(int id, int on)
{
    return (int)_svc2(SYS_GUI_SET_MINIMIZED, (long)id, (long)on);
}
```

The kernel implementation handles three small subtleties:

```c
long wm_set_minimized(uint64_t pid, int32_t id, int on)
{
    if (id < 0 || id >= WM_MAX_WINDOWS) return -EINVAL;
    struct wm_window *w = &g_wins[id];
    if (!w->in_use) return -ENOENT;
    /* Wallpapers refuse to minimize.  They're click-transparent
     * and have no decorations, so the UX makes no sense, and
     * losing the desktop would leave a black background. */
    if (w->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) return -EINVAL;

    if (on) {
        if (w->minimized) return 0;     /* idempotent */
        w->minimized = 1;
        /* Hand keyboard focus off if we just hid the focused
         * window — otherwise typing would go into the void. */
        if (g_focus_id == w->id) {
            int32_t  best = -1;
            uint32_t best_z = 0;
            for (int32_t i = 0; i < WM_MAX_WINDOWS; i++) {
                struct wm_window *o = &g_wins[i];
                if (!o->in_use)                              continue;
                if (o->minimized)                            continue;
                if (o->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM)   continue;
                if (best < 0 || o->z > best_z) { best = i; best_z = o->z; }
            }
            g_focus_id = (best >= 0) ? g_wins[best].id : -1;
        }
    } else {
        if (!w->minimized) return 0;
        w->minimized = 0;
        if (!(w->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP))
            w->z = ++g_next_z;
        g_focus_id = w->id;
    }
    compose_all();
    return 0;
}
```

We also taught `wm_raise_window` to auto-restore a minimized
window: a "raise" on something the user can't see is nonsense,
and matches what everybody expects from a taskbar click.

```c
long wm_raise_window(uint64_t pid, int32_t id) {
    /* ... */
    if (w->minimized) w->minimized = 0;     /* NEW */
    if (!(w->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP))
        w->z = ++g_next_z;
    g_focus_id = w->id;
    compose_all();
    return 0;
}
```

That keeps the taskbar's hot path to one syscall per click in
the common case ("click an unfocused or hidden window to bring
it to the front").

## The title-bar button

The decoration-painting code in `blit_window` already drew a red
"X" close button at the right edge of the title bar.  We add a
grey "_" minimize button immediately to its left:

```c
/* kernel/core/wm.c — inside blit_window() */
if (deco_w >= WM_CLOSE_BTN_W + WM_MIN_BTN_W + WM_BTN_GAP + 8) {
    uint32_t bx = (uint32_t)deco_x + (uint32_t)deco_w
                - WM_CLOSE_BTN_W - 2 - WM_BTN_GAP - WM_MIN_BTN_W;
    uint32_t by = (uint32_t)deco_y + 2;
    uint32_t bw = WM_MIN_BTN_W;
    uint32_t bh = WM_TITLE_HEIGHT - 4;
    fb_fill_rect(bx, by, bw, bh, FB_COLOR(0x60, 0x60, 0x60));
    /* Underscore: a 2-px-tall horizontal bar near the bottom. */
    if (bw > 8 && bh > 6) {
        uint32_t ly0 = by + bh - 4;
        for (uint32_t i = 4; i + 4 < bw; i++) {
            uint32_t px = bx + i;
            if (px < fb->width) {
                if (ly0     < fb->height)
                    fb_draw_pixel(px, ly0,     FB_COLOR(0xFF,0xFF,0xFF));
                if (ly0 + 1 < fb->height)
                    fb_draw_pixel(px, ly0 + 1, FB_COLOR(0xFF,0xFF,0xFF));
            }
        }
    }
}
```

`classify_click` mirrors the geometry exactly so the click rect
matches the painted rect.  We add an `'M'` zone to the existing
`'C' / 'T' / 'B' / '-'` quartet, and `wm_pointer_button` handles
it by calling `wm_set_minimized(w->owner_pid, w->id, 1)` and
returning early — `wm_set_minimized` already calls `compose_all`,
so falling through would just paint twice.

The width gate (`>= WM_CLOSE_BTN_W + WM_MIN_BTN_W + WM_BTN_GAP +
8`) protects very narrow windows from having two buttons crammed
on top of each other or off the right edge.  At 80 px (the
minimum window width) the close button still draws but the
minimize button cleanly disappears, and `classify_click` agrees
— there is no ghost click rect for an unpainted button.

## The taskbar's tri-state click

The new `gui_window_info.flags` bit is the only thing the
taskbar needs from the kernel.  Render-side, minimized cells
get a darker fill and dimmer border:

```c
/* userspace/taskbar/taskbar.c */
uint32_t fill   = info->focused ? CELL_FOCUS_BGRA : CELL_BGRA;
uint32_t border = CELL_BORDER;
uint32_t fg     = TEXT_BGRA;
if (minimized) {
    fill   = CELL_MIN_BGRA;       /* (24, 32, 56) */
    border = CELL_MIN_BORDER;     /* (64, 80, 120) */
    fg     = TEXT_MIN_BGRA;       /* (144, 152, 176) */
}
```

Click-side, the cell now has three states and the click handler
picks a verb based on what the cell looked like at click time:

```c
if (c->minimized) {
    gui_raise_window(c->win_id);          /* restore + raise + focus */
} else if (c->focused) {
    gui_set_minimized(c->win_id, 1);      /* hide it */
} else {
    gui_raise_window(c->win_id);          /* bring to front */
}
```

That's a standard desktop convention: the focused window's
taskbar cell is a *toggle* (click to hide, click to show), while
background-window cells are pure raise.

## The taskbar timing bug

This was the actual hard part of the milestone.

The first cut of this code passed steps 1–4 of the smoke test
(launch → minimize via title-bar button → restore via taskbar
click) but failed step 5 (click the focused taskbar cell to
re-minimize).  The cell looked focused on screen, the click
landed on it, and yet the launcher stayed visible.

Tracing the event order revealed the culprit:

1. User clicks the launcher's taskbar cell.
2. The WM's pointer handler runs.  Step one of every left-down:
   focus the clicked window.  *The taskbar takes focus.*
3. The WM forwards a `MOUSE_DOWN` event into the taskbar's
   ring buffer.
4. The taskbar's main loop wakes up, runs in this order:

   ```c
   for (;;) {
       int n = gui_list_windows(infos, 16);
       int redraw = needs_redraw(infos, n);
       if (redraw) render(infos, n);     /* (a) repopulates g_cells */
       /* ... */
       while (gui_poll_event(&ev)) {
           if (ev.type == GUI_EVENT_MOUSE_DOWN)
               handle_click(ev.arg0, ev.arg1);   /* (b) reads g_cells */
       }
   }
   ```

5. At step (a), `gui_list_windows` reports launcher.focused == 0
   (because the WM moved focus to the taskbar in step 2).  The
   render therefore writes `c->focused = 0` for the launcher's
   cell.
6. At step (b), `handle_click` consults `c->focused`, sees 0,
   and falls through to the third branch — `gui_raise_window`,
   which is a no-op because the launcher is already on top and
   visible.

The rendered cell is "stale" the moment the click lands.  The
fix is to drain events *before* re-rendering:

```c
for (;;) {
    /* Drain events FIRST so the cell snapshot from the previous
     * render still reflects what the user clicked on. */
    while (gui_poll_event(&ev)) {
        if (ev.type == GUI_EVENT_MOUSE_DOWN) handle_click(...);
    }

    /* Then update display. */
    int n = gui_list_windows(infos, 16);
    if (needs_redraw(infos, n)) render(infos, n);
    /* ... */
}
```

With the order swapped, `handle_click` runs against the cell
snapshot from the *previous* iteration's render — which is the
one the user actually saw and clicked on.  After it finishes,
the next `list_windows` reports the new state and the next
render shows it.

The general lesson: **input events should always be consulted
against the screen state the user is reacting to, not the screen
state we're about to draw.**  This applies to any UI loop where
"render" and "handle input" are both side-effecting and the
input changes what gets rendered.  Web frameworks call this the
"controlled vs uncontrolled" distinction; immediate-mode GUIs
solve it by using last frame's hover state to decide this
frame's clicks; this implementation fixes it by simply moving four lines.

The same issue would have lurked in the existing pre-M51
taskbar (clicking a cell would re-render with the wrong focus
state for one frame), but it was undetectable because the only
action the click triggered was `gui_raise_window`, whose effect
("ensure this window is on top") is the same regardless of who
had focus a moment ago.  Adding the *toggle* turned a benign
cosmetic glitch into a hard functional bug.

## What the test exercises

`scripts/test_minimize.py` boots fully headless and:

1. Verifies the launcher window paints (white body pixel).
2. Tablet-clicks the title-bar minimize button at (288, 72) —
   the kernel `'M'` click path.
3. Asserts the body pixel is no longer white (window hidden).
4. Asserts the taskbar cell is rendered dim grey-blue
   (`CELL_MIN_BGRA`).
5. Tablet-clicks the dim cell at (98, 786).
6. Asserts the body pixel is white again (`gui_raise_window`'s
   auto-restore worked).
7. Asserts the cell is now in the focused colour
   (`CELL_FOCUS_BGRA`).
8. Tablet-clicks the focused cell.
9. Asserts the body pixel is no longer white (focused-cell
   toggle worked, i.e. the event-order fix is effective).

All nine pass on the first cold boot, every time.

## What we didn't add

A few things a real desktop would have that this one still
doesn't:

- **Animation.**  Minimize is instantaneous.  No "fly to
  taskbar" tween.  Adding it would mean either an asynchronous
  WM animation thread (overkill) or a per-frame interpolated
  blit driven by the existing compositor (cheap, but the
  framebuffer doesn't double-buffer yet so it would tear).
- **Keyboard shortcuts.**  Cmd-M to minimize the focused
  window.  Cmd-` to cycle.  These need a system-wide keymap
  layer that the WM intercepts before delivering keys to apps,
  which is its own milestone.
- **Per-app behaviour.**  Some windows shouldn't be minimizable
  (a modal dialog, say).  We have no flag for "decline minimize
  on this window" yet, because we have no modals.

Each of those is a future milestone.  The *foundation* — a WM
that knows the difference between hidden and dead, and a
taskbar that lets the user navigate the difference — is here.

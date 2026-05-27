# Chapter 51 — launcher: clicking is the new typing

> *Up to now, every GUI app on this system has had
> to be launched the same way: type its name into a serial-port
> shell.  This chapter builds the smallest possible thing that
> turns the system into something a human could plausibly use
> with a mouse — a tiny floating launcher window with three
> buttons that `spawn()` the binaries we built in chapters 48,
> 50, and 51.  In ~200 lines we cross the line from "an OS that
> happens to render windows" to "an OS where you launch
> programs with a click."*

## What the launcher actually is

A 240×180 window with three centred buttons:

```
+----------------------------+
|  launcher              [X] |
+----------------------------+
|  +----------------------+  |
|  |       gui_term       |  |
|  +----------------------+  |
|  +----------------------+  |
|  |        paint         |  |
|  +----------------------+  |
|  +----------------------+  |
|  |       notepad        |  |
|  +----------------------+  |
+----------------------------+
```

A click on a button calls `spawn(path, "")` with the matching
absolute path.  The launched program inherits nothing except
its own fresh address space (chapter 23) and a brand-new
window from `gui_create_window` (chapter 47).  The launcher's
state is unaffected — it stays open ready for the next click.

## Mouse-driven UI in this codebase

Until this milestone, every GUI app cared about exactly one
thing the WM forwarded: keystrokes (`GUI_EVENT_KEY`).  Chapter
49 added the pointer and three more event types that the WM
already forwards but no app had needed:

| event                    | arg0 | arg1 | arg2          |
|--------------------------|------|------|---------------|
| `GUI_EVENT_MOUSE_MOVE`   | x    | y    | —             |
| `GUI_EVENT_MOUSE_DOWN`   | x    | y    | button bitmap |
| `GUI_EVENT_MOUSE_UP`     | x    | y    | button bitmap |

`x` and `y` are *content-relative* — the WM has already
subtracted the title bar and border, so `(0, 0)` is the
top-left pixel of the app's drawable area.  This makes hit
testing trivial: every UI element is just a rectangle in
content coordinates, and `hit_test(cx, cy)` is

```c
for (int i = 0; i < N_BUTTONS; i++) {
    int by = btn_y(i);
    if (cx >= BTN_X && cx < BTN_X + BTN_W &&
        cy >= by    && cy < by + BTN_H)   return i;
}
return -1;
```

That's the entire mouse-event vocabulary the launcher
implements.

## Hover feedback for free

Because the WM also delivers `MOUSE_MOVE` events (one per
position update from virtio-tablet), the launcher gets to
implement hover highlighting almost for free:

```c
case GUI_EVENT_MOUSE_MOVE: {
    int new_hover = hit_test((int)ev.arg0, (int)ev.arg1);
    if (new_hover != g_hover) {
        g_hover = new_hover;
        render();           /* only on actual change */
    }
    break;
}
```

Two non-obvious things here:

1. **Re-render only on transition.**  The tablet may emit dozens
   of MOVE events per second.  Rendering on every one would
   waste compositor work.  We compare to `g_hover` and skip
   the redraw if the cursor is still in the same button (or
   still in dead space).
2. **The cursor sprite is the WM's job.**  The compositor draws
   the pointer on top of every window after the apps have
   flushed; the launcher never touches it.  Our hover state is
   purely about the button colour underneath.

The hover colour is a slight darken of the button fill — same
logic that any desktop UI library uses to make widgets feel
responsive.

## Fire-and-forget spawn

The single line of substance in the click handler is

```c
(void)spawn(g_buttons[i].path, "");
```

`spawn` returns the child's tid, which we ignore.  We do *not*
call `wait()`; we don't want to block the launcher waiting
for `gui_term` to exit.

This leaks a kernel `struct thread` per launched-and-exited
child until either:

- some other process happens to call `wait()` and reaps a
  child whose parent is the launcher (the kernel's `wait()`
  semantics are "any zombie child of mine"), or
- the launcher itself exits, at which point all its zombie
  children become orphans (see `thread_exit` in
  `kernel/core/thread.c` — if `parent_id < 0` after parent
  has gone, the struct is removed from `all_head` and
  effectively leaked but no longer eats fd-table or scheduler
  cycles).

For a handful of launches per session this is fine.  A
"proper" launcher would either install a periodic background
reaper thread or have the kernel auto-reap when the parent
already exited; both are mechanical refactors we can do once
we care about long-running launcher sessions.

## Spawned child's window placement

When you click `gui_term`, the launcher fires `spawn`, the
child's `main()` runs, calls `gui_create_window(720, 440,
"gui_term")`, and the WM picks a position for the new window.
Currently the WM uses a fixed initial position of (80, 60)
for every window — so the gui_term window opens *underneath*
the launcher window because they overlap.

You can see this in the smoke test screenshot: only the
launcher is visible.  But the kernel log says
`[wm] window created` twice and the second window IS
present in the WM's z-list — clicking on whatever pixel of
the gui_term window pokes out from behind the launcher will
focus and raise it.

A future "tile new windows in a cascade" change to the WM
(start at (80, 60), then (110, 90), then (140, 120) ...) would
make this look better immediately.  The chapter 48 `wm_focus`
+ raise machinery already handles the "click to bring to
front" behaviour, so once windows don't overlap perfectly,
everything falls into place.

## Smoke test: the kernel log is the receipt

The smoke test cannot rely on the *visual* presence of a
second window because it overlaps the launcher.  But it can
rely on the kernel's serial log:

```python
prev_creates = log.count(b"[wm] window created")
left_click(qmp, btn0_cx, btn0_cy)
log += wait_for(ser, b"[wm] window created", 5.0)
new_creates = log.count(b"[wm] window created")
assert new_creates - prev_creates >= 1
```

Every call to `wm_create_window` prints exactly one
`"[wm] window created"` line on the serial console.  Counting
those messages before and after the click is a precise,
race-free way to assert "the click actually spawned a new
window."

The button-centre coordinate calculation is hard-wired to the
known initial-window placement of `(80, 60)`:

```python
btn0_cx = WIN_X + WIN_W // 2                  # 80 + 120 = 200
btn0_cy = WIN_Y + TITLE_H + 16 + 18            # 60 + 24 + 16 + 18 = 118
```

This breaks the moment the WM grows a smarter window-placer,
but it is the simplest possible thing that works today.

## What clicking *implies* about the system

Look at what has to be true for that one button click to land:

1. The Python harness encodes the `(x, y)` as fractions of
   `0..0x7FFF` and sends them as virtio-tablet `EV_ABS` events
   over QMP.
2. QEMU's tablet device synthesises evdev events into the
   virtio-input ring.
3. Our virtio-input driver (chapter 48) decodes them, scales
   to framebuffer pixels, and feeds the WM through
   `pump_input_into_wm`.
4. The WM hit-tests the screen-absolute pointer position
   against its z-stack of windows, finds the launcher on top,
   converts to content-relative coordinates, and pushes a
   `GUI_EVENT_MOUSE_DOWN` into the launcher session's event
   ring.
5. The launcher polls, receives the event, hit-tests against
   its own button rectangles, calls `spawn()`.
6. The kernel's spawn syscall walks `/bin/gui_term` from OSFS
   (chapter 20), allocates an address space (chapter 23),
   loads the ELF (chapter 12), creates a thread, and schedules
   it.
7. The new thread reaches `main`, calls `gui_create_window`,
   the WM allocates a pixel buffer + decoration, and prints
   `"[wm] window created"` on the serial console.
8. The smoke test on the host counts that string and prints
   `PASS`.

There is no piece of this stack that is "stub" or "TODO."  All
of it is real, all of it has been independently smoke-tested
in the eight chapters that built each layer, and all of it
runs in under a second from the moment a virtual mouse button
goes down.

## What this milestone *isn't*

- **Not a desktop shell.**  A real desktop shell would own the
  background, render a wallpaper, place a taskbar full of
  running window thumbnails along an edge, and run forever.
  Building any one of those (especially the wallpaper, which
  needs a 1280×800×4 ≈ 4 MB pixel buffer) is its own
  milestone-sized project.  We deliberately scoped the
  launcher to be the smallest *useful* mouse-driven app, not
  a whole shell environment.
- **Not a permanent window.**  Closing the launcher with the
  red X kills it, just like any other window.  The user is
  expected to launch it from the serial shell at session
  start.
- **Not configurable.**  The three apps are hard-coded.  A
  more grown-up version would read `/etc/launcher.conf` (which
  doesn't exist yet, because we don't have a notion of `/etc`)
  or take its app list from `argv`.

## The cumulative effect

Eight chapters into Part VI we now have, on a real headless
QEMU, a system that:

- Boots through MMU + GIC + timer + scheduler in under a
  second.
- Mounts a writable tmpfs and a read-only OSFS off virtio-blk.
- Renders pixels into a virtio-gpu framebuffer at 1280×800.
- Reads keystrokes and absolute pointer coordinates from
  virtio-input devices.
- Composites multiple decorated windows owned by separate
  user processes, with focus, drag, and close.
- Lets the user click a button to spawn a new GUI process
  that owns its own window and has its own address space.
- Runs a real text terminal, a real text editor, and a real
  paint program inside that GUI.

That is the entire pre-1995 home-computer feature list,
delivered by an OS we wrote ourselves on top of QEMU's HVF
acceleration on Apple Silicon.  Everything beyond this point
— networking, browser, multitasking polish, real fonts,
permissions, packages — is incremental.

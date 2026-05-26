# Chapter 48 — An in-kernel window manager and seven GUI syscalls

Chapters 46 and 47 gave us pixels and keystrokes.  This chapter
glues them together with a **window manager** that lives entirely
in the kernel, and a **GUI syscall ABI** that lets userspace open
windows, paint them, and read keystrokes from them — without ever
touching the framebuffer or the keyboard device directly.

The end-state of this chapter is a working desktop:

- Boot the kernel.
- Type `hellogui` at the shell.
- A 480×320 window appears with a gradient background, a focused
  title bar, and a four-line greeting in three colours.
- Press `a`, `b`, `c` — they appear inside the window's input row
  in real time, while the shell stays silent.
- Press `Esc` — the window closes, focus returns to the shell.

We touch six files for this:

- `kernel/core/wm.{h,c}` — the WM itself (~430 LOC).
- `kernel/core/syscall.{h,c}` — seven new syscalls 40..46.
- `kernel/core/console_in.c` — gain the WM-routes-keystrokes
  branch.
- `kernel/core/thread.c` — `thread_exit` calls
  `wm_destroy_owner(pid)` so windows free on process death.
- `userspace/libc/syscall.h` — userspace wrappers + the
  `gui_event` and arg-pack structs.
- `userspace/hellogui/hellogui.c` — the canonical GUI smoke test.

## Design at a glance

```
                    +------------------+
   virtio-input  -> | wm_keyboard_byte |  push GUI_EVENT_KEY into
   bytes            +------------------+  focused window's ring
                            |
   userspace gui_poll_event |
   syscall ----------------+|
                           v
                    +-------------+
                    | wm_poll_event| copy ring head to user
                    +-------------+
                            ^
                            |
                    +------------+         compose_all = paint
   userspace gui_  | wm_present  | write   wallpaper, then walk
   present, draw,  | / draw_text | into    in z-ascending order,
   fill, flush  -> | / fill_rect | window  blit each window's
                   +------------+  pixels  pixel buffer over fb,
                            |              then fb_present(0,0,0,0).
                            v
                   +-------------+
                   |  fb_present  |
                   +-------------+
```

Three rules govern the design:

- **The kernel owns the framebuffer.**  Userspace never sees a
  pointer into it.  Apps own their own pixel buffer (BGRA, one
  per window) and copy into the framebuffer only via syscalls.
- **The WM owns decorations.**  The 24 px title bar and 1 px
  border are painted by the kernel using the same 8×16 font from
  chapter 46.  Apps paint into the content area only.
- **Focus is exclusively input-routed.**  As long as any window
  exists, every keyboard byte goes to the topmost (focused)
  window.  Stdin is silent.  Close the last window and stdin
  comes back.

## Per-window state

```c
struct gui_event_ring {
    struct gui_event slots[WM_EVENT_QUEUE_CAP];
    uint32_t head, tail;
};

struct wm_window {
    int          in_use;
    int32_t      id;
    uint64_t     owner_pid;
    int32_t      x, y;
    uint32_t     w, h;
    uint32_t     z;
    char         title[WM_TITLE_MAX + 1];
    uint32_t    *pixels;             /* w*h, BGRA, kheap-owned */
    struct gui_event_ring events;
};

static struct wm_window g_wins[WM_MAX_WINDOWS];   /* 16 slots */
```

The pixel buffer is `kmalloc(w * h * 4)` at create time and
`kfree`'d on destroy.  At 480×320 that's 600 KiB, which is fine
on our 8 GiB of guest RAM but starts to add up.  A future
milestone might switch to `pmem_alloc_contig` and map the same
pages into the user too, so `gui_present` could become a no-op —
but the copy-based ABI is much easier to reason about for now.

## The composite

`compose_all()` is the painter's algorithm in 25 lines:

```c
static void compose_all(void)
{
    paint_wallpaper();
    /* Walk windows in ascending z (topmost last). */
    for (uint32_t z = 1; z < g_next_z; z++) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (g_wins[i].in_use && g_wins[i].z == z) {
                blit_window(&g_wins[i]);
                break;
            }
        }
    }
    fb_present(0, 0, 0, 0);
}
```

`paint_wallpaper()` clears to `FB_COLOR(0x10, 0x14, 0x28)` plus
a placeholder taskbar strip at the bottom.  `blit_window()`
paints the title bar (the focused window gets a brighter colour),
draws the title text, paints a 1 px border, then row-by-row
copies the window's BGRA pixels into the framebuffer via
`fb_draw_pixel` (clipped to screen bounds).

We re-paint the entire screen on every `wm_flush`.  This is
wasteful — at HVF speed the repaint is invisible, and
having a single composite path makes z-order changes,
focus changes, and window destroys trivial to get right.

## The seven syscalls

Numbers 40..46 in [kernel/core/syscall.h](../../../kernel/core/syscall.h):

| number | name                  | args                                           |
|-------:|-----------------------|------------------------------------------------|
|     40 | `gui_create_window`   | `w, h, title*` → window id                     |
|     41 | `gui_destroy_window`  | `id`                                           |
|     42 | `gui_present`         | `args*` (`{id, x, y, w, h, pixels*}`)          |
|     43 | `gui_fill_rect`       | `args*` (`{id, x, y, w, h, bgra}`)             |
|     44 | `gui_draw_text`       | `args*` (`{id, x, y, s*, fg, bg, transparent}`)|
|     45 | `gui_flush`           | `id`                                           |
|     46 | `gui_poll_event`      | `event_out*` → 0/1                             |

### Why arg-pack structs

Three of the seven syscalls have more than four arguments.  Our
SVC ABI passes `x0..x5`, but packing 32-bit fields
into 64-bit registers is fragile (sign-extend mistakes, alignment
mistakes, no struct-printer in the debugger).  Instead each
multi-arg call passes **one user pointer to a packed struct**:

```c
struct gui_present_args {
    uint32_t id, x, y, w, h;
    uint64_t pixels_uptr;     /* pointer to BGRA buffer */
};
```

The kernel `copy_from_user`s the struct into a local copy, then
validates the embedded pointer separately before dereferencing.
This pattern adds 5 lines of boilerplate per call but is
considerably easier to extend (just add a field) and matches the
shape of the inline wrappers in `userspace/libc/syscall.h`:

```c
static inline int gui_present(int id, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h,
                              const uint32_t *pixels)
{
    struct gui_present_args a = {
        .id = id, .x = x, .y = y, .w = w, .h = h,
        .pixels_uptr = (uint64_t)(uintptr_t)pixels,
    };
    return (int)_svc1(SYS_GUI_PRESENT, (long)(uintptr_t)&a);
}
```

## Routing keystrokes — and the bug that almost shipped

`console_in.c` already pushes bytes into the WM with this branch:

```c
if (wm_has_windows() && wm_keyboard_byte(c))
    return serial_try_getc(out);     /* don't deliver to stdin */
```

But `console_try_getc` is only invoked from `vfs_read` --
i.e. from a userspace `read()` call.  The first hellogui app
never called `read()`; it only called `gui_poll_event` in a loop.
Result: virtio-input slots filled up, no `console_try_getc` ran,
no `wm_keyboard_byte` ran, and the WM never saw a key.

Fix lives at the top of `kernel/core/syscall.c`:

```c
static void pump_input_into_wm(void)
{
    if (!virtio_input_present() || !wm_has_windows()) return;
    char c;
    while (virtio_input_try_getc(&c))
        (void)wm_keyboard_byte(c);
}

static long sys_yield(void) {
    pump_input_into_wm();
    yield();
    return 0;
}

static long sys_gui_poll_event(long out_uptr) {
    pump_input_into_wm();
    return wm_poll_event(thread_current()->id,
                         (struct gui_event *)(uintptr_t)out_uptr);
}
```

Both `sys_yield` and `sys_gui_poll_event` drain the device.  The
rule for milestone 41 (mouse) is the same: any new input device
needs its own `pump_*_into_wm` hook on the syscall path.

## hellogui

[userspace/hellogui/hellogui.c](../../../userspace/hellogui/hellogui.c) is
~140 LOC of GUI code:

```c
int main(int argc, char **argv) {
    const char *title = (argc > 1) ? argv[1] : "hellogui";
    int win = gui_create_window(WIDTH, HEIGHT, title);
    if (win < 0) { write(1, "..."); return 1; }

    paint_gradient();
    gui_present(win, 0, 0, WIDTH, HEIGHT, pixels);
    gui_draw_text(win, 16, 24, "Hello from milestone 40!",
                  GUI_BGRA(0xFF, 0xFF, 0xFF), 0, 1);
    /* ... three more lines of greeting + pid ... */
    gui_flush(win);

    char  line[128]; size_t cur = 0; line[0] = 0;
    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) { yield(); continue; }
        if (ev.type != GUI_EVENT_KEY) continue;
        char c = (char)(ev.arg0 & 0xFFu);
        if (c == 0x1B) break;                              /* ESC */
        if (c == '\r' || c == '\n') { cur = 0; line[0] = 0; }
        else if (c == 0x7F) { if (cur) line[--cur] = 0; }
        else if (cur + 1 < sizeof(line)) {
            line[cur++] = c; line[cur] = 0;
        }
        repaint_input_row(line);                           /* gradient + text */
        gui_flush(win);
    }
    gui_destroy_window(win);
    return 0;
}
```

The repaint cost on each keystroke is tiny: just the 40 px input
row gets re-`gui_present`'d, then `gui_draw_text` overlays the
new line.  `gui_flush` re-composites everything (cheap) and we're
back at the poll loop.

## Smoke test

[scripts/test_wm.py](../../../scripts/test_wm.py) is the canonical end-to-end
GUI test:

1. Boot QEMU `-display none` with `virtio-gpu` + `virtio-keyboard`
   + `virtio-blk` + serial-over-Unix-socket + QMP-over-Unix-socket.
2. Wait on serial for `$ ` and `window manager ... ok`.
3. Type `hellogui\n` via QMP `input-send-event` qcode strings.
4. Wait for `[wm] window created` log line.
5. QMP `screendump filename=/tmp/osdev-fb.ppm format=ppm`.
6. Parse the PPM (P6, 1280×800, 255), count distinct colours on
   an 8 px sample grid; demand ≥ 5 non-wallpaper colours.
7. Type `abc`, screendump again, assert pixels changed.

For visual debugging:
`sips -s format png /tmp/osdev-fb.ppm --out /tmp/wm.png`.

## Three gotchas

1. **`memset` from struct initialisers** (yet again).  `wm.c`
   gets the same one-line `memset` and `memcpy` shims as
   chapters 46 and 47.
2. **`thread_exit` must call `wm_destroy_owner(pid)`** before
   tearing down the address space, or the WM keeps a dangling
   pointer to a freed pixel buffer.  Easy to miss; caused a
   beautiful "previous app's content is now ghost-rendered into
   the next app's window" bug during bring-up.
3. **GUI events overflow silently.**  The 64-entry per-window
   ring drops the oldest event when full.  Apps that hold-down a
   key and forget to drain will see consistent loss; this is
   intentional and matches the X11 behaviour for unresponsive
   clients.

## Files changed

- `kernel/core/wm.{h,c}` — new.
- `kernel/core/syscall.{h,c}` — added `SYS_GUI_*` enum and seven
  handlers + `pump_input_into_wm()`.
- `kernel/core/console_in.c` — added the `wm_has_windows()`
  branch.
- `kernel/core/thread.c` — `thread_exit` calls
  `wm_destroy_owner(g_current->id)`.
- `userspace/libc/syscall.h` — `GUI_BGRA(R,G,B)`,
  `GUI_EVENT_*`, `struct gui_event`, three arg-pack structs,
  seven inline wrappers.
- `userspace/hellogui/hellogui.c` — new demo app.
- `Makefile` — link rules for `hellogui`, include in OSFS image.
- `scripts/test_wm.py` — new headless smoke test.

## What's deferred (the milestone-41+ to-do list)

- **Mouse** (`virtio-tablet`, EV_ABS): cursor sprite, click-to-
  focus, title-bar drag, close-button hit testing.
- **Multi-app composition**: stagger logic exists, but the
  taskbar in `paint_wallpaper` is just a stripe — milestone
  41/42 will replace it with `desktopd`.
- **`gui_term`** (terminal-in-window): needs stdout-capture
  syscalls so the window can render the child's stdout into its
  own pixel buffer.
- **Mode lines / window-server protocol**: today the kernel _is_
  the window server.  When/if we want a userspace compositor
  (à la Mir or Wayland) the seven syscalls become an IPC
  channel.
- **Alt-tab**: today there's no way to focus a non-topmost
  window.  Trivial keyboard hotkey + raise-to-top in `wm.c`.
- **Drop-shadow / alpha / antialiased text**: would need a
  shadow buffer between the per-window pixel buffer and the
  framebuffer.

The single biggest design choice ratified by this chapter is
**focus = topmost & input is exclusive**.  Everything we add in
milestone 41+ slots into that model without breaking it.

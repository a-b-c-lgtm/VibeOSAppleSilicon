# Chapter 49 — virtio-tablet, mouse focus, drag, and close

> *Milestone 41.  We added a window manager in chapter 48 and gave it
> a keyboard in chapter 47.  This chapter adds the second half of
> "input" — a pointer — and turns the WM from a static compositor
> into something you can actually drive: focus on click, raise on
> click, drag by the title bar, close with the red X.  The whole
> thing is exercised by a tiny `paint` userspace demo and a fully
> headless QMP-driven smoke test.*

## What we want, and why we cannot use a relative mouse

We want a working pointing device.  virtio-input exposes two
canonical ones:

- **virtio-mouse** reports relative motion with `EV_REL`.  Each event
  is a delta — "pointer moved +3 in x, -1 in y" — and the host
  hides the mouse cursor and locks it to the guest window.  Under
  HVF on macOS this kind of cursor capture is awkward to set up
  headless, and our test harness needs to inject pointer motion
  with no host cursor at all.
- **virtio-tablet** reports absolute coordinates with `EV_ABS`.  Each
  event is "the pointer is at (X, Y)" where X and Y range from 0 to
  some maximum (QEMU's tablet uses `0..0x7FFF` on each axis).  No
  cursor capture; the guest just sees coordinates.

A tablet is the right choice for a hobby OS for three reasons:

1. **The QMP test harness becomes trivial.** Sending
   `{"type":"abs","data":{"axis":"x","value":N}}` over the QMP
   socket moves the cursor to a known pixel; no need to model
   relative-motion accumulation.
2. **No cursor capture, so the macOS host stays usable.** A
   relative mouse would either steal the host pointer or behave
   weirdly under `-display none`.
3. **Cleaner state machine.** With an absolute device the kernel
   does not have to track sub-pixel accumulators or accel curves.
   It just maps `(ax, ay)` → screen pixels and dispatches.

Both devices use evdev semantics inside virtio-input.  We probed for
a keyboard in chapter 47 by accepting the first virtio-input slot
that responded; for the tablet we have to be choosier — the
keyboard is *also* a virtio-input device — so we need a way to tell
them apart.

## Telling a tablet from a keyboard at probe time

virtio-input's device-config space exposes a "selector + subselector"
view of the device's capabilities (see virtio 1.2 §5.8.5).  You
write the selector byte at offset 0, the subselector at offset 1, a
DSB to publish, and then read the size byte at offset 2 followed by
up to 128 bytes of payload at offset 8.

The selector we care about is `VIRTIO_INPUT_CFG_EV_BITS = 0x11`,
with the event-type code as the subselector:

| subselector       | meaning                       |
|-------------------|-------------------------------|
| `EV_KEY = 1`      | bitmap of supported key codes |
| `EV_REL = 2`      | bitmap of relative axes       |
| `EV_ABS = 3`      | bitmap of absolute axes       |
| `EV_SYN = 0`      | sync — every device has this  |

A keyboard reports `size > 0` for `EV_KEY` and `size == 0` for
`EV_ABS`.  A tablet reports `size > 0` for both.  So the rule is:

```c
/* virtio_input.c (keyboard probe) */
if (cfg_query_size(base, EV_BITS, EV_ABS) != 0) {
    /* This is a tablet/mouse, not a keyboard.  Skip. */
    return -1;
}
```

```c
/* virtio_tablet.c (tablet probe) */
if (cfg_query_size(base, EV_BITS, EV_ABS) == 0) {
    /* No absolute axes.  Not a tablet. */
    return -1;
}
```

With both probes running back-to-back from `main.c`, the keyboard
and the tablet self-sort into separate slots regardless of how QEMU
ordered them on the bus.

The tablet driver also reads the per-axis maximum value from
`VIRTIO_INPUT_CFG_ABS_INFO = 0x12`.  The payload union for that
selector starts with a `struct virtio_input_absinfo { __le32 min,
max, fuzz, flat, res; }` at offset 8, so we just walk to
`config + 0x08 + 4` and read the 32-bit max.  For QEMU's tablet
this comes back as `0x7FFF` for both axes.

## Coordinate mapping: absolute → screen

The tablet driver receives `(ax, ay)` in `[0, abs_max]`.  Mapping to
screen pixels in `[0, fb_w-1]` is one multiply and one divide:

```c
static int absolute_to_screen(uint32_t a, uint32_t a_max, uint32_t fb_dim)
{
    if (a_max == 0) return 0;
    if (a > a_max) a = a_max;
    /* 64-bit math because a*fb_dim overflows 32-bit at 4K * 0x7FFF. */
    return (int)((uint64_t)a * (fb_dim - 1) / a_max);
}
```

`uint64_t` is mandatory.  `0x7FFF * 4096 = 0x07FFF000`, which fits
in 32 bits; but `0x7FFF * 8192 = 0x0FFFE000`, and a 4K-x screen
would overflow as soon as we add the rounding bias.  Once burnt,
twice cautious.

## Frame coalescing with EV_SYN

evdev guarantees that a single physical event (a pointer move plus
a button press) arrives as a sequence terminated by `EV_SYN`:

```
EV_ABS  ABS_X   3214
EV_ABS  ABS_Y   1187
EV_KEY  BTN_LEFT 1
EV_SYN  SYN_REPORT 0       <-- frame boundary
```

If we deliver each component event to the WM as it arrives, the WM
sees three "things happened" in a row instead of one composite gesture.
Worse, between the two `EV_ABS` events the pointer would briefly
jump to `(3214, old_y)` — our compose pass would render at the
wrong position for one frame.

The driver coalesces:

```c
static int32_t  g_last_ax = -1, g_last_ay = -1;   /* latched raw */
static uint32_t g_buttons = 0;

static void handle_event(const struct virtio_input_event *e)
{
    switch (e->type) {
    case EV_ABS:
        if (e->code == ABS_X) g_last_ax = e->value;
        else if (e->code == ABS_Y) g_last_ay = e->value;
        break;
    case EV_KEY:
        handle_button(e->code, e->value);    /* defers WM dispatch */
        break;
    case EV_SYN:
        flush_pending_motion();              /* one wm_pointer_move */
        break;
    }
}
```

`flush_pending_motion` only invokes `wm_pointer_move` if the screen
position actually changed — saturating drags that pin against a
window edge stop spamming events.

## A new "pump" hook for the syscall path

Chapter 48 introduced `pump_input_into_wm()` so that a GUI app
calling `gui_poll_event()` would still see keystrokes the kernel
had buffered.  The same trick now applies to the tablet, with a
crucial extension: the pump must run on **every** syscall a GUI
loop is likely to make:

```c
static void pump_input_into_wm(void)
{
    if (!wm_has_windows()) return;
    if (virtio_input_present()) {
        char c;
        while (virtio_input_try_getc(&c))
            (void)wm_keyboard_byte(c);
    }
    if (virtio_tablet_present())
        virtio_tablet_poll();
}

/* sys_yield AND sys_gui_poll_event call pump_input_into_wm. */
```

If you forget to wire the pump into a new syscall, GUI apps that
park on `yield()` between event polls will hang the moment the user
moves the mouse, because nothing else is draining the tablet
eventq.

## The window manager learns to point

`wm.c` now tracks four bits of pointer state:

```c
static int32_t  g_pointer_x = -1, g_pointer_y = -1;   /* screen px */
static uint32_t g_buttons   = 0;                      /* GUI_BTN_* */
static int32_t  g_drag_id   = -1;                     /* window id */
static int32_t  g_drag_dx, g_drag_dy;                 /* grab offset */
```

It exposes two new entry points to the tablet driver:

```c
void wm_pointer_move(int32_t sx, int32_t sy);
void wm_pointer_button(uint32_t button, int down);
```

### Hit-testing and zone classification

Two helpers carry the geometry knowledge:

```c
/* Topmost in-use window whose framed rect contains (sx, sy). */
static int32_t hit_test(int32_t sx, int32_t sy);

/* For a window we already know was hit, classify which zone:
 *   'C' = close button (top-right of title bar)
 *   'T' = title bar (drag handle)
 *   'B' = body (content; report cx,cy in window-relative coords)
 *   '-' = miss
 */
static char    classify_click(struct wm_window *w,
                              int32_t sx, int32_t sy,
                              int32_t *cx, int32_t *cy);
```

A 20-pixel-wide red close button is painted onto the title bar by
`blit_window`, with a white "×" drawn from two pixel-loop diagonals.
The same width feeds back into `classify_click` so the zone
boundary stays consistent with the visuals.

### Move: drag if dragging, dispatch otherwise

```c
void wm_pointer_move(int32_t sx, int32_t sy)
{
    if (g_pointer_x == sx && g_pointer_y == sy) return;
    g_pointer_x = sx; g_pointer_y = sy;

    if (g_drag_id >= 0) {
        struct wm_window *w = win_by_id(g_drag_id);
        if (w) { w->x = sx - g_drag_dx; w->y = sy - g_drag_dy; }
    } else if (g_focus_id >= 0) {
        struct wm_window *w = win_by_id(g_focus_id);
        int32_t cx, cy;
        if (w && classify_click(w, sx, sy, &cx, &cy) == 'B')
            wm_post_event(w, GUI_EVENT_MOUSE_MOVE, cx, cy, g_buttons);
    }
    compose_all();
}
```

Note that *every* pointer move recomposes — the cursor sprite has
to follow the pointer, and that's the cheapest correct way to do it
on a 1280×800 framebuffer.  When we add dirty-rect tracking in a
later chapter we will revisit this.

### Button: focus, raise, drag-start, dispatch, close

Left-click is the busy one:

```c
void wm_pointer_button(uint32_t button, int down)
{
    /* update bitmap regardless */
    if (down) g_buttons |= button; else g_buttons &= ~button;

    if (button == GUI_BTN_LEFT && !down) {
        g_drag_id = -1;            /* release ends drag */
        return;
    }
    if (button != GUI_BTN_LEFT)   /* M/R: forward only, no focus change */
        return;

    int32_t hit = hit_test(g_pointer_x, g_pointer_y);
    if (hit < 0) {                 /* clicked the wallpaper */
        g_focus_id = -1; compose_all(); return;
    }

    /* RAISE + FOCUS on every left-down. */
    struct wm_window *w = win_by_id(hit);
    w->z = ++g_next_z;
    g_focus_id = hit;

    int32_t cx, cy;
    char zone = classify_click(w, g_pointer_x, g_pointer_y, &cx, &cy);
    switch (zone) {
    case 'C':   /* close button */
        wm_post_event(w, GUI_EVENT_CLOSE, 0, 0, 0); break;
    case 'T':   /* start a title-bar drag */
        g_drag_id = hit;
        g_drag_dx = g_pointer_x - w->x;
        g_drag_dy = g_pointer_y - w->y; break;
    case 'B':   /* body click → app */
        wm_post_event(w, GUI_EVENT_MOUSE_DOWN, cx, cy, button); break;
    }
    compose_all();
}
```

Two design choices worth flagging:

- **Raise on every left-down, not just the title bar.** Clicking
  inside a buried window's content area should bring it to the top.
  The raise fires *before* zone dispatch so the app's own
  `MOUSE_DOWN` handler runs against an already-raised window.
- **`GUI_EVENT_CLOSE` is just an event, not a forced destroy.**
  The kernel does not unilaterally tear down a process's window
  when the user clicks ×; it tells the app, and the app calls
  `gui_destroy_window`.  This mirrors real-world behaviour
  (apps can show a "really quit?" prompt) and keeps the kernel
  out of policy.

### Drag-state bookkeeping on destroy

If an app destroys its window mid-drag (or just exits while you're
dragging it), `g_drag_id` would be left pointing at a freed window.
`wm_destroy_window` and `wm_destroy_owner` both clear it:

```c
if (g_drag_id == id) g_drag_id = -1;
```

Same trick for `g_focus_id`.  Forgetting this manifests as a NULL
`win_by_id(g_drag_id)` deref the next time the user moves the
pointer — easy to fix once you've seen it, hard to find if you
haven't.

## Cursor sprite

The sprite is a 12×19 monochrome bitmap stored in `.rodata`:

```c
static const char CURSOR_BITMAP[19][13] = {
    "1...........",
    "12..........",
    "122.........",
    "1222........",
    /* ... arrow shape ... */
    "12121.......",
    "11.121......",
    ".. .121.....",
};
```

`'1'` is white, `'2'` is black, `.` is transparent.  `blit_cursor`
runs at the very end of `compose_all`, after every window has been
composited, so the cursor always lands on top.  It clips against
the framebuffer bounds; no separate top-level clip path needed.

## Userspace: GUI_EVENT_MOUSE_* and GUI_BTN_*

`userspace/libc/syscall.h` mirrors the new event types and button
bits:

```c
#define GUI_EVENT_NONE        0
#define GUI_EVENT_KEY         1
#define GUI_EVENT_CLOSE       2
#define GUI_EVENT_MOUSE_MOVE  3   /* arg0=x, arg1=y, arg2=button bitmap */
#define GUI_EVENT_MOUSE_DOWN  4   /* arg0=x, arg1=y, arg2=button bitmap */
#define GUI_EVENT_MOUSE_UP    5   /* arg0=x, arg1=y, arg2=button bitmap */

#define GUI_BTN_LEFT          0x1u
#define GUI_BTN_RIGHT         0x2u
#define GUI_BTN_MIDDLE        0x4u
```

`(cx, cy)` are window-relative — the same coordinate system the app
already uses for `gui_present`, `gui_fill_rect`, and `gui_draw_text`.
Apps therefore never have to know where their window is on the
desktop or how big the title bar is.

## Userspace demo: `paint`

`userspace/paint/paint.c` is a 100-line demo that exercises every
new code path:

```c
int main(void) {
    int win = gui_create_window(600, 400, "paint");
    /* ... clear canvas to off-white, paint a help line ... */

    uint32_t colour = PALETTE[0];
    int dragging = 0;

    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) { yield(); continue; }
        switch (ev.type) {
        case GUI_EVENT_CLOSE:                            goto done;
        case GUI_EVENT_KEY:
            if ((char)(ev.arg0 & 0xFF) == 0x1B) goto done; break;
        case GUI_EVENT_MOUSE_DOWN:
            if (ev.arg2 == GUI_BTN_LEFT) {
                dragging = 1;
                stamp(win, ev.arg0, ev.arg1, colour);
            } else if (ev.arg2 == GUI_BTN_RIGHT) {
                colour = PALETTE[++palette_idx % PALETTE_LEN];
            } break;
        case GUI_EVENT_MOUSE_UP:
            if (ev.arg2 == GUI_BTN_LEFT) dragging = 0; break;
        case GUI_EVENT_MOUSE_MOVE:
            if (dragging && (ev.arg2 & GUI_BTN_LEFT))
                stamp(win, ev.arg0, ev.arg1, colour);
            break;
        }
    }
done:
    gui_destroy_window(win);
    return 0;
}
```

`stamp` paints a 12×12 BGRA square with `gui_fill_rect`, clips it
against the canvas, and calls `gui_flush` so the WM recomposes.

## The .bss-as-NOBITS detour

The paint app has a 600×400 BGRA canvas:

```c
static uint32_t pixels[600 * 400];   /* 960 000 bytes */
```

The *original* `userspace/linker_user.ld` merged `.bss` into `.data`
inside a single `PT_LOAD` segment.  That made `.bss` PROGBITS — the
canvas was emitted as 960 KB of zero bytes inside the on-disk ELF
binary, blowing past the OSFS-1 image's 1 MiB capacity:

```
make: image full: cannot fit paint (965192 bytes)
```

The fix is twofold:

1. Put `.bss` (and `COMMON`) in its own `PT_LOAD` segment marked
   `NOBITS`, so it has `p_filesz == 0` and the binary stops
   carrying the zero bytes.
2. Page-align that segment.  The kernel ELF loader maps each
   `PT_LOAD` page-by-page; if `.bss` shares a page with the tail of
   `.data`, both segments will try to allocate and map the same
   virtual page and the second `address_space_map` call silently
   fails or overwrites the first.

```ld
PHDRS
{
    load PT_LOAD FLAGS(7);   /* RWX text+rodata+data */
    bss  PT_LOAD FLAGS(6);   /* RW  NOBITS .bss      */
}

SECTIONS
{
    . = USER_LOAD_ADDR;
    .text : ALIGN(4K) { ... } :load
    .data : ALIGN(8)  { *(.data .data.*) } :load
    .bss  : ALIGN(4K) { *(.bss .bss.*) *(COMMON) } :bss
}
```

After the fix `paint.stripped.elf` is **5,192 bytes** instead of
960 KB.  `hellogui` likewise dropped from 620 KB to 5,664 bytes.
The OSFS-1 image goes from a tight 1212 sectors back to a
comfortable 12.

The bug surfaced as a hang during `/bin/sh`'s first prompt — the
shell's first writable static (its readline buffer) lived on a page
the loader had double-mapped, so the moment the shell tried to
write into it the page tables disagreed with reality and the CPU
ground to a halt without any visible exception.  Half an hour of
"why is sh frozen" was traced to a missing `ALIGN(4K)`.

## Headless smoke test

`scripts/test_tablet.py` boots QEMU with `-display none`, drives
the tablet through QMP, and screenshots the framebuffer at
checkpoints:

1. Wait for `mouse online` and `window manager ... ok` on serial.
2. Type `paint\n` on the virtio-keyboard via QMP.
3. Wait for `[wm] window created`.
4. Screenshot the FB → record the centre pixel of the canvas.
5. Send `abs x`, `abs y`, `btn left down`, eight more `abs x/y` events
   along a diagonal, `btn left up`.
6. Screenshot again → assert that ≥ 4 of 8 pixels along the drag
   line have changed colour.
7. Send a left-click at `(WIN_X + WIN_W - 10, WIN_Y + 10)` — the
   close button.
8. Screenshot → assert the centre pixel is no longer the
   post-paint colour (i.e. the window is gone and wallpaper shows
   through).

The QMP event format that does the work:

```python
{"execute": "input-send-event", "arguments": {"events": [
    {"type": "abs", "data": {"axis": "x", "value": 0x4000}},
    {"type": "abs", "data": {"axis": "y", "value": 0x2000}},
    {"type": "btn", "data": {"button": "left", "down": True}}
]}}
```

`"axis"` is a string `"x"`/`"y"`; `"value"` is in the device's own
range, so `0..0x7FFF` for QEMU's tablet.  Buttons are
`"left"`/`"right"`/`"middle"`/`"side"`/`"extra"` with `"down": bool`.

The full run looks like this:

```
$ python3 scripts/test_tablet.py
PASS: tablet + WM probed, shell ready
PASS: paint window opened
  centre pixel before drag: (248, 248, 248)
  drag-line pixels changed: 8/8
PASS: drag painted into canvas
PASS: close button worked (centre (192, 48, 48) -> (16, 20, 40))

MILESTONE 41: ALL TESTS PASSED
```

## Recap

After this milestone the WM is genuinely interactive:

- Keyboard and tablet probed and disambiguated by `EV_BITS / EV_ABS`.
- Pointer events are coalesced per `EV_SYN` frame.
- The pump runs on `sys_yield` and `sys_gui_poll_event` so apps
  using either pattern see input.
- Click-to-focus + click-to-raise + title-bar-drag + close-button-X
  all work and are tested headless.
- A 100-line `paint` app demonstrates the four new GUI events.
- The user linker script is now production-shaped: `.bss` is
  NOBITS in its own page-aligned `PT_LOAD`.

The next chapter lifts the WM into a real desktop: a wallpaper
process, a taskbar that lists open windows, and a start-menu that
can spawn binaries by clicking on them.

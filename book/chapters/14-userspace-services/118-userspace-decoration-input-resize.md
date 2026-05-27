# Chapter 118 — Decoration, the cursor, input routing, and resize

Chapter 117 retired the kernel compositor. The
screen now boots into a navy-blue wallpaper with
client pixel buffers blitted on top, raw, with no
title bars, no border, no cursor, no way to drag
a window, no close button, and no way to resize.
Every existing app runs and paints, but the
window-manager *experience* has regressed to
something worse than chapter 47.

This chapter rebuilds that experience entirely
inside `/bin/wsd`. After it lands:

* Every decorated window grows a 24-pixel title
  bar with the window title centred in it, a
  close button on the right, and a minimize
  button immediately to its left.
* A cursor sprite tracks the pointer in real
  time, painted by `wsd` without a full
  recompose per move.
* Title-bar drag works (left-press on the title
  bar starts a move; release commits).
* The close button delivers a `GUI_EVENT_CLOSE`
  to the owning client so apps shut down
  cleanly.
* The minimize button hides the window;
  clicking its taskbar cell brings it back.
* Resizable windows grow a 12×12 grip in the
  bottom-right corner; live-dragging it
  reallocates the backing buffer and the
  client receives a `GUI_EVENT_RESIZE` to
  repaint.
* `wsd` becomes the authority on pointer input
  routing: it hit-tests in its own z-order
  and injects `MOUSE_DOWN`/`MOUSE_UP`/
  `MOUSE_MOVE` events into the right client's
  queue.

That last bullet is the structural one. The
chapter 117 cutover left the kernel still in
charge of pointer routing — `wm_pointer_move` /
`wm_pointer_button` were still computing
hit-tests against the kernel window table.
Decoration paint lives in `wsd`, but
decoration *clicks* (close, minimize, resize
grip, title-bar drag) only make sense if the
process that drew the decoration is also the
one that interprets clicks on it. So we are
forced to move pointer hit-testing into `wsd`.

We do this in a way that keeps the kernel
input router alive but factored out of the
decision: the kernel still owns the per-window
event queue and `gui_poll_event` still returns
from it, but `wsd` decides *which* window's
queue an event lands in. The transitional
shape — the **input shadow** — is the most
interesting design decision in this chapter
and the reason it earns its own slot rather
than living as a sub-phase of 117.

## Why a hybrid model instead of moving input out wholesale

The clean architectural answer is to make
`wsd` the sole input owner — pull the
virtio-input IRQ handlers up to `wsd`, retire
every kernel-side event queue, deliver input
exclusively over IPC. That is the right
end-state.

We don't ship it in this chapter for three
reasons:

1. **It is a separate chapter's worth of
   work.** virtio-input lives in
   `kernel/device/virtio_input.c` and feeds
   bytes into `kernel/core/wm.c`'s pointer
   state machine. Both have callers
   outside the WM path (`/proc`-style
   debug, future joysticks). Moving them
   safely requires designing `/srv/input`
   and porting every client.
2. **`gui_poll_event` is everywhere.**
   Every GUI app's main loop calls
   `gui_poll_event` (now via
   `wm_poll_event`). Changing its
   underlying transport is a tree-wide
   churn that doesn't pay off until input
   is actually moved.
3. **The kernel router already works.**
   It owns the focused window, dispatches
   keyboard correctly, and the per-window
   event queue is sound. We don't need to
   rewrite it; we need to make `wsd` the
   one deciding *which window* an event
   reaches.

So we factor: the kernel routes events into
queues; `wsd` decides which queue an event
belongs in. Three new syscalls let `wsd`
become the deciding voice without touching
the queues themselves.

## The three new kernel syscalls

| Syscall | Purpose |
|---|---|
| `SYS_POINTER_STATE`           | Snapshot the kernel's current `(x, y, button_mask)`. `wsd` polls this every tick. |
| `SYS_GUI_DELIVER_EVENT`       | Inject a synthesised `gui_event` into a kernel window's event ring. `wsd` uses this to deliver `MOUSE_DOWN`, `MOUSE_MOVE`, `MOUSE_UP`, and `GUI_EVENT_CLOSE`. |
| `SYS_GUI_MOVE_WINDOW`         | Reposition a kernel-WM window so its hit rect tracks the `wsd` window after a drag. |
| `SYS_GUI_SET_INPUT_PASSTHROUGH` | Tell the kernel's hit-tester to ignore this window entirely — `wsd` will route every pointer event itself. |

The first three are the public mechanism. The
fourth is the load-bearing one. Without
passthrough, the kernel and `wsd` would both
try to route the same click and the user
would see double-fires; with it, `wsd` is the
single authority for any window that has
asked to be passthrough-routed.

Their declarations live in
[kernel/core/syscall.h](../../../kernel/core/syscall.h)
at numbers 89–92, and the implementations
in [kernel/core/wm.c](../../../kernel/core/wm.c)
near the bottom of the file, in a section
whose header reads:

```c
/* ---------------------------------------------------------------
 * Chapter 118 -- userspace decorations + cursor (wsd takes over)
 *
 * The four helpers below are the kernel API surface that wsd
 * leans on once it owns title-bar paint, drag, close-button paint,
 * and cursor-sprite paint.  Each one is intentionally tiny and
 * idempotent: this is the kernel's input/window contract with the
 * userspace compositor, nothing more.
 * --------------------------------------------------------------- */
```

The whole new surface is ~80 lines of kernel
code. That's the price for what would
otherwise be a much larger rewrite.

### Passthrough: one bit, two skip checks

The passthrough flag is one int on `struct
wm_window`, set by
`sys_gui_set_input_passthrough`, read by
exactly two paths in the kernel hit-tester:

```c
static int32_t hit_test(int32_t sx, int32_t sy)
{
    /* ... walk windows top to bottom ... */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        struct wm_window *w = &g_wins[i];
        if (!w->in_use) continue;
        if (w->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
        if (w->minimized) continue;
        if (w->input_passthrough) continue;   /* <-- new */
        /* ... point-in-rect check ... */
    }
}
```

And in the focused-window forwarding inside
`wm_pointer_move`/`wm_pointer_button`:

```c
} else if (g_focus_id >= 0) {
    struct wm_window *w = win_by_id(g_focus_id);
    if (w && w->input_passthrough) w = NULL;  /* <-- new */
    if (w) { ... }
```

That's the whole change. When passthrough is
on, the kernel hit-tester treats the window
as if it didn't exist. Every event still
*flows* through the kernel — `wm_pointer_move`
still tracks `(g_pointer_x, g_pointer_y)`,
the IRQ handler still updates state — but
no event is *routed* to a passthrough
window's event queue from the kernel. `wsd`
calls `gui_deliver_event` to put events in
that queue itself.

Keyboard input still uses the kernel's
`g_focus_id` so apps don't have to change
how they read keystrokes; `wsd` keeps focus
in sync by calling `gui_raise_window` after
each click-to-raise.

## The input shadow pattern

A `wsd`-only window has no kernel
representation, so it has no kernel event
queue and `gui_poll_event` against it
returns nothing. The first time a GUI app
ports to `wmclient` and tries to read input,
this becomes visible: the window appears,
the user clicks it, nothing happens.

The fix is the **input shadow**: alongside
the wsd window, the app opens a hidden
kernel-WM window with the same position,
the same dimensions, `GUI_WIN_FLAG_NO_DECORATION`,
and passthrough turned on. The kernel
shadow:

* exists in the kernel window table so it
  has an event queue;
* is invisible because it never gets a
  pixel buffer drawn into it (the kernel
  no longer composites anyway);
* is invisible to the kernel hit-tester
  because passthrough is set;
* tracks its `wsd` peer's position via
  `SYS_GUI_MOVE_WINDOW` after every drag
  so the body-rect coords stay aligned.

The shadow is what `wm_poll_event` reads
from. `wsd` knows about it via
`WM_WIN_BIND_KERNEL`, a wire op the
library uses immediately after
`wm_create_window` succeeds:

```c
/* req: a = wsd_window_id, b = kernel_window_id
 * rep: status only */
WM_WIN_BIND_KERNEL = 15,
```

When that op arrives in
[wsd.c::handle_bind_kernel](../../../userspace/wsd/wsd.c),
`wsd`:

1. Records the kernel id on the wsd
   window slot.
2. Calls
   `gui_move_window(kernel_id, w->x, w->y + WSD_TITLE_H)`
   to put the shadow's body under the wsd
   window's body.
3. Calls
   `gui_set_input_passthrough(kernel_id, 1)`
   so the kernel hit-tester ignores it.
4. Raises the wsd window in its own
   z-order and calls
   `gui_raise_window(kernel_id)` so the
   kernel keyboard focus follows.

The `wmclient` library hides all of this
behind one new entry point:

```c
int wm_create_window_input(uint32_t w, uint32_t h, uint32_t flags,
                           const char *title,
                           struct wm_window *out);
```

which internally is `wm_create_window` plus
`gui_create_window_ex(flags |
GUI_WIN_FLAG_NO_DECORATION, …)` plus
`WM_WIN_BIND_KERNEL`. Apps that take input
call this; apps that don't (`/bin/notify`,
the desktop wallpaper) keep using plain
`wm_create_window` and `wm_poll_event`
returns 0 every time.

### Why not just put input ops in the wire protocol?

We could have added
`WM_EVENT_DOWN` / `WM_EVENT_UP` /
`WM_EVENT_MOVE` / `WM_EVENT_KEY` and made
`wsd` forward events over IPC. We didn't,
because:

* The chapter-107 IPC bus is request/reply,
  not push. Streaming events requires
  either a per-client event-pull RPC (extra
  round-trip per input event, easily
  hundreds per second), or a new
  server-pushes-to-client primitive that
  doesn't exist yet.
* The kernel queue and `gui_poll_event`
  already do everything an event channel
  needs: bounded ring buffer, per-window
  isolation, blocking + non-blocking
  modes.
* The shadow can disappear cleanly in a
  future chapter that retires
  `gui_poll_event` entirely. Today's apps
  don't have to change.

The shadow is a transitional shape we will
unwind, but it carries no cost while it
exists: input has a clear flow and
debugging is easy because every event
still goes through the kernel queue an
operator can read.

## Decoration

With input plumbing settled, the visible
half of the chapter is straightforward.
`wsd` paints decoration using
`libgui/draw.h` from chapter 116 — the same
library every app uses for its own pixels.
The compositor doesn't care that the bytes
came from a button drawer rather than a
client paint loop.

The decoration constants live near the top
of [wsd.c](../../../userspace/wsd/wsd.c):

```c
#define WSD_TITLE_H        24u
#define WSD_CLOSE_BTN_W    20u
#define WSD_MIN_BTN_W      20u
#define WSD_BTN_GAP         2u
#define WSD_BTN_INSET       2u

#define WSD_DECO_BG_ACTIVE     0xff3a6ea5u  /* steel blue */
#define WSD_DECO_BG_IDLE       0xff556677u  /* dim gray-blue */
#define WSD_DECO_FG            0xffffffffu  /* white */
#define WSD_CLOSE_BG_IDLE      0xff883333u  /* darker red */
#define WSD_MIN_BG_IDLE        0xff336688u  /* navy-blue */
#define WSD_BORDER_COLOR       0xff202020u  /* almost black */
```

The body origin shifts down by the title
height:

```c
static inline uint32_t deco_top_h(uint32_t flags)
{
    return (flags & WM_WF_NODECORATION) ? 0u : WSD_TITLE_H;
}
```

A window flagged `WM_WF_NODECORATION` (the
desktop wallpaper, the taskbar, notify
popups) has `deco_top_h == 0` and `wsd`'s
behaviour for it is byte-identical to
chapter 117: paint the FB at `(x, y)`,
done. Every other window gets `WSD_TITLE_H`
pixels above the body for the bar.

### The decoration painter

`paint_decoration_clipped` is the single
function that draws a title bar plus its
two buttons:

```c
static void paint_decoration_clipped(const struct wm_window *w,
                                     int is_focused,
                                     int32_t cx, int32_t cy,
                                     int32_t cw, int32_t ch);
```

Internally it:

1. Picks `bg = is_focused ?
   WSD_DECO_BG_ACTIVE : WSD_DECO_BG_IDLE`.
2. `draw_fill_rect(scanout_fb, bar_x, bar_y,
   bar_w, bar_h, bg)` — fills the title
   bar.
3. Draws the title text via `draw_text`
   (which talks to `/srv/font` from
   chapter 115 for glyph cache lookups).
4. Paints the close button on the right
   (filled red rect with a white X drawn
   via `draw_hline`/`draw_vline`).
5. Paints the minimize button immediately
   to its left (filled navy rect with a
   white underscore).
6. Draws a one-pixel bottom border across
   the bar to separate it from the body.

The `_clipped` variant exists because the
cursor's save/restore loop (next section)
needs to repaint a small region of the
decoration without redrawing the whole
title bar. Every paint primitive — fill,
text, button — goes through a
`rects_intersect_clip` check first so
out-of-rect writes are no-ops. That makes
the function correct to call with *any*
clip rect, which is the property the cursor
path leans on.

The non-clipped wrapper is one line:

```c
static void paint_decoration(const struct wm_window *w, int is_focused)
{
    paint_decoration_clipped(w, is_focused,
                             0, 0,
                             (int32_t)g_scanout_w,
                             (int32_t)g_scanout_h);
}
```

i.e. "clip to the entire scanout" → nothing
gets clipped.

### Why the buttons can hide on narrow windows

`point_in_close_button` returns 0 if the
window is narrower than `WSD_CLOSE_BTN_W +
2 * WSD_BTN_INSET`, and `point_in_minimize_button`
similarly checks for room for both
buttons:

```c
static int point_in_close_button(const struct wm_window *w,
                                 int32_t px, int32_t py)
{
    if (w->flags & WM_WF_NODECORATION) return 0;
    if (w->w < WSD_CLOSE_BTN_W + 2u * WSD_BTN_INSET) return 0;
    /* ... rect test ... */
}
```

The paint side has the same guard. So a
window dragged to 30 pixels wide quietly
loses its buttons rather than drawing them
overlapping. The user can still drag the
window wider via the title bar; we don't
trap them in an unclosable state because we
also accept `GUI_EVENT_CLOSE` via the
keyboard escape path the apps already
handle.

## The cursor sprite

`wsd` ships its own X11-style left_ptr
cursor sprite, an 11×18 ASCII-art bitmap:

```c
#define WSD_CURSOR_W   11u
#define WSD_CURSOR_H   18u
static const char WSD_CURSOR_BITMAP[WSD_CURSOR_H][WSD_CURSOR_W] = {
    "X..........",
    "XX.........",
    "XOX........",
    "XOOX.......",
    "XOOOX......",
    /* ... */
};
```

`X` is black (border), `O` is white (fill),
`.` is transparent. The painter expands
this into 1–2 pixel touches per `O`/`X`
per frame.

### The naive painter and why it doesn't work

The obvious cursor approach is "paint the
cursor sprite at every compose call." That
works but has a serious flaw: the cursor
moves orders of magnitude more often than
anything else changes. At 60 Hz polling, a
sweeping cursor produces a
`compose_all`-per-tick load even if nothing
else changed. For a 1280×800 scanout that
is a 4 MB memcpy 60 times a second.

### The save-under model and why it's wrong here

The classic alternative is **save-under**:
before painting the cursor, copy the
pixels it covers into a scratch buffer.
Next frame, restore those pixels before
painting the cursor at the new location.
Two small memcpys instead of one giant
one.

This is what `wsd` originally shipped and
it has a subtle correctness bug. If
*anything else* — a window damage, a
title-bar repaint after focus change, a
wallpaper change — modifies pixels under
the cursor between save and restore, the
restore overwrites the new content with
the cursor's stale view of the screen.
The visible symptom: drag the cursor over
a focused window's title bar; the title
text disappears beneath the cursor's
"save" rect; move the cursor away; the
text comes back. Or worse, drag over a
neighbour's window and the neighbour's
pixels get smeared into your window when
the cursor leaves.

### The compose-based cursor model we ship

The model that actually works is **compose
just the cursor's rect from clean state**.
On a cursor move:

1. Compute the union of `(old_x, old_y,
   cursor_w, cursor_h)` and `(new_x, new_y,
   cursor_w, cursor_h)`.
2. Call `compose_rect(union)` — recompose
   exactly those pixels from wallpaper +
   windows in z-order.
3. Paint the cursor sprite at the new
   position.
4. `fb_present(union)` to flush.

```c
static void cursor_move_only(int32_t new_x, int32_t new_y)
{
    int32_t old_x = g_cursor_x, old_y = g_cursor_y;
    g_cursor_x = new_x; g_cursor_y = new_y;

    /* union rect */
    int32_t ux0 = min(old_x, new_x);
    int32_t uy0 = min(old_y, new_y);
    int32_t ux1 = max(old_x, new_x) + WSD_CURSOR_W;
    int32_t uy1 = max(old_y, new_y) + WSD_CURSOR_H;

    compose_rect(ux0, uy0, ux1 - ux0, uy1 - uy0);
    paint_cursor_sprite(new_x, new_y);
    fb_present(ux0, uy0, ux1 - ux0, uy1 - uy0);
}
```

A 1-pixel cursor move touches ~250 pixels;
a 10-pixel move touches ~500. Compare to
the 1M+ pixels a `compose_all` would touch.
Cursor smoothness is excellent and the
save-under correctness bug can't exist
because the cursor never sees a stale
copy of anything — it's always painted
over freshly composed pixels.

The cost is that `compose_rect` has to be
fast. It is: it walks the z-order top
down, and for each window it intersects
the requested rect against the window's
own rect on the scanout, then memcpys the
intersection. Most windows don't intersect
the cursor's union rect at all, so the
walk is fast in practice.

This same `compose_rect` primitive is what
`handle_damage` calls for client repaints,
so the same code path is exercised by
every typed character. It's the most
exercised function in `wsd` and we have
tests
([`scripts/test_cursor_over_window.py`](../../../scripts/test_cursor_over_window.py),
[`scripts/test_cursor_over_title.py`](../../../scripts/test_cursor_over_title.py),
[`scripts/test_cursor_over_browser.py`](../../../scripts/test_cursor_over_browser.py))
that drag the cursor over decorated
windows and assert the title text and the
window content are still intact after the
sweep.

## The input poller

The compose model and the input shadow
hook together inside a single function:

```c
static void poller_tick(void);
static void input_poller_thread(void *arg);  /* loop calling tick */
```

The poller runs as a dedicated thread
spawned at startup (after the FB is mapped,
before the accept loop), and ticks at ~60
Hz. Each tick:

1. `pointer_state(&x, &y, &btn)` — read
   the kernel's current cursor state.
2. If the cursor moved: call
   `cursor_move_only(x, y)`.
3. If the left button rose
   (`!prev_btn & btn`): hit-test against
   `wsd`'s z-order. A close-button hit
   delivers `GUI_EVENT_CLOSE` to the
   shadow's queue. A minimize-button hit
   hides the window. A title-bar hit
   starts a drag and raises the window.
   A resize-grip hit starts a resize. A
   body hit raises the window and
   delivers `MOUSE_DOWN` to the shadow.
4. If a drag is in progress and the
   cursor moved: update the window's
   `(x, y)`, call
   `gui_move_window(kernel_id, ...)` to
   keep the shadow's hit-rect aligned,
   and `compose_all` (only path where a
   cursor-move forces a full compose).
5. If a resize is in progress and the
   cursor moved: compute new dims, call
   `resize_apply(w, new_w, new_h)`.
6. If the left button fell
   (`prev_btn & !btn`): exit any drag,
   exit any resize, deliver `MOUSE_UP`
   to whichever shadow originally
   received `MOUSE_DOWN`.
7. If the cursor moved while a button is
   held: deliver `MOUSE_MOVE` to the
   press target (window-locked drag).
   If no button is held: deliver
   `MOUSE_MOVE` to the topmost window
   under the cursor.

The poller takes `g_wsd_lock` for the
duration of each tick so the table walk
and the event injections don't race the
accept-loop's window-creation handlers.

### Tracking the press target

When the user presses inside a window's
body, they expect the corresponding
release to count as a click on that
window even if they dragged the cursor
off it before releasing. The poller
tracks this with one variable:

```c
static int g_press_slot = -1;
```

set on left-button rising edge, cleared
on left-button falling edge. The
`MOUSE_UP` event goes to `g_press_slot`
even if the cursor is now elsewhere; the
matching `MOUSE_MOVE` events while held
also go to `g_press_slot` (window-locked
drag). This is what every desktop
windowing system does — apps assume
press-drag-release is atomic per window.

### Tracking the hover target

The cursor moving across a window's body
also needs to deliver `MOUSE_MOVE` events
so apps can implement hover effects
(button highlights, browser tooltips).
And when the cursor leaves a window, the
old window should see a "leave" event so
its hover state can clear:

```c
static int g_hover_slot = -1;
```

When the cursor enters a different
window, the poller delivers a synthetic
`MOUSE_MOVE` to the *previous* window
with coords translated to local —
typically negative or past the window
bounds, which any sane hit_test treats as
"no widget." Apps that care about precise
enter/leave compare coords against window
bounds; existing apps like the browser
and launcher use `hit_test(lx, ly)` and
get -1 for out-of-bounds, which is the
right thing.

## Live resize: the `win_fb` realloc path

Resizable windows (created with
`GUI_WIN_FLAG_RESIZABLE`) get a 12×12
grip painted in the bottom-right corner.
Dragging it should grow or shrink the
window in real time, the way every modern
GUI does.

This is the hardest path in the chapter
because it requires reallocating the
backing pixel buffer while both `wsd` and
the owning client have it mapped. The
fifth syscall we mentioned at the top of
chapter 117 lands here:

```c
/* Chapter 118 — resize an existing win_fb to (new_w, new_h).
 * Owner-only.  Reallocates the backing, copies the surviving
 * top-left of the old contents into the new buffer, uninstalls
 * every existing mapping (owner + mappers) since their VAs
 * back stale pages, frees old pages.  Owner and mappers must
 * re-call sys_win_fb_map to get a fresh VA; their old VA
 * returns translation-fault until they do. */
long sys_win_fb_resize(long id_arg, long new_w_arg, long new_h_arg);
```

The implementation in
[kernel/core/win_fb.c](../../../kernel/core/win_fb.c)
does five things in order, each of which
must succeed or the whole call rolls back:

1. Allocate the new page list from pmem.
2. Copy `min(old_w, new_w) × min(old_h, new_h)`
   rows from the old pages into the new.
3. Uninstall every existing mapping (owner
   + each entry in `mappings[]`) — every
   user VA pointing at the old buffer
   becomes invalid.
4. Install the new pages into the owner's
   AS, recording the new owner VA.
5. Free the old pages.

If step 1 fails: return `-ENOMEM`, old
buffer unchanged. If step 2/3/4 fail:
restore the original mappings (which we
still have records of) and return the
error. The kernel guarantees "FB is
unchanged on failure" so `wsd`'s
fault-handling is straightforward.

### Why uninstall all mappings

Step 3 looks aggressive — why not
re-install in place? Two reasons:

* The new page list typically has
  different physical pages than the old
  one (pmem doesn't guarantee any
  reuse), so the VAs would have to be
  rewritten anyway.
* Even if pages happened to match, the
  *number* of pages changes for any
  non-tiny resize, and the user VA
  range has to grow or shrink. Easier
  to uninstall and reinstall than to
  manage partial overlaps.

The contract `wsd` and the client agree
on is: after a `SYS_WIN_FB_RESIZE`, any
old user VA is invalid until the caller
re-calls `SYS_WIN_FB_MAP`. Touching the
old VA returns translation-fault. The
[wmclient::wm_window_remap_fb](../../../userspace/libgui/wmclient.c)
helper does this re-map for the client,
and the client must call it before its
next paint.

### The `resize_apply` flow in `wsd`

The poller's resize path calls into
`resize_apply` in
[wsd.c](../../../userspace/wsd/wsd.c):

```c
static int resize_apply(struct wm_window *w,
                        uint32_t new_w, uint32_t new_h)
{
    /* ... clamp to [WSD_RESIZE_MIN_W/H, scanout_w/h] ... */

    /* Kernel reallocates backing.  On success the old owner
     * VA is GONE (kernel uninstalled it).  We MUST re-map
     * before touching w->fb_va again. */
    int kr = win_fb_resize(w->fb_id, new_w, new_h);
    if (kr != 0) return 0;          /* old FB intact */

    /* Discover wsd's new owner-VA. */
    struct win_fb_map_args ma;
    int mr = win_fb_map(w->fb_id, &ma);
    /* ... handle map failure ... */
    w->fb_va = ma.va;
    /* update stride/size/w/h */

    /* Fill the grown region with WSD_DECO_BG_IDLE so the
     * user sees the window expand smoothly instead of
     * flashing black until the client repaints. */
    if (new_w > old_w || new_h > old_h) {
        /* ... two-strip fill ... */
    }

    /* Tell the client app the logical viewport changed AND
     * its old win_fb mapping is dead. */
    struct gui_event ev = {
        .type = GUI_EVENT_RESIZE,
        .window_id = w->kernel_id,
        .arg0 = new_w, .arg1 = new_h,
    };
    (void)gui_deliver_event(w->kernel_id, &ev);
    return 1;
}
```

Three details earn comment:

* **The grown-region pre-fill.** The
  kernel zero-fills new pages, so a
  freshly resized window's grown region
  is black until the client paints.
  Without the pre-fill, dragging a
  window larger produces a black L-shape
  that lingers for as long as the
  client takes to repaint (for the
  browser's parser thread, hundreds of
  ms; if the user keeps dragging,
  forever). The pre-fill uses
  `WSD_DECO_BG_IDLE` so the user
  perceives "the window is growing"
  rather than "the window is broken."
* **The FB is allocated once and never
  grows past that.** The window has
  separate logical `(w, h)` and FB
  `(fb_w, fb_h)`. Resize clamps logical
  to FB; the kernel realloc only fires
  when shrinking. This bounds memory
  pressure to whatever the app
  initially requested. Apps that want
  a generous resize range request a
  large window at create and shrink at
  startup. The browser does exactly
  this.
* **The event is delivered to the
  kernel shadow.** The client receives
  `GUI_EVENT_RESIZE` via its normal
  `wm_poll_event` loop; the same
  syscall path everything else uses.
  No new event-delivery transport.

### What the client does on `GUI_EVENT_RESIZE`

```c
case GUI_EVENT_RESIZE:
    if (wm_window_remap_fb(&g_win) < 0) {
        /* fb is now NULL; next draw is a clean NULL-deref */
        break;
    }
    repaint_everything(&g_win.fb);
    wm_window_dirty(&g_win, 0, 0, g_win.fb.w, g_win.fb.h);
    break;
```

`wm_window_remap_fb` is one wire op
(`WM_WIN_MAP_FB`) and one local syscall
(`SYS_WIN_FB_MAP`); on success it
updates `win->fb.pixels`, `stride`, `w`,
`h`. On failure it sets `pixels` to
NULL so an app that accidentally
paints during the bad tick gets a clean
null-deref rather than a silent
translation fault on the old VA.

### Postscript — transient IPC interruption during resize

A later stress run (rapid grow/shrink on a busy browser
window) exposed a second failure mode distinct from the
VA-freelist leak below: `wmclient` logged
`[wmclient] read err n=-4`, poisoned its `/srv/wm`
connection, and the app then repeatedly logged
`remap_fb failed; ignoring resize ...`.

The resiliency fix is in
[userspace/libgui/wmclient.c](../../../userspace/libgui/wmclient.c):

* retry `read`/`write` on transient `-EINTR` (`-4`) and
  `-EAGAIN` (`-11`),
* keep poisoning `g_conn` on hard errors,
* in `wm_window_remap_fb`, attempt `wm_connect()` when
  `g_conn < 0`.

This keeps resize handling robust when the transport is
briefly interrupted and belongs conceptually to chapter 112's
IPC error-handling discipline, but the symptom is easiest to
see in this chapter's resize loop.

### The VA freelist (a follow-up that became necessary)

After the initial resize implementation
landed, we noticed that hammering the
resize grip — drag-grow-drag-shrink in a
tight loop — would eventually leak user
VA space inside the kernel's
WM-window VA allocator. Each
resize allocated a new VA range; the
freed range was never returned to the
pool. After a few hundred resizes the
allocator ran out of VA and
`SYS_WIN_FB_MAP` started failing.

The fix is a tiny per-AS freelist in
[kernel/arch/address_space.c](../../../kernel/arch/address_space.c)
that the WM-window install/uninstall
helpers consult before bumping the
hwm pointer:

```c
/* WM-window VA freelist.  Each entry is one (va, npages)
 * span returned by uninstall_wm_window.  install_wm_window
 * pops a span before falling back to the bump pointer. */
```

The freelist is a fixed-size array
(64 entries — plenty for our workload),
and a full freelist just drops the
oldest entry — leaking a VA range, not
the pages it pointed at. The pages are
freed independently by pmem.

This is the kind of bug that hides
behind the assumption that the
allocator's hot path is "alloc, never
free." Resize broke that assumption and
made the leak visible. The fix is a few
dozen lines; the bug was a few hours of
debugging. The lesson is recorded as a
rule next to this chapter.

## What the tests pin

Decoration, cursor, input, and resize
each get dedicated tests, but the most
load-bearing one is
[scripts/test_cursor_over_title.py](../../../scripts/test_cursor_over_title.py).
It boots, opens a decorated window,
sweeps the cursor back and forth across
the title bar 20 times, and asserts:

1. The title text is still intact
   (specific pixels are still white,
   not blue).
2. The cursor is visible at its final
   position.
3. The window's body content is still
   intact (no smearing).

This single test catches every
correctness regression in the cursor
compose model, every clipping bug in
the decoration painter, and every
race in the input poller's drag
handling.

The full new-test list:

* `test_cursor_over_window.py` —
  cursor compose correctness over a
  client's pixel buffer.
* `test_cursor_over_title.py` —
  cursor compose correctness over
  `wsd`-drawn decoration.
* `test_cursor_over_browser.py` —
  cursor sweep over a content-rich
  window with frequent repaints.
* `test_window_resize.py` — drag the
  resize grip, assert dims change and
  client receives `GUI_EVENT_RESIZE`.
* `test_browser_resize_cycle.py` —
  the hammer test for the VA
  freelist: 100 grow-shrink cycles.
* `test_minimize.py` — click
  minimize, click taskbar cell,
  assert window returns.
* `test_wsd_paint_bugs.py`,
  `test_wsd_taskbar_raise.py`,
  `test_wsd_wallpaper_focus.py`,
  `test_wsd_bar_overlap.py`,
  `test_wsd_browser_resize.py`,
  `test_wsd_bugs.py` — focused
  regressions for bugs that surfaced
  during bring-up.

Every existing GUI test from chapter
117 also keeps passing — the cascade
positions, body coords, and
decoration heights are now what they
will be for the rest of the book.

## Prerequisites

* [Chapter 116](116-gui-sdk-userspace-drawing.md)
  — `libgui/draw.h`. `wsd` calls
  `draw_fill_rect`, `draw_text`,
  `draw_hline`, `draw_vline` to paint
  decoration.
* [Chapter 117](117-userspace-window-server.md)
  — the daemon, the wire protocol,
  the `wmclient` library, the
  `win_fb` kernel primitive. This
  whole chapter builds on top of
  those.
* Chapter 37 — virtio-tablet. The
  kernel pointer state machine the
  poller reads from.
* Chapter 48 — the in-kernel WM
  pointer router. The shadow pattern
  reuses that router with one new bit.

## Applied to

* **Every existing GUI app gets
  decoration "for free."** The
  ported app source from chapter
  117 doesn't change — the title
  bar appears because `wsd` paints
  it, not because the app does.
* **Every app that handles input
  switches from
  `wm_create_window` to
  `wm_create_window_input`** so the
  kernel shadow exists and `wsd`
  knows about it. The diff is one
  line per app.
* **`/bin/paint`** gains
  right-click handling via
  `GUI_EVENT_MOUSE_DOWN` with the
  `RIGHT` button bit, since the
  poller now forwards right clicks
  through to the press-target
  window.
* **`/bin/browser`** uses the
  resize path. Its main loop
  handles `GUI_EVENT_RESIZE`,
  remaps the FB, recalculates
  layout, and repaints.
* **`/bin/taskbar`** uses
  `WM_WIN_RESTORE` to bring
  minimized windows back. The
  taskbar cells call it
  unconditionally on click; `wsd`
  treats it as both "un-minimize"
  and "raise to top."

## What this unlocks

* **Themes.** The decoration
  colours are file-scope constants
  today. A future chapter that
  reads them from
  `/etc/wsd/theme.conf` and
  reloads on SIGHUP is small.
* **Per-window focus indication
  beyond decoration colour.** A
  drop shadow on the focused
  window, an outline on the
  hover-target — any of these are
  paint additions in
  `wsd_compose_all`, no new
  kernel surface needed.
* **A real input migration
  ([`/srv/input`](../14-userspace-services/112-ipc.md)).**
  The shadow is a transitional
  shape we can unwind once `wsd`
  becomes the only consumer of
  pointer events. The kernel
  router becomes a forwarder to
  `/srv/input` and the shadow
  table goes away.
* **A second window manager.**
  A tiling `swsd` could bind
  `/srv/wm` instead of this
  decoration-heavy one. The
  protocol is the contract; the
  decoration policy is a `wsd`
  implementation detail.

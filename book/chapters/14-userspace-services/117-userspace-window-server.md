# Chapter 117 — The window server moves to userspace

Chapters 114, 115 and 116 each pulled one thing
out of the kernel: window pixel buffers, font
rendering, then per-app rasterisation. After 116
the kernel still owned the **compositor** — the
loop that walked the window list, blitted each
window's pixels into the framebuffer, drew title
bars on top, and flushed to virtio-gpu. That is
roughly 2 KLOC of `kernel/core/wm.c` running at
EL1 every time a window damaged a pixel.

This chapter retires the compositor. After it
lands the kernel still owns the framebuffer device
and the input devices, but every pixel that
reaches the scanout is written by a normal
userspace daemon called `wsd` (window-server
daemon). The kernel keeps a window list and an
input router — see [chapter 118](118-userspace-decoration-input-resize.md)
for why those are not gone too yet — but it does
no rasterisation, no compositing, no cursor
sprite, and no decoration. `kernel/core/wm.c`
shrinks by ~700 lines in one slice; the kernel
TTF rasteriser bridge `kernel/core/wm_font.c`
becomes unreachable from any drawing path; the
"GUI" reduces to "scan out this list of buffers."

Before we start, the orientation table from the
previous chapter, extended with the new row:

| Chapter | What moved out of the kernel | What's still in the kernel after |
|---|---|---|
| 114     | window pixel buffers, via shared mapping | rasterisation, fonts, compositor, decoration |
| 115     | font rendering, via `/srv/font`          | rasterisation, compositor, decoration |
| 116     | per-app rasterisation, via `libgui/draw` | compositor, decoration |
| **117** | **the compositor**                        | **the input router and the window list** |

This is the largest single architectural slice in
Part XIV. We will land it in five interlocking
pieces:

1. A userspace daemon (`/bin/wsd`) that holds the
   scanout buffer.
2. A new kernel primitive (`win_fb`) that lets
   one process allocate a pixel buffer and a
   second process map the same physical pages.
3. A wire protocol (`/srv/wm`) for clients to
   ask `wsd` to do things on their behalf.
4. A client library (`libgui/wmclient`) that
   wraps the wire protocol so apps that ported
   to `libgui/draw.h` in 116 only need a
   six-line change to point at `wsd`.
5. A cutover: rewrite every existing GUI app to
   use `wmclient`, stub the kernel render
   syscalls to no-ops, and ship.

## Why this is worth a chapter

The pure code-deletion case is strong — about 2 KLOC
of kernel out, replaced by ~1.5 KLOC of userspace
daemon — but the architectural cases are stronger:

1. **The trust boundary stops mattering for GUI
   bugs.** A WM bug today can corrupt kernel
   memory or hang every process behind the
   in-kernel WM lock. A `wsd` bug crashes
   `wsd`, init respawns it, every client
   transparently reconnects (chapter 113's
   supervisor pattern carries the weight).
2. **The compositor can have a debugger
   attached.** `gdb /bin/wsd` works the moment
   `wsd` is a userspace process. Today
   debugging compositor bugs means
   instrumenting kernel printk and rebooting.
3. **Themes, tiling alternatives, remoting all
   become possible.** Once the compositor is a
   userspace process talking a documented wire
   protocol, a second compositor can be
   written without rebuilding the kernel. We
   don't ship any of those in this chapter,
   but the design no longer prevents them.
4. **It closes the loop on chapter 115's
   awkwardness.** The whole reason
   `kernel/core/wm_font.c` exists is that the
   compositor draws title bars and the
   compositor lives in the kernel. Moving the
   compositor out makes that bridge
   unnecessary instead of "necessary but
   acceptable."

## Why this comes after 116, not before

Two reasons:

1. **`wsd` needs `libgui/draw.h`** (chapter 116)
   to draw its decorations and wallpaper.
   Without it, `wsd` would either re-implement
   rasterisation (duplicating what 116
   built) or call syscalls back into the
   kernel (defeating the point of moving
   out).
2. **Client apps need to already be using the
   mapped-buffer path.** If clients still
   called `SYS_GUI_FILL_RECT`, those syscalls
   would have to either be retained (forcing
   `wsd` to expose a syscall-shaped facade)
   or be ported in the same chapter.
   Chapter 116 does the porting; this
   chapter inherits the result.

The ordering `108 → 114 → 115 → 116 → 117` is
fixed. None of the intermediate steps are
throwaway — each one is a viable place to stop and
ship.

## The destination, in one picture

```
                       userspace                          kernel
                       ─────────                          ──────

  /bin/launcher                                       ┌───────────────┐
  /bin/notepad      ─── libgui/draw ─── pixel writes  │ scanout pages │
  /bin/browser                                        │  (one BGRA    │
  /bin/desktop                                        │   framebuf)   │
  ...                                                 └───────┬───────┘
       │                                                      │
       │                                                      │
       │  wmclient.h: connect, create, dirty                  │
       │                                                      │
       ▼                                                      ▼
  /bin/wsd  ─── compose loop:                          ┌─────────────┐
              walk window table,                       │ virtio-gpu  │
              blit each FB to scanout,                 │  driver     │
              call fb_present(rect)  ─────────────────►│             │
                                                       └─────────────┘
```

`wsd` holds three things in its address space:

* the scanout pages (mapped via `SYS_FB_MAP_SCANOUT`);
* every client window's BGRA pixel buffer
  (allocated as `win_fb` objects, then
  `SYS_WIN_FB_MAP`'d into `wsd`'s AS);
* a userspace window table that records each
  window's id, owner, position, geometry, FB
  id, mapped VA, and the cfd of the IPC
  connection that owns it.

The kernel keeps the framebuffer device, the
virtio-input drivers, and (for now) a slimmed
input-routing path. Everything else is pushed
across the EL boundary.

## The four new kernel primitives

You can almost do this chapter without changing
the kernel at all. The primitives we already have
from earlier chapters — `mmap`, `fork`, page
tables, `srv_bind`, IPC datagrams — are enough to
talk a protocol and share memory. But two specific
needs are not yet served:

1. There is only one scanout buffer in the
   system, and only one process should own it
   at a time. There must be a one-shot "claim
   the framebuffer" syscall whose lifetime is
   tied to a pid, so a `wsd` crash releases it
   automatically.
2. The 114 path
   (`SYS_GUI_MAP_WINDOW`) is inverted from what
   we need: it lets a client map a buffer the
   *kernel* allocated. Now we want a daemon to
   allocate buffers it owns and let *other*
   processes map them.

That is what `wsd_fb` (the scanout claim) and
`win_fb` (the per-window pixel buffers) are for.
Plus `SYS_FB_PRESENT` so `wsd` can flush a rect to
the GPU, since the kernel WM that used to call
`fb_present` is being deleted.

The four new syscalls and what each does, in one
table:

| Syscall                | Owner   | Purpose |
|------------------------|---------|---------|
| `SYS_FB_MAP_SCANOUT`   | first caller | Map the virtio-gpu scanout buffer RW into the caller's AS. First-caller-wins; subsequent callers get `-EBUSY`. Auto-released on `thread_exit`. |
| `SYS_WIN_FB_ALLOC`     | caller becomes "owner" | Allocate N pages from pmem to back a `w × h` BGRA buffer, install them RW in caller's AS, return `{id, va, stride, size}`. Owner pid is recorded. |
| `SYS_WIN_FB_MAP`       | any caller with the id | Install the same physical pages into the caller's AS at a fresh VA. Tracked per-mapper. |
| `SYS_WIN_FB_FREE`      | owner only | Tear down every mapper's installation, then free the pages. |
| `SYS_FB_PRESENT`       | any caller | Tell virtio-gpu to re-scan a rect of the scanout. |

(There is a fifth, `SYS_WIN_FB_RESIZE`, that
[chapter 118](118-userspace-decoration-input-resize.md)
needs for live window resize; we will leave it
out of this chapter and ship it then.)

### Why a separate `win_fb` table at all

The 114 primitive
`address_space_install_wm_window` already maps a
list of physical pages into an AS with the right
descriptor bits. We could put the per-window
table inside `wsd` itself and let the kernel just
expose "install these N PAs into my AS." We
deliberately don't, for three reasons:

* **Sharing is a kernel concern.** Two processes
  with read/write access to the same physical
  pages is a privilege escalation if either
  side could lie about owning them. The
  kernel-owned table is the single source of
  truth on who owns what.
* **Teardown has to be kernel-mediated.** When
  a client dies, its mapping has to be
  uninstalled from its dying AS before the
  reaper destroys the AS — otherwise we'd
  either double-free the page or leave it
  unreachable. The kernel-side
  `win_fb_release_pid` hook in `thread_exit`
  does this in one pass.
* **The right ACL primitive isn't a capability
  yet.** Until osdev has capabilities, the
  kernel keeping `owner_pid` per FB is the
  cheapest correct ACL. The chapter-107 IPC
  channel becomes the *de facto* capability:
  `wsd` only hands out the FB id to the conn
  that created the window.

The implementation lives in
[kernel/core/win_fb.c](../../../kernel/core/win_fb.c)
and [kernel/core/win_fb.h](../../../kernel/core/win_fb.h).
A fixed-size table of 64 backing objects, each
with `{id, owner_pid, owner_as, owner_va, pages[],
n_pages, mappings[4]}`. Pages come from
`pmem_alloc_page` (no contiguity requirement),
get zero-filled, and are tagged with
`DESC_SW_WM_WINDOW` (chapter 114's bit) so
AS-teardown and `fork` skip them.

### The exit hook that prevents use-after-free

`win_fb_release_pid` runs from `thread_exit`,
**before** the reaper destroys the address space.
The walk does two things in one pass:

* For any FB whose `owner_pid` matches the
  exiting thread: same as `sys_win_fb_free` —
  uninstall every mapping, free pages, clear
  slot. The owner's own AS is passed in as
  `skip_as` so we don't recursively touch the
  AS that is about to be torn down.
* For any FB where one of `mappings[]` matches
  the exiting thread's AS: just clear the
  tracking slot. We do *not* call
  `address_space_uninstall_wm_window` — the AS
  is already past the final yield and its L3
  tables are about to be freed. AS-teardown
  skips `DESC_SW_WM_WINDOW` descriptors
  anyway, so the pages themselves aren't
  pmem-freed through that path.

Without that hook, a sequence like

```
client thread_exit                              ← clears mapping slot? NO
reaper destroys client AS
... later ...
wsd: gc_conn_windows → sys_win_fb_free
    → uninstall_wm_window against the
      destroyed AS pointer → use-after-free
```

would dereference freed kernel memory. With the
hook, the slot is gone by the time `wsd` tries to
uninstall, and the FREE path skips it.

This is the trap that every "shared-memory between
two userspace processes" subsystem has to solve
once. Get it wrong and the bug surfaces as a
kernel oops whenever a GUI app crashes; get it
right and the system tolerates arbitrary client
death without `wsd` even noticing.

### The scanout claim is simpler

`SYS_FB_MAP_SCANOUT` (in
[kernel/core/wsd_fb.c](../../../kernel/core/wsd_fb.c))
is a one-page module by comparison. The kernel
remembers the first pid to call it; subsequent
callers from other pids get `-EBUSY`. The
holding pid sees idempotent returns of the same
descriptors. A `thread_exit` hook
(`wsd_fb_release_owner`) clears the slot if the
owner thread is exiting.

Why first-caller-wins instead of a capability
list? The OS has no capability mechanism yet, and
inventing one for a single client is overkill. In
the steady state there is exactly one `wsd`; if a
second tries to claim it, that is a bug, and
`-EBUSY` is the right answer.

## The wire protocol: `/srv/wm`

`wsd` binds the well-known path `/srv/wm` using
chapter 112's `srv_bind`. The protocol is
defined in
[userspace/libc/wm_proto.h](../../../userspace/libc/wm_proto.h)
and shared between `wsd`, the `wmclient`
library, and the `wmtest` smoke client.

Every message is a fixed-size 24-byte header.
Some replies carry a payload immediately after
the header inside the same IPC datagram. The 24
bytes are:

```c
struct wm_msg {
    uint32_t op;        /* WM_OP_*                              */
    int32_t  status;    /* reply only; 0 success or WM_ERR_*    */
    uint32_t a, b, c, d;
};
```

A handful of fields per message turns out to be
enough for everything in this chapter. When we
need to carry a string (a window title) or an
array (a list of window descriptors), the bytes
ride after the header — but the header itself
never grows.

The op table for the chapter as shipped:

| Op | Direction | Payload | Reply |
|---|---|---|---|
| `WM_HELLO`        | C → S | `a` = client_version              | `a` = session_id, `b` = `wsd` version |
| `WM_LIST`         | C → S | —                                 | `a` = n_windows, payload: `n × wm_win_desc` |
| `WM_WIN_CREATE`   | C → S | `a` = w, `b` = h, `c` = flags     | `a` = win_id, `b` = auto_x, `c` = auto_y |
| `WM_WIN_CREATE_AT`| C → S | `a` = w, `b` = h, `c` = flags, `d` = (x<<16) \| y | same as CREATE |
| `WM_WIN_DESTROY`  | C → S | `a` = win_id                      | status only |
| `WM_WIN_MAP_FB`   | C → S | `a` = win_id                      | `a` = fb_id, `b` = w, `c` = h, `d` = stride |
| `WM_WIN_DAMAGE`   | C → S | `a` = win_id, `b` = src_x, `c` = src_y, `d` = `WM_DAMAGE_PACK_WH(w,h)` | status only |
| `WM_WIN_MOVE`     | C → S | `a` = win_id, `b` = x, `c` = y    | status only |
| `WM_WIN_TITLE`    | C → S | `a` = win_id, `b` = title_len, payload: title bytes | status only |

A few protocol design choices worth spelling out:

* **Strictly monotonic window ids.** A
  destroyed id is *never* reused for the
  lifetime of the `wsd` process. The reason is
  that input routing (introduced in
  [chapter 118](118-userspace-decoration-input-resize.md))
  uses window ids as map keys; without
  monotonicity, a stale focus reference could
  silently steer events to the wrong client.
  Monotonicity makes a stale id resolve to
  "no such window" rather than "wrong
  window."
* **Window-local damage coordinates.**
  `WM_WIN_DAMAGE`'s `(src_x, src_y)` are inside
  the window's own FB, not on the scanout.
  `wsd` translates by adding the window's
  position. This matches `libgui`'s existing
  `gui_window_dirty(fb, x, y, w, h)` signature
  from chapter 116, so the client library
  doesn't have to remember positions.
* **`WM_WIN_CREATE` returns a cascade
  position.** Each new window gets an
  `(x, y)` from a 40-px cascade starting at
  `(100, 100)` so successive windows don't
  pile up. Clients that want a specific
  position use `WM_WIN_CREATE_AT` (the
  desktop wallpaper at (0,0), the taskbar at
  (0, screen_h - bar_h)). Cascade memory is
  trivial; the algorithm is a mid-1990s
  Motif/CDE constant-step cascade with
  wrap-on-overflow.
* **One conn, many windows.** Clients open a
  single `/srv/wm` connection at startup and
  multiplex every window over it. The conn
  is identified by its `cfd` inside `wsd`,
  and every window remembers `owner_cfd`.
  Cross-conn ops (a different connection
  trying to destroy a window it does not
  own) get `WM_ERR_NOTOWNER`.

### Why the protocol does not return the pages

`WM_WIN_MAP_FB` returns the FB id, geometry and
stride. It does *not* return the user VA. The
client uses the id to call the local
`SYS_WIN_FB_MAP` syscall, which is what actually
installs the pages.

Splitting the work in two has three benefits:

1. **The IPC payload stays small.** No page
   list passes over the wire.
2. **The kernel is the single ACL gate.** Only
   the kernel knows whether the calling AS is
   allowed to map a given FB.
3. **A future capability layer is easy.** A
   token that wraps `fb_id` slots in without
   changing either the wire protocol or the
   syscall signature.

This is the same shape Wayland uses with
`wl_shm`: the server tells the client "here is a
shared-memory handle" and the actual install is
a separate syscall.

## The compositor inside `wsd`

`wsd` is a single-threaded event loop. It serves
the IPC conn (accept, read, dispatch, write,
read) and runs an input-poller loop (covered in
[chapter 118](118-userspace-decoration-input-resize.md));
both call into the compose path when the screen
needs to change.

The minimal compositor is in
[userspace/wsd/wsd.c](../../../userspace/wsd/wsd.c).
Three primitives:

```c
static void paint_wallpaper(void);                  /* fill scanout */
static void blit_full_window(const struct wm_window *w);
static void wsd_compose_all(void);                  /* the whole frame */
```

The compose loop is straightforward — a
back-to-front walk over the window list:

```c
static void wsd_compose_all(void)
{
    paint_wallpaper();
    int painted = 0;
    for (int s = 0; s < WM_MAX_WINDOWS; s++) {
        if (!g_windows[s].in_use) continue;
        blit_full_window(&g_windows[s]);
        painted++;
    }
    fb_present(0, 0, 0, 0);          /* flush whole scanout */
    printf("[wsd] compose_all painted=%d\n", painted);
}
```

That's it. The picture-frame algorithm from
chapter 47's in-kernel WM, translated to
userspace.

`blit_full_window` walks each row of the window's
mapped FB and copies it into the scanout pages
(both are in `wsd`'s AS, so this is a pure
memcpy). `paint_wallpaper` fills the back-buffer
with a single colour (`WSD_WALLPAPER_BGRA =
0xff112233`, a dark navy chosen distinct from
every app's debug colours so a "nothing painted"
screen is unmistakable).

`fb_present(0, 0, 0, 0)` is the all-zero
shortcut for "flush the whole screen." Targeted
damage paths pass the actual rect.

### Single-rect compose: the fast path

Recomposing the whole screen on every damage
would be wasteful, especially once apps start
animating. `wsd` actually has a second compose
primitive that paints only a rect:

```c
static void compose_rect(int32_t rx, int32_t ry,
                         int32_t rw, int32_t rh);
```

Internally it walks the windows in z-order and
clips each window's blit to the rect. The detail
that matters is that `compose_rect` is what
`handle_damage` calls in the hot path — one
chapter-107 datagram comes in, one rect of pixels
goes out, one `fb_present(rect)` ships the
change to the GPU. Every typed character, every
mouse-moved cursor, every browser-rendered glyph
flows through `compose_rect` exactly once.

The implementation is plain — `for each row, for
each window, clip and memcpy` — and the
[wsd.c::compose_rect](../../../userspace/wsd/wsd.c) source has the
intersection arithmetic in full. The point is
that the *whole* compositor is now ordinary C in
a userspace process. It can be profiled, gdb'd,
and replaced.

## The client library: `wmclient`

The wire protocol is small but tedious to use
directly. Every app would need to repeat the
same five steps at startup: connect, hello,
create, map, install. The library
[userspace/libgui/wmclient.h](../../../userspace/libgui/wmclient.h)
and
[wmclient.c](../../../userspace/libgui/wmclient.c)
bundles them.

The header surface is small enough to quote:

```c
struct wm_window {
    struct gui_fb fb;            /* same layout as 116 gui_fb */
    uint32_t      id;            /* wsd window id; 0 == not live */
    uint32_t      fb_id;         /* kernel win_fb id */
    uint32_t      x, y;          /* scanout-relative origin */
    int32_t       kernel_id;     /* see chapter 118 */
};

int  wm_connect(void);
int  wm_create_window(uint32_t w, uint32_t h, uint32_t flags,
                      struct wm_window *out);
int  wm_window_dirty(struct wm_window *win,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  wm_window_move (struct wm_window *win, uint32_t x, uint32_t y);
int  wm_destroy_window(struct wm_window *win);
```

Four design choices in the API worth calling out:

* **One connection per process.** `wm_connect`
  stashes the conn fd and the assigned
  session id in file-scope `static` globals.
  `wm_create_window` calls `wm_connect`
  itself, so a client that only ever wants
  one window never touches the connect API
  directly.
* **`struct gui_fb` embedded as the first
  field.** Every chapter-116 drawing
  primitive takes a `struct gui_fb *`.
  Putting `gui_fb` first inside
  `struct wm_window` makes `&win->fb`
  interchangeable with the kernel-WM gui_fb
  that every app already uses. The port
  recipe in the next section relies on this.
* **CREATE folds in MAP_FB and the kernel
  install.** Three round-trips collapse into
  one library call. Errors anywhere in the
  three trigger a best-effort
  `WM_WIN_DESTROY` so `wsd` doesn't end up
  holding a half-created window.
* **Idempotent destroy.**
  `wm_destroy_window(&win)` is a no-op when
  `win.id == 0`, and it zeros every field
  after destroying. An app's exit path can
  call destroy unconditionally without a
  guard.

These are not accidents — they are the
affordances we want for the long-tail port that
follows. The fewer things each ported app has
to think about, the closer the port gets to a
mechanical search-and-replace.

### A library note: freestanding C

`wmclient.c` runs in userspace but on our
freestanding C environment (no libc). Two traps
worth flagging:

* `struct wm_msg req = { 0 };` triggers an
  implicit `memset` call that doesn't exist
  in freestanding userspace.
  Every field of every staging struct is
  initialised explicitly.
* The library uses `"../libc/printf.h"`, the
  freestanding-safe printf used by the rest
  of the tree. Format strings stay inside
  its supported subset (`%u`, `%d`, `%lx`,
  `%s`).

### Resiliency follow-up (May 2026)

Later resize-storm testing found a client-side IPC
failure chain worth recording in this chapter.

Observed sequence:

1. `wmclient` saw `read err n=-4` (transient
   interruption),
2. it immediately poisoned `g_conn`,
3. `wm_window_remap_fb` failed repeatedly,
4. browser logged `remap_fb failed; ignoring resize ...`
   while the window kept resizing.

The fix lives in
[userspace/libgui/wmclient.c](../../../userspace/libgui/wmclient.c):

* `wm_send` and `wm_recv` retry on transient
  `-EINTR` (`-4`) and `-EAGAIN` (`-11`) with `yield()`,
* hard failures still poison `g_conn`,
* `wm_window_remap_fb` attempts `wm_connect()` when
  `g_conn < 0`.

This is fundamentally chapter-107 discipline at a
chapter-117 call site: framed datagrams remove message
boundary ambiguity, but interrupted I/O still needs a
retry loop in clients.

## The first new client: `/bin/hellowsd`

We want to test the library against a brand-new
app before retro-fitting existing ones, so any
library bug surfaces in a controlled greenfield
client rather than a 2,000-line browser. That
client is
[userspace/hellowsd/hellowsd.c](../../../userspace/hellowsd/hellowsd.c)
and it is intentionally the smallest thing that
exercises the full path:

```c
int main(void)
{
    struct wm_window win;
    if (wm_create_window(300, 200, 0, &win) < 0) exit(1);

    /* fill the FB through the mapped pointer */
    uint32_t *p = win.fb.pixels;
    for (uint32_t i = 0; i < win.fb.w * win.fb.h; i++)
        p[i] = 0xff7755aa;

    wm_window_move (&win, 200, 120);
    wm_window_dirty(&win, 0, 0, 4, 1);   /* the test pin */
    wm_destroy_window(&win);
    puts("[hellowsd] PASS");
    return 0;
}
```

The magic colour `0xff7755aa` and the post-move
position `(200, 120)` are chosen distinct from
the `wmtest` smoke client's `0xff332211` and
`(100, 50)`. That gives both test harnesses
disjoint signatures in `wsd`'s log even though
they exercise the same code path.

What `hellowsd` deliberately omits:

* No event loop — input routing is the next
  chapter.
* No text rendering — that would pull in the
  `/srv/font` dependency from chapter 115
  and dilute the question this app is here to
  answer.
* No close button, no resize grip — those are
  also next chapter.

The test harness is
[scripts/test_hellowsd.py](../../../scripts/test_hellowsd.py).
Nine PASS-line assertions covering: init
launched `wsd`, `wsd` printed its banner, `wsd`
bound `/srv/wm`, the shell reached its prompt,
hellowsd printed PASS, the library logged its
session id, the cascade returned `(100, 100)`,
`wsd` processed `WM_WIN_MOVE win=1 to=200,120`,
and the load-bearing line

```
[wsd] damage win=1 src=0,0,4,1 dst=200,120,4,1 px=0xff7755aa
```

That single damage line proves the whole stack:
the IPC connection worked, the library composed
the right wire op, `wsd` decoded it, the window
position was applied to translate window-local
coords to scanout coords, the per-window FB and
the scanout were the same physical pages (else
the source would be zero), and the scanout was
RW from `wsd` (else the blit would fault).

## The cutover: porting every existing GUI app

`hellowsd` exists; `wmclient` works; `wsd`
composes. None of that is visible to a user
until the kernel's compositor stops running and
the existing apps start drawing through `wsd`.
That is the long-tail work, and we do it as a
single chapter-spanning slice because trying to
land it incrementally would leave the system
half-broken on the screen.

### The port recipe

Every app gets the same six-line mechanical
change.
[notepad](../../../userspace/notepad/notepad.c)'s
diff is the canonical example; the others are
the same shape:

```c
struct wm_window g_win;                         /* was: int g_win_id; */

if (wm_create_window_input(WIN_W, WIN_H, 0,
                           "notepad", &g_win) < 0) {
    write(1, "[notepad] wm_create_window_input failed\n", 41);
    exit(1);
}

draw_fill_rect(&g_win.fb, 0, 0, WIN_W, WIN_H, BG);
draw_text     (&g_win.fb, x, y, msg, FG, BG, 1);

wm_window_dirty(&g_win, 0, 0, WIN_W, WIN_H);

while (wm_poll_event(&ev) > 0) { /* handle */ }

wm_destroy_window(&g_win);
```

`wm_create_window_input` is the next-chapter
sibling of `wm_create_window`; it opens the wsd
window *and* a shadow kernel-WM window so input
keeps flowing while the kernel input router is
still in charge. Apps that don't take input
(`/bin/notify`, the desktop wallpaper) use plain
`wm_create_window`. Apps with a fixed scanout
position (the desktop, the taskbar) use
`wm_create_window_at`.

### What gets stubbed in the kernel

After the port, no app in the tree calls
`gui_fill_rect`, `gui_draw_text`, `gui_present`
or `gui_flush` any more. Their kernel handlers
are reduced to no-ops returning success:

```c
long wm_present  (uint64_t pid, int32_t id, ...) { return 0; }
long wm_fill_rect(uint64_t pid, int32_t id, ...) { return 0; }
long wm_draw_text(uint64_t pid, int32_t id, ...) { return 0; }
long wm_flush    (uint64_t pid, int32_t id)      { return 0; }
```

We keep the syscall numbers reserved (and the
stubs callable) so a future app that wants to
poke pixels into a kernel-WM window keeps
working, and so the `mixtest` invariant — "a
mapped window can't also be drawn via the legacy
syscalls" — can still be checked. The full
predicate stays:

```c
struct wm_window *w = win_by_id(id);
if (w && w->in_use && w->owner_pid == pid && w->user_pages_n != 0)
    return -EBUSY;
return 0;
```

When `user_pages_n` is non-zero the FB is mapped
to user; the legacy call returns `-EBUSY` so a
buggy mixed-mode app fails loudly. Otherwise the
call is a harmless no-op (returns 0 without
painting).

### What gets deleted from the kernel

* `paint_wallpaper`, `blit_window`,
  `blit_cursor` — the entire scanout paint
  path.
* `wm_draw_text_fb` — the FB-side glyph
  drawer used only by `blit_window` for
  title-bar text.
* `wm_blend_pixel` — the per-pixel alpha
  blender used only by the deleted text
  drawers.
* `compose_all` (the real one).
* `CURSOR_BITMAP[]` — the hand-rolled X11
  left_ptr cursor sprite; the chapter 118
  one in `wsd` replaces it.
* `g_wm_painted_wallpaper` — the one-shot
  bit that gated kernel wallpaper painting.

About 700 lines net.

### One sleight-of-hand: the `compose_all` stub

`compose_all()` was called from ~12 sites in
`kernel/core/wm.c` — every window-table
mutation, every focus change, every input
event. We could surgically remove each call
site. We don't. Instead `compose_all` becomes:

```c
static inline void compose_all(void) { }
```

Three reasons:

* Surgery risks subtle control-flow bugs at
  every call site (early-return paths,
  conditional branches, comments referencing
  "compose_all already called inside").
* The stub is one inline function that the
  compiler folds away to nothing.
* The call sites are exactly where a future
  chapter (probably the one that adds
  in-flight WM state broadcast to `wsd`)
  will want to notify the daemon. Keeping
  the calls means that future diff is
  "rename `compose_all` to
  `wsd_publish_state` and fill in the body"
  instead of "go find every spot the kernel
  used to need to notify and add a new
  call."

This is the second time in osdev the same
pattern has paid off — see chapter 88's
spinlock no-op stubs.

## The boot sequence

`wsd` is supervised by `init` the same way fontd
and clipboardd are. The supervisor table in
[userspace/init/init.c](../../../userspace/init/init.c)
gains:

```c
supervise("/bin/wsd", "");
```

inserted **after** fontd (so the future
decoration painter doesn't race the daemon it
depends on at boot) and **before** the GUI apps
(so by the time any client reaches
`gui_create_window`, `/srv/wm` is alive).
`SUPERVISED_MAX` bumps from 4 to 6 to leave
headroom for future daemons.

A wsd crash now tears down two resources
atomically: the `/srv/wm` endpoint (chapter 112
auto-release) and the scanout-owner slot
(`wsd_fb_release_owner` from the exit hook).
The supervisor respawns; the new instance sees
a clean slate. Any client mid-RPC gets `-EPIPE`
on its next read or write and reconnects.

`wmclient` now retries transient interrupted
read/write returns and reconnects on demand in the
remap path, so a brief `/srv/wm` hiccup no longer
turns into a permanent resize failure for that app.

### Boot-daemon ordering and the cascade

`init` launches the four GUI daemons in this
order:

1. `/bin/desktop` (full-screen via
   `wm_create_window_at(0, 0, …)` — no cascade
   slot consumed).
2. `/bin/taskbar` (bottom strip via
   `wm_create_window_at` — no cascade slot
   consumed).
3. `/bin/launcher` (cascade slot 0 →
   `(100, 100)`).
4. `/bin/sh`.

The desktop and taskbar use `WM_WIN_CREATE_AT`
precisely so they don't perturb the cascade
counter. The first user-spawned cascade client
therefore lands at slot 1 = `(140, 140)`, not
`(100, 100)`. A handful of pixel-pinned tests
shifted their sample coordinates to match this
fact (see
[scripts/test_pixapp.py](../../../scripts/test_pixapp.py)
for the canonical update). The lesson for any
future GUI test: the `(x, y)` in the test must
be the scanout coords the app actually wrote,
not the window-local coords plus a guessed
decoration height.

## A latent kernel bug the cutover surfaced

The port surfaced a kernel bug that had been
sitting harmlessly until now. `wsd`'s worker
threads (spawned by `thread_spawn_files`) get
their stacks from `mmap(MAP_ANONYMOUS)`, which
is lazy: pages don't exist until first touch.
When `wsd`'s first IPC `read` landed and the
kernel's `copy_to_user` walked into that fresh
stack, the EL1 memcpy itself translation-faulted
because the page wasn't mapped at all yet.

The same bug fires for any forked app touching
its inherited RO-mapped buffer: EL1 RO
permissions apply to the kernel too, so a
kernel memcpy through a COW page faults with a
permission abort.

The fix lives in
[kernel/core/uaccess.c](../../../kernel/core/uaccess.c):

```c
static int prefault_user_write(
    struct address_space *as, uint64_t va)
{
    if (!as) return -1;
    /* Lazy anon mmap: install the page if there's a
     * VMA but no PTE.  No-op if already mapped. */
    (void)address_space_handle_mmap_fault(as, va,
                                          /*is_write=*/1);
    /* COW: if RO + DESC_SW_COW, break the share. */
    return address_space_make_writable(as, va);
}

static void prefault_user_read(
    struct address_space *as, uint64_t va)
{
    if (!as) return;
    (void)address_space_handle_mmap_fault(as, va,
                                          /*is_write=*/0);
}
```

`copy_from_user` calls `prefault_user_read` for
each page in the source range; `copy_to_user`
calls `prefault_user_write` and turns its `-1`
into `-EFAULT`. The syscall boundary becomes
COW-aware and lazy-mmap-aware without changing
the boundary's contract: callers still see
`-EFAULT` for genuinely bad pointers, just no
longer for genuinely good but not-yet-mapped
ones.

This is the kind of bug that only shows up when
the kernel starts taking arbitrary pointers from
arbitrary user threads in arbitrary states.
Single-threaded apps with bss-allocated buffers
hide it; `wsd` exposes it on the first request.

## A Makefile gotcha the cutover surfaced

`WMCLIENT_OBJ := $(BUILD)/userspace/libgui/wmclient.o`
and the matching `DRAW_OBJ` had been defined late
in the libgui block, after the first per-app link
recipes that consumed them. Make's `:=` snapshots
the value at the point of definition, so the
first few apps linked against an empty
`$(WMCLIENT_OBJ)` / `$(DRAW_OBJ)` and silently
came up with unresolved symbols when `wsd` was
the target.

Fixed by hoisting both `:=` assignments to the
top of the libgui group, before any consumer. The
fix and a comment block live in
[Makefile](../../../Makefile). The rule for future
ports: Make's "most specific wins" tiebreaker requires
that pattern-specific rules sit after the generic ones.

## What's still in the kernel after this chapter

Reading `kernel/core/wm.c` at the end of 117
makes the boundary clear:

* The window table (`g_wins[]`), slot
  allocation, owner-pid tracking, focus
  tracking (`g_focus_id`).
* The chapter-114 backing-buffer machinery
  (`wm_map_window`, `wm_unmap_window`,
  `wm_damage`, `wm_drop_user_pages`).
* `g_pointer_x/y`, `g_buttons` — the
  pointer-state state machine, still updated
  by `wm_pointer_move` / `wm_pointer_button`.
* Per-window event rings — `wm_keyboard_byte`
  still enqueues input events.
* The stubbed legacy render calls
  (`wm_present`, `wm_fill_rect`, `wm_draw_text`,
  `wm_flush`) — all return 0.
* `wm_measure_text` and the kernel font
  bridge (`kernel/core/wm_font.c`) — kept
  because `/bin/notify` calls
  `wm_measure_text` to compute popup width
  before painting. (`/bin/notify` ports to
  the new path in chapter 118, at which
  point both of these can go.)

About 1,300 lines of WM remain at the end of
this chapter. Compare to ~2,050 at the start.
Almost half the in-kernel WM is gone, and
nothing in the remaining half rasterises a
single pixel.

The input router stays at EL1 for one
practical reason: clients still call
`gui_poll_event` against the kernel queue, so
the kernel has to be the one filling the
queue. Moving that out is the work of a future
chapter; chapter 118 takes a first step by
introducing the "input shadow" pattern that
lets `wsd` route some events without owning
the queue.

## Prerequisites

* Chapter 112 — IPC, the bus underneath
  `/srv/wm`.
* Chapter 113 — the supervisor pattern that
  keeps `wsd` respawning.
* Chapter 114 — `SYS_GUI_MAP_WINDOW`.
  Conceptually `win_fb` is its inverse: in
  114 the kernel allocated, the client
  mapped; in 117 a userspace daemon
  allocates, both the daemon and the
  owning client map.
* Chapter 115 — fontd. `wsd`'s eventual
  decoration text (chapter 118) uses it
  via libgui.
* Chapter 116 — `libgui/draw.h`. `wsd`
  uses it to paint wallpaper and (in
  118) decoration; clients use it for
  content.

## What gets exercised in tests

* `scripts/test_wsd_smoke.py` — the
  chapter's foundational test. Boots,
  watches for `[wsd] mapped FB`, asserts
  `wsd` stays up for five seconds without
  the supervisor noticing a death.
* `scripts/test_wsd_hello.py` — protocol
  smoke. Drives `wmtest` through every
  wire op and asserts the readback BGRA
  round-trip pattern arrives at the right
  scanout coord.
* `scripts/test_hellowsd.py` — library
  smoke. Drives `/bin/hellowsd` and pins
  the same one-line damage log shape.
* Every existing GUI test — `test_pixapp`,
  `test_notepad`, `test_browser_*`,
  `test_gui_term`, `test_taskbar`,
  `test_launcher`, `test_paint_drag`,
  `test_wm`, `test_notify`,
  `test_boot_to_desktop` — keeps running
  through the cutover but each one had to
  be updated to use the new (scanout-coord,
  no-decoration) pixel positions, since
  the cascade offsets and the absent
  title bar shifted things. Test updates
  are a few lines each.

After the cutover the sweep stands at
**79/79 PASS**. The kernel WM is now an
input-routing + window-list service; every
pixel on the screen flows through `wsd`.

## Applied to

* **Every existing GUI app** — `/bin/notepad`,
  `/bin/gui_term`, `/bin/browser`,
  `/bin/launcher`, `/bin/taskbar`,
  `/bin/desktop`, `/bin/paint`,
  `/bin/notify`, `/bin/pixapp` — ported to
  `wmclient`. Source diffs are small; the
  draw-loop bodies are unchanged.
* **New app:** [userspace/hellowsd/hellowsd.c](../../../userspace/hellowsd/hellowsd.c)
  — the smallest possible new client,
  there to prove the library works on a
  greenfield user.
* **New supervised daemon:** `/bin/wsd`.
* **New test:** `scripts/test_hellowsd.py`.
* **New tests:**
  `scripts/test_wsd_smoke.py`,
  `scripts/test_wsd_hello.py` — protocol
  + lifecycle coverage.
* **Existing tests upgraded:**
  `test_pixapp.py`, `test_truetype.py`,
  `test_boot_to_desktop.py`, `test_wm.py`,
  `test_hellowsd.py`, `test_wsd_hello.py`
  (id and session non-determinism made
  the assertions content-pinned rather
  than id-pinned).

## What this unlocks

* **[Chapter 118](118-userspace-decoration-input-resize.md)**
  — userspace title bars, the cursor sprite,
  drag-to-move, the close button, the
  minimize button, the resize grip, and the
  hybrid kernel-shadow / wsd input model
  that lets click-to-focus work without
  rewriting the kernel input router yet.
* **Themes.** Drop a config file, restart
  `wsd`, decorations change. Future
  chapter material.
* **A second compositor.** A tiling-only
  `swsd` could swap in by binding the
  same `/srv/wm` path. The OS no longer
  imposes one window-manager design.
* **`ps` shows `wsd` like any other
  service.** Today the WM is invisible to
  `ps` because it's threads in the
  kernel. After this chapter it appears
  in `ps` like fontd and clipboardd.
* **Crash recovery.** A wsd bug crashes
  wsd, not the kernel. The supervisor
  respawns. Once `wmclient` learns
  `EPIPE`-reconnect, the desktop will
  flicker rather than freeze when wsd
  dies.

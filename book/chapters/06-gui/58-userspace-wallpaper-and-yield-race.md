# Chapter 58 — A userspace wallpaper, and the yield/IRQ race it
# uncovered

This milestone has two halves.  The first is an architectural
correction: wallpapers belong in userspace, not in the kernel.
The second is a 16-year-old race condition in the scheduler that
went undetected for six dozen chapters because the test load was
never busy enough to trigger it — and that the new wallpaper
process surfaced on the very first interactive boot.

## Half one: a real wallpaper, in a real process

Chapter 54 painted a gradient on the framebuffer from inside the
kernel.  Chapter 56 swapped that for a JPEG-converted BGRA blob
linked into the kernel image via `objcopy -I binary -O elf64`.
Both were expedient and both were wrong.

Wallpapers are *content*.  Content does not belong in the kernel
for the same reason that `/etc/wallpaper.png` doesn't belong in
the Linux source tree:

- the kernel image bloats by however many megabytes the picture
  takes,
- you can't change the wallpaper without recompiling,
- there's no way for a desktop process to manage themes, fade
  between images, animate a parallax background, or do any of the
  other things real desktops do, because the bytes are owned by
  ring 0.

The fix is structural.  We:

1. Add a single new flag to the window manager — `PIN_TO_BOTTOM`
   — that marks a window as forever beneath every other window
   and transparent to mouse hit-testing.  Eight lines of code.
2. Move the BGRA blob from the kernel image onto the OSFS at
   `/mnt/wallpaper.bgra`.  One Makefile line and one
   `mkosfs.py` table entry.
3. Write `/bin/desktop` (~170 LOC) that creates a screen-sized
   `PIN_TO_BOTTOM | NO_DECORATION` window at `(0, 0)`, opens
   `/mnt/wallpaper.bgra`, reads an 8-byte little-endian header
   `(width, height)`, then streams the pixels into the window
   via `gui_present` 16 rows at a time.
4. Have `init` spawn `/bin/desktop` *first*, before
   `/bin/launcher` and `/bin/taskbar`, so the wallpaper window
   has the lowest z value and the taskbar/launcher cleanly
   sit on top.

After this change, `kernel.elf` shrinks back to ~370 KB (from
~9 MB with the wallpaper embedded) and `wallpaper.bgra` lives
on disk where it belongs.  Swapping the wallpaper is a matter
of rebuilding `assets/osfs/wallpaper.bgra` — no kernel touch.

### The PIN_TO_BOTTOM flag

```c
/* userspace/libc/syscall.h */
#define GUI_WIN_FLAG_NO_DECORATION  0x1u
#define GUI_WIN_FLAG_ALWAYS_ON_TOP  0x2u
#define GUI_WIN_FLAG_PIN_TO_BOTTOM  0x4u   /* NEW */
```

The kernel mirrors the flag and updates two places:

- **The compositor's painter** (`compose_all` in
  `kernel/core/wm.c`) goes from a two-pass painter
  (regular → always-on-top) to three passes (pin-to-bottom →
  regular → always-on-top).  Each pass picks the
  lowest-z eligible window not yet drawn and blits it.
  16 windows, three passes, O(N²) — N=16 means it doesn't
  matter.
- **Hit testing** (`hit_test`) skips PIN_TO_BOTTOM windows
  entirely so clicks fall through to whatever app is below
  the cursor.

A single flag handles "this is the wallpaper" semantics fully.
We deliberately *don't* introduce a separate "wallpaper" or
"background" type — the WM should not know what wallpapers
are, only how to draw windows in z order and route clicks.

### Reading the BGRA blob without knowing the resolution

The host script that prepares the wallpaper —
`scripts/img_to_bgra.py` — converts whatever PNG/JPEG you
give it to a packed BGRA byte stream.  Before this milestone
the on-disk file was bare pixel data with no metadata, and the
kernel had to be recompiled to a matching `WALLPAPER_W ×
WALLPAPER_H`.

Now the script writes:

```python
header = struct.pack("<II", W, H)   # 8 bytes
out.write(header)
out.write(pixels)
```

and `/bin/desktop` reads the eight bytes first:

```c
/* userspace/desktop/desktop.c */
uint32_t hdr[2];
if (read_full(fd, hdr, sizeof(hdr)) != sizeof(hdr)) {
    puts("[desktop] short header");
    return 1;
}
uint32_t img_w = hdr[0], img_h = hdr[1];
```

Centring is now resolution-tolerant.  At 1920×1080 with a
1920×1080 image, the offsets are zero.  At 1280×800 with a
1920×1080 image, the desktop clips to the screen and centres
the visible portion.  At 1920×1080 with a 1280×800 image, the
desktop draws the image centred with the screen-coloured
margin showing around it.

### Streaming, not slurping

A 1920×1080 BGRA wallpaper is 8.3 MB.  `/bin/desktop`'s user
heap is more than big enough — but allocating 8 MB just to
turn around and send it to the kernel via 1080 separate
`gui_present` calls would waste an entire copy.

Instead the desktop reads the image one chunk of `ROW_CHUNK
= 16` rows at a time into a stack-sized buffer
(16 × 1920 × 4 = 120 KB on its 2 MB user heap), `gui_present`s
that strip into the window, and loops.  Total user-heap
footprint: ~120 KB regardless of screen size.

### The Makefile glue

Two Makefile changes.  First, the resolution becomes a
configurable `FB_RES`:

```make
FB_RES   ?= 1920x1080
FB_XRES  := $(word 1,$(subst x, ,$(FB_RES)))
FB_YRES  := $(word 2,$(subst x, ,$(FB_RES)))

WALLPAPER_W  := $(FB_XRES)
WALLPAPER_H  := $(FB_YRES)
```

`make run-graphical FB_RES=1280x800` rebuilds the wallpaper at
1280x800 and tells QEMU to scan out at 1280x800.  With a
default `FB_RES=1920x1080` you get full HD.

Second, the desktop binary and the wallpaper blob both go onto
the OSFS:

```make
$(DISK_IMG): ...
        python3 scripts/mkosfs.py \
            --out=$@ \
            ... \
            desktop=$(DESKTOP_STRIPPED) \
            wallpaper.bgra=$(WALLPAPER_BIN)
```

After this milestone, the OSFS holds 29 files in a 16 MB image,
of which the wallpaper is the largest single file by far
(8.3 MB).  We bumped `TOTAL_SECTORS` from 8192 to 32768 to
accommodate it.

## Half two: SYS_GUI_GET_SCREEN_SIZE

The first interactive boot at 1920×1080 produced a desktop
that was, charitably, 67% of a desktop.  The taskbar sat at
y = 772 (because someone hardcoded `BAR_Y = FB_H - BAR_H` with
`FB_H = 800`), the notify toasts hugged a column 360 px from
the *intended* right edge instead of the actual one, and the
wallpaper itself filled the screen because the desktop process
queries the BGRA header.

Three userspace processes had been hardcoded to 1280×800 since
chapter 55.  We needed a runtime query.

```c
/* kernel/core/syscall.h */
SYS_GUI_GET_SCREEN_SIZE = 50,

/* kernel/core/syscall.c */
static long sys_gui_get_screen_size(long out_w_uptr, long out_h_uptr)
{
    if (!fb_is_ready()) return -EINVAL_VFS;
    const struct fb_info *fb = fb_get_info();
    uint32_t w = fb->width, h = fb->height;
    if (out_w_uptr) copy_to_user(out_w_uptr, &w, sizeof(w));
    if (out_h_uptr) copy_to_user(out_h_uptr, &h, sizeof(h));
    return 0;
}
```

Three callers.  Three two-line changes.  Now everyone agrees
on the actual resolution and the GUI fills the entire scanout
no matter what `FB_RES` you pick.

## Half three: the bug that wasn't supposed to be there

With M50 functionally complete and the resolution issue fixed,
`make run-graphical` produced a perfect desktop for 1.5
seconds — and then panicked, hard, in the kernel.  Always with
the same fingerprint:

```
ESR_EL1  = 0x000000008a000000
  EC     = 0x0000000000000022  PC alignment fault
ELR_EL1  = 0x00000002300121bf
SPSR_EL1 = 0x0000000031d01cc5
x29 = 0x0000000020000345     <-- *that's an SPSR-shaped value*
x10 = 0x00000010001028cc     <-- *that's a userspace PC*
x30 = 0x0000000230012180
```

Two clues stood out.

First, `x29 = 0x...0345`.  Bit pattern 0x345 is exactly the
synthesized SPSR_EL1 our cooperative `cswitch_to` writes for
kernel threads (`M=EL1h, F=A=D=1, I=0`).  Finding it in the
*frame pointer* slot means the saved-frame's offset 232 (which
is where x29 lives) was overwritten with what looks like an
SPSR value.

Second, `x10 = 0x10001028cc` — a user-space program counter.
Userspace text lives in `0x10000000..0x10100000`.  x10 in the
saved frame is at offset 80.  Finding a user PC there means
the frame is the residue of some user thread's SVC entry.

Putting both clues together: `cswitch_to` was unspooling a
frame from a memory location that *didn't have a valid frame
in it* — instead it had random stack bytes from some user
thread's previous SVC call, including a saved user PC and what
used to be an SPSR_EL1 value.

### How a saved frame becomes garbage

`yield()` (in `kernel/core/thread.c`) ends with a critical
section:

```c
next->state = THREAD_RUNNING;
g_current   = next;
if (next->as != prev->as)
    address_space_activate(next->as);
cswitch_to(&prev->sp, next->sp);
```

The intent is: "make `next` the current thread, switch to
its stack."  The first three lines do the bookkeeping; the
fourth line does the actual stack swap.

There is a window — measured in microseconds — between
`g_current = next` and the `mov sp, x16` inside `cswitch_to`
where:

- The **logical** current thread is `next`.
- The **physical** SP still points into `prev`'s kernel stack.

If the timer IRQ fires anywhere in that window, the
exception entry in `vectors.S` does what it always does:
push 272 bytes of frame onto the current stack.  *That stack
is `prev`'s.*  Then `irq_dispatch` runs, the timer handler
calls `schedule()`, schedule calls `yield()`, and the
recursive yield reads `g_current` — which is `next`.

The recursive yield treats `next` as the outgoing thread.
It picks something else, possibly the original `prev`, sets
that to RUNNING, calls `cswitch_to(&next->sp, prev->sp)`.
Inside the inner cswitch, the line that matters is:

```asm
mov     x17, sp
str     x17, [x16]      /* *save_sp = sp — saves into next->sp! */
```

This stores the *current* SP — which is on `prev`'s kernel
stack, mid-IRQ — into `next->sp`.  A pointer to a frame
that's about to be overwritten by `prev`'s continued
execution.

The inner cswitch then loads `prev->sp`, returns into prev's
yield, completes prev's `cswitch_to`, and life continues.
Until later when somebody schedules `next` again — at which
point `cswitch_to` reads `next->sp`, looks at the frame
contents (now garbage from prev's stack), restores ELR/SPSR
from heap-shaped bytes, and `eret`s into oblivion.

### The cure

Mask IRQs for the entire scheduling window.  `cswitch_to`'s
final `eret` restores the destination thread's SPSR, which
has `I = 0`, so IRQs come back on automatically the moment
we land on the new stack:

```c
void yield(void)
{
    uint64_t saved_daif;
    __asm__ volatile("mrs %0, daif" : "=r"(saved_daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");

    drain_stack_to_free();
    /* ... wake sleepers, pick next, requeue prev ... */

    if (!next) {
        __asm__ volatile("msr daif, %0" :: "r"(saved_daif) : "memory");
        return;
    }

    next->state = THREAD_RUNNING;
    g_current   = next;
    if (next->as != prev->as)
        address_space_activate(next->as);

    cswitch_to(&prev->sp, next->sp);
    /* eret restored SPSR with I=0; no manual unmask needed. */
}
```

Three lines.  10/10 stress-test runs pass deterministically
afterwards, where 0/10 passed before.

### Why didn't this fire before?

The race window is tiny — a handful of cycles between two
memory writes.  At 100 Hz a timer interrupt has roughly a 10 ms
period; the probability of landing *in* the window on any
given yield is on the order of 10⁻⁷.  But:

- The shell sits in `vfs_read`'s
  `while (!console_try_getc(&c)) yield();` cooked-mode loop.
  At idle, it yields tens of thousands of times per second.
- Add a launcher polling for events, a taskbar polling for
  events, a desktop process polling for events, and that's
  four kernel threads each yielding several thousand times
  a second.

Even at 10⁻⁷ probability per yield, with 40k yields/second
the expected time to a hit is on the order of 250 ms.  Up
through chapter 57 we never had four yield-heavy processes
running concurrently.  Adding `/bin/desktop` was the sliver
that pushed the system over.

### The diagnostic dance

The frustrating part of this kind of bug is that adding
*any* code between `g_current = next` and `cswitch_to`
makes the panic disappear. The trap shows up early:
a frame-validation check inserted "to print a diagnostic
when the frame looks bad" turned 0/5 into 5/5. The cost of
the check itself was sufficient to close the race window
enough that the timer never landed inside it.

The lesson: races that disappear when you add prints
*haven't been fixed*; they've been camouflaged.  The right
move is to keep the diagnostic, study the timing, and then
write a real fix that doesn't depend on the cost of
`serial_putc`.

## Postscript: why `desktop` busy-yields

The first version of `desktop`'s "stay alive forever" loop
looked exactly the way an introductory OS book would tell you
to write it:

```c
for (;;) {
    struct gui_event ev;
    while (gui_poll_event(&ev)) { /* handle */ }
    sleep_ms(500);
}
```

This is sensible. The desktop has nothing to do until something
happens; sleeping for 500 ms is the polite cooperative thing
to do. That's the first cut.

A couple of milestones later, a user reported: "the cursor is
smooth while at least one window is open, but very jerky when
I close every window." That sentence has the answer in it, but
the connection is easy to miss.

Recall how the cursor sprite gets repainted. The
virtio-tablet's used-ring fills as the host pointer moves.
**Nothing in the kernel polls the used-ring on its own.**
Instead, the syscall layer has a helper:

```c
static void pump_input_into_wm(void)
{
    if (virtio_input_present()) {
        char c;
        while (virtio_input_try_getc(&c))
            (void)wm_keyboard_byte(c);
    }
    wm_flush_pending_keys();
    if (virtio_tablet_present())
        virtio_tablet_poll();   /* drains used-ring, may call wm_pointer_move → compose_all */
}
```

…and that helper is invoked from exactly two places: `sys_yield`
and `sys_gui_poll_event`. So the cursor's effective sample rate
is "however often somebody calls one of those two syscalls".

When an app is open, that app's event loop is

```c
while (gui_poll_event(&ev) || (yield(), 1)) { ... }
```

— thousands of yields per second, none of them sleeping. The
cursor is buttery smooth because the tablet is being drained
basically as fast as the scheduler can run.

The moment the user closes the last window, the only kernel-
resident yielders left are `init` (blocked in `waitpid`) and
`desktop` (the wallpaper owner). And `desktop` was sitting in
`sleep_ms(500)`, which means it was waking up and yielding
*twice per second*. That's exactly the sample rate of the
cursor — twice per second of host motion gets collapsed into
one ten-pixel jump on the screen.

The temptation is to fix this in the kernel: drain the tablet
from the timer IRQ, bump the timer rate, add a kernel
compositor thread. Two of those approaches were attempted
before the realisation that they would require fixing a
different bug first -- `virtio_gpu`'s
descriptor submission uses static `g_avail_idx_seen` /
`g_used_idx_seen` counters that aren't safe to enter
concurrently. An IRQ-context `compose_all` interrupting a
userspace `gui_present` mid-`fb_present` corrupts the
descriptor ring. M58's repeat-stress harness caught the
regression on the second run -- the kernel heap re-initialised
itself in the middle of the test, a remarkably
emphatic way to say "something is broken."

The actual fix is a one-line change in userspace. Replace
`sleep_ms(500)` with a bare `yield()`:

```c
for (;;) {
    struct gui_event ev;
    while (gui_poll_event(&ev)) { /* handle */ }
    yield();   // pump-of-last-resort
}
```

`desktop` is now always on the runqueue, but on a single-CPU
cooperative scheduler that costs nothing — when something else
is runnable, the scheduler picks it; only when nothing else
is runnable does the scheduler land on `desktop`, and then
`desktop` immediately yields back. The visible effect is that
every `sys_yield` drains the tablet, so the cursor tracks the
host pointer at the scheduler's full rate even with zero apps
open.

The lesson generalises: on a cooperative kernel, **anything
that needs to feel real-time has its sample rate capped by
the busiest cooperating process.** If you want a high-frequency
heartbeat, make sure *some* always-alive process yields fast
enough to provide it. Don't reach for the IRQ until you've
audited every device driver the IRQ would touch for re-entrancy.

Two related cleanups landed in the same investigation:

- **`pump_input_into_wm` no longer early-returns** when there
  are zero windows. The keyboard helpers self-skip when no
  window has focus, so the gate was over-broad and was hiding
  the cursor stall from the windowed case as well.
- **`compose_all` lazy-seeds the cursor at screen-center** the
  first time it runs after the framebuffer is up. Under HVF,
  QEMU does not synthesise an EV_ABS until the host pointer
  physically enters the QEMU window, so a freshly-booted
  system used to sit there with `g_pointer_x = -1` and no
  cursor sprite at all. The seed is sticky; the very next
  motion event overwrites it.

## What we built

| Artifact | What it does |
|----------|--------------|
| `userspace/desktop/desktop.c` | `/bin/desktop` — opens `/mnt/wallpaper.bgra`, draws into a screen-sized PIN_TO_BOTTOM window.  Stays alive via `yield()`-poll so the cursor pump runs at full scheduler rate when no app windows are open. |
| `assets/osfs/wallpaper.bgra` | The wallpaper itself, on disk. |
| `scripts/img_to_bgra.py` | Now writes an 8-byte (width, height) header.  Self-bootstraps Pillow on first import miss (PEP-668 friendly) so a fresh `make clean && make` doesn't trip on a missing host package. |
| `kernel/core/wm.c` | New three-pass painter (PIN_TO_BOTTOM → regular → ALWAYS_ON_TOP).  `compose_all` lazy-seeds the cursor at screen-center on the first paint after fb-ready. |
| `userspace/libc/syscall.h` | `GUI_WIN_FLAG_PIN_TO_BOTTOM`, `gui_get_screen_size()`. |
| `kernel/core/syscall.c` | `SYS_GUI_GET_SCREEN_SIZE = 50`.  `pump_input_into_wm` no longer early-returns when no windows are open. |
| `userspace/{taskbar,notify,desktop}/*.c` | All three query the screen size at startup. |
| `kernel/core/thread.c` | `yield()` now masks IRQs around the cswitch critical section. |
| `Makefile` | `FB_RES` controls both QEMU scanout and on-disk wallpaper resolution. |

The desktop now boots straight to a flowers-themed 1920×1080
wallpaper with a launcher in the top-left and a taskbar
spanning the bottom — and stays up, with a smooth cursor
whether or not any apps are open.

# Chapter 53 — Boot to desktop

After milestones 40–45 the system has a window manager, a mouse, three
GUI applications, and a launcher that can spawn them.  But the user
experience is still:

```
Welcome to ...
$ launcher
[wm] window created ...
```

You boot to a serial shell, type the name of the GUI launcher, *then*
get a desktop.  Every commercial OS does this differently: PID 1 spawns
a shell **and** a desktop session, in parallel, with neither blocking
the other.  This chapter closes that gap.

## What "boot to desktop" actually requires

Three things, none of which is hard individually:

1. PID 1 (`init`) must spawn the launcher in the background.
2. PID 1's `wait()` loop must reap whatever child exits next, but
   only **terminate** when the *foreground* (shell) process exits.
3. The wallpaper must look like a wallpaper, not a placeholder.

The first two together are the entire change to `userspace/init/init.c`:

```c
puts("[init] launching /bin/launcher (background, GUI)");
int gtid = spawn("/bin/launcher", "");
if (gtid < 0) { /* non-fatal */ }

puts("[init] launching /bin/sh");
int tid = spawn("/bin/sh", "");
if (tid < 0) return 1;

int sh_code = 0;
for (;;) {
    int code = 0;
    int reaped = wait(&code);
    if (reaped < 0) break;
    if (reaped == tid) { sh_code = code; break; }
    /* background child exited — log it and keep going */
}
```

Two things are subtle here:

* **Background isn't detached.**  We don't `exec` the launcher into the
  ether; we just don't `wait()` for it specifically.  When it exits
  (because the user closes its window), `wait()` will return *some*
  child, and we keep looping until the child that exited happens to be
  the shell.  This is the simplest possible "session leader" pattern.
* **`wait()` doesn't take a target tid.**  Our SVC-based syscall ABI
  exposes only `wait(int *status)`, returning whichever child reaped
  first.  Our loop turns that into a tid-targeted wait by busy-reaping
  in user space.  In a future syscall pass we'd add a `waitpid(tid,
  &code, 0)` to do this in the kernel without the loop.

## The wallpaper

Up until this chapter the wallpaper was a single `fb_clear(WALLPAPER)`
fill — a flat dark navy.  That was fine for "is the GUI alive?" but
read as "uninitialised framebuffer" once the launcher window sat on
top.  A vertical gradient is enough to telegraph "this is a desktop":

```c
const uint32_t bands = 16;
for (uint32_t i = 0; i < bands; i++) {
    /* Linear blend WALLPAPER_TOP -> WALLPAPER as i goes 0 -> bands-1. */
    uint32_t num = i, den = bands - 1;
    uint8_t r = (WALLPAPER_TOP.r * (den - num) + WALLPAPER.r * num) / den;
    uint8_t g = (WALLPAPER_TOP.g * (den - num) + WALLPAPER.g * num) / den;
    uint8_t b = (WALLPAPER_TOP.b * (den - num) + WALLPAPER.b * num) / den;
    fb_fill_rect(0, i*band_h, fb->width, h, FB_COLOR(r, g, b));
}
```

Sixteen 50-row bands hide the discontinuities at this resolution (an
1280×800 framebuffer divided 16 ways gives 50-pixel bands; with two
adjacent bands differing by one of 16 RGB steps the boundary is
imperceptible at normal viewing distance).  The kernel doesn't pay
per-pixel cost.

We kept the dark horizontal bar at the bottom — the next chapter will fill
it with a real taskbar and a clock.

## Verification: zero-input boot

The smoke test is the strictest one yet — it sends no input at all:

```
PASS: shell prompt reached
PASS: init auto-spawned /bin/launcher
PASS: launcher window created in WM
PASS: launcher body painted (pixel at (200,90) = (232, 236, 240))
PASS: wallpaper gradient (top (24, 32, 64) > bottom (16, 20, 41))
ALL TESTS PASSED
```

The pixel asserts are deliberately specific:

* `(200, 90)` is inside the launcher's title bar margin (above all
  three buttons).  `(232, 236, 240)` is the launcher's BG colour
  `0xE8ECF0` — a light grey-blue.
* `(1000, 30)` and `(1000, 700)` are in the wallpaper, near the top
  and bottom respectively.  Sum-of-channels at the top exceeds the
  bottom by ~63 — small but clearly above noise.

Both assertions are tight enough to fail loudly if the launcher's
spawn regresses or the gradient direction inverts.

## Lessons

- A "desktop" is not a feature — it's a **process model decision**.
  PID 1 has to know that two children are different (one foreground,
  one optional) and act accordingly.
- The cheapest version of "background child" is "foreground child but
  don't wait for it specifically".  We don't need detach semantics or
  setsid yet.
- A flat colour reads as "broken" while a gradient reads as "designed".
  The two differ by 16 fb_fill_rect calls.

Next milestone: a real taskbar (process 4 in this session, fed by a
new `gui_query_windows` syscall that returns the WM's window list).

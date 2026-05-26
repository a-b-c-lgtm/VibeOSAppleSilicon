# Chapter 130c — Doom plays

> **Milestone in this chapter:** close Phase 1 of Part XVIII
> by verifying end-to-end gameplay — title → menu → episode →
> skill → E1M1 → motion → fire.
> **Code referenced:**
> - [userspace/doom/doomgeneric_osdev.c](../../../userspace/doom/doomgeneric_osdev.c)
>   (the input shim path — see note below)
> - [scripts/test_doom_plays.py](../../../scripts/test_doom_plays.py)
>
> **At the end of this chapter** you will have a verified
> manual-play procedure and a green automated acceptance test
> for the cross-built Doom binary. Prerequisites: chapter 130a
> (Doom port) and chapter 130b (WAD on disk).
>
> **Heads-up.** The 250-ms timed-release input shim used in
> this chapter is retired in chapter 133g and replaced by real
> `GUI_EVENT_KEY_UP` events plumbed through `virtio_input.c`.
> The acceptance story below is unchanged — step 8 of the
> manual procedure still walks with W and fires with F — but
> the underlying mechanism is now genuine release events, not
> a timer. Read mentions of `HOLD_RELEASE_MS` below as
> historical context.

---

## What you'll do in this chapter

1. Write `scripts/test_doom_plays.py` — boot the OS,
   `serial.sendall(b"doom\n")`, wait for `[doom] window
   created` and `V_Init:` on serial, then `screendump` via
   QMP and assert the title-screen region is at least 25%
   non-black.
2. Add the script to the regression sweep.
3. Run through the nine-step manual smoke procedure
   (launcher → terminal → doom → title → New Game → episode
   → skill → E1M1 → W to walk → F to fire → Esc) on the
   commit that closes Phase 1.

## What "Phase 1 done" actually means

Phase 1 has been one task in three takes:

- **130a** built the binary. DoomGeneric cross-compiled,
  linked against the chapter-128-series libc additions,
  loaded by the chapter-13 ELF loader, ran straight through
  `D_IdentifyVersion` and the FP/SIMD-at-EL0 paths added in
  chapter 129.
- **130b** put the data next to the binary. `mkosfs2.py`
  learned to seed an optional `doom1.wad` into the OSFS-2
  root, the Makefile picked it up through `$(wildcard)`,
  and the shim's default argv pointed at `/data/doom1.wad`.
  Title screen rendered. A postscript section of that
  chapter then fixed the in-game motion bug — chapter 30's
  virtio_input dropping key releases at the kernel layer,
  combined with DoomGeneric's per-tick `gamekeydown[]`
  reads in `G_BuildTiccmd`, was clearing every key half a
  millisecond after it was pressed. The fix was a
  `HOLD_RELEASE_MS = 250` timer in the doom shim that
  defers the synthetic UP until 250 ms after the last DOWN
  (and gets refreshed on repeat DOWNs from typematic), so
  `gamekeydown[w]` actually stays true long enough for
  `G_BuildTiccmd` to register forward motion.
- **130c** (this chapter) is the *acceptance* chapter: a
  regression test that catches future breakage to the
  render and WAD-load pipelines, plus a documented manual
  procedure for the input half. It also writes down the
  one design decision left implicit by 130a/b: where
  `/bin/doom` sits in the desktop's app surface (answer:
  it doesn't, yet — see "What 130c is **not**" below).

This chapter is short. The interesting work shipped in 130a
and 130b. What this chapter does is decide what "Doom plays"
means as a regression contract, write the test that enforces
the regression-prone half, and document the manual play
procedure for the half that doesn't automate cleanly today.

---

## What 130c is **not**

- **Not a desktop-launcher integration.** A user playing
  Doom right now types `doom` at `/bin/sh` over the serial
  console or in a `gui_term` window. There's no Doom icon
  in `/bin/launcher`'s menu and no `gui_term`-launched
  shortcut. Doing that needs `launcher`'s app list to grow
  an extra entry and `desktop`'s icon grid to learn one
  more sprite; both are mechanical (chapter 31 and chapter
  44 material) but neither belongs in Phase 1.
- **Not a Doom-from-GUI test.** A first cut of the script
  this chapter ships did launch Doom by `serial.sendall(b"gui_term\n")`,
  then `type_text(qmp, "doom\n")`. That's the user-facing
  path. It hit a diagnostic dead-end (more below); the
  test launches directly from the serial-attached
  `/bin/sh` instead. The substitution is sound — focus in
  the chapter-108d WM is by *window-creation order*, not
  by parent-process lineage, so QMP keyboard input still
  routes to Doom even when its parent is `sh` rather than
  `gui_term`.
- **Not a full input-driven regression.** The hardest
  thing the chapter-130b postscript fixed was a *gameplay*
  bug — keys pressed in the title screen worked fine, but
  motion in E1M1 didn't. The regression-shaped version of
  that test would be: drive Doom through the menus via
  QMP key events, walk forward via three `w` presses, and
  pixel-diff the framebuffer to prove the player moved.
  The script in this chapter does the boot-and-render half
  (which catches WAD load or renderer breakage); the
  input half is deferred to a future GUI-input harness
  (Part XIX — see the "Why the input half is manual"
  section for the QMP/wsd/focus-shadow root cause).
- **Not a full input-driven regression.** The hardest
  thing the chapter-130b postscript fixed was a *gameplay*
  bug — keys pressed in the title screen worked fine, but
  motion in E1M1 didn't. The regression-shaped version of
  that test would be: drive Doom through the menus via
  QMP key events, walk forward via three `w` presses, and
  pixel-diff the framebuffer to prove the player moved.
  The script in this chapter does the boot-and-render half
  (which catches WAD load or renderer breakage); the
  input half is deferred to a future GUI-input harness
  (Part XIX — see the "Why the input half is manual"
  section for the QMP/wsd/focus-shadow root cause).

---

## What 130c **is**: the regression contract

> "Doom plays" = the binary boots, finds its WAD, runs
> through V_Init, and paints a non-black title-screen
> frame inside its window. Manual smoke covers menu
> navigation and in-game motion until an automated
> GUI-input harness lands.

The regression-prone half is automated:
[`scripts/test_doom_plays.py`](../../../scripts/test_doom_plays.py).
The manual half is six commands a human types after
booting the OS GUI (the "Manual smoke procedure"
section below).

The automated test fails loudly on the kinds of breakage
the kernel can introduce by accident:

- ELF loader regression makes `/bin/doom` fail to start →
  no `[doom] window created` line on serial → FAIL.
- OSFS-2 read path regression makes the WAD unreadable →
  Doom prints `Game mode indeterminate.` and exits before
  `V_Init:` → FAIL.
- Renderer init regression (libc `<math.h>`, `qsort`,
  FP-at-EL0) makes `D_DoomMain` crash between WAD load and
  title screen → no `V_Init:` line → FAIL.
- WM regression breaks the chapter-108d damage path so the
  window stays at its initial all-black fill → title
  region samples 0% non-black → FAIL.

That's exactly the set of regressions a kernel-touching
chapter is most likely to introduce. Catching them
automatically without the input-driven half is the right
trade for "ship Phase 1 today."

---

## The test script in 80 lines (the parts that matter)

The full script is 250 lines. Most of it is the same QMP
+ Unix-socket boot harness all the GUI tests use. The
Doom-specific shape is:

```python
# scripts/test_doom_plays.py (abridged)

WAD  = os.path.join(ROOT, "assets/wads/doom1.wad")
FB_W, FB_H = 1280, 800
# Doom window is the first cascade window (launcher uses an
# explicit position).  wsd cascade base = (100,100); doom is
# 640x400 so it spans (100,100)..(740,500).  We sample a
# 240x200 region centred at (420,350) — well inside the
# body for any small position jitter from future wsd changes.
RX0, RY0 = 300, 250
RX1, RY1 = 540, 450

def main():
    if not os.path.exists(WAD):
        print("doom_plays: SKIP (no WAD at assets/wads/doom1.wad)")
        return 0

    qemu = boot()
    try:
        qmp, serial = waitsock(QMP_SOCK), waitsock(SERIAL_SOCK)
        qsend(qmp, {"execute": "qmp_capabilities"})

        # 1. wait for /bin/sh on the serial console
        log = wait_for(serial, "$ ", 90.0, b"")
        # 2. launch doom directly — keeps its stdout on the UART
        serial.sendall(b"doom\n")
        # 3. shim-side and DOOM-side sync points
        log = wait_for(serial, "[doom] window created", 30.0, log)
        log = wait_for(serial, "V_Init:",               45.0, log)
        time.sleep(3.0)                  # title screen settles

        # 4. screendump and prove the doom window painted
        screendump(qmp, "/tmp/doom_plays_title.ppm")
        w, h, px = read_ppm("/tmp/doom_plays_title.ppm")
        region = region_bytes(px, w, RX0, RY0, RX1, RY1)
        if nonblack_pct(region) < 25.0:
            fail("title region looks empty")
        print("doom_plays: PASS — title screen rendered")
    finally:
        qemu.terminate(); qemu.wait(timeout=5)
```

The `nonblack_pct < 25%` threshold is the key
assertion. On a clean run the title region samples 100%
non-black (the cyan wallpaper underneath is 42+58+73 = 173,
above the 60 threshold; Doom's title screen paints over it
with the demon-and-sky artwork, which is even more
saturated). An all-wallpaper region (if Doom never opened
its window) would also be ~100% non-black — but in that
case the test would have already failed at step 3
(`[doom] window created` never appeared on serial). If
the window opens but Doom never paints into it, the wsd
fill of `0xFF000000` (opaque black) covers the wallpaper
and the region samples ~0% non-black.

That's the chain: serial sync points say "Doom got far
enough to run"; the pixel sample says "Doom drew
something into its window". Together they're the
"Phase 1 acceptance" gate.

---

## Why launch from `/bin/sh` and not `gui_term`

The user-facing path is `gui_term` → `doom`. That's how a
human plays. A first cut of this test mirrored that path:

```python
# rejected approach
serial.sendall(b"gui_term\n")          # 1
time.sleep(2.0)                        # 2
type_text(qmp, "doom\n")               # 3 (via virtio-keyboard)
serial_log = wait_for(serial, "V_Init:", 45.0, ...)   # 4
```

That fails *silently*. Doom exits with
`[sys_exit] thread '/bin/doom' exited with code 0xffffffffffffffff`,
but *no Doom-side diagnostic ever reaches serial*. The
root cause is structural: `gui_term`'s child is its own
shell; that shell's `fork`/`exec` of `/bin/doom` makes
Doom a *grandchild* of `gui_term`; Doom's `stdout` /
`stderr` is the `gui_term` pty pipe, not the kernel UART.
Whatever Doom printed about *why* it was exiting goes
into a buffer the test can't read.

Switching to `serial.sendall(b"doom\n")` over the
serial-attached `/bin/sh` makes `/bin/doom` a direct child
of the serial shell — same UART, full visibility. Focus is
unaffected because the chapter-108d WM grants focus on
*window creation*, not on parent-process identity; Doom's
window is the third-most-recent at the moment QMP keys
start arriving (launcher → gui_term-if-present → doom),
and the launcher is invisible because the test never
spawns `gui_term`.

This is a generally-useful pattern for any future
GUI-app regression test: launch from the serial shell to
keep diagnostics visible; trust the WM's focus-by-creation
model to route input correctly anyway.

---

## Why the input half is manual

This is the embarrassing part of Phase 1. The chapter-130b
postscript fix is *exactly* the kind of subtle bug a
pixel-diff regression test should catch — and the harness
to run it exists. But the test doesn't catch it today
because of a separate, unrelated issue in how QMP-injected
keyboard events route through the focus chain.

The path a real keystroke takes:

1. PS/2 or USB keycode arrives at the kernel virtio-input
   driver (`kernel/device/virtio_input.c`).
2. Driver converts to ASCII and calls
   `wm_keyboard_byte()` in `kernel/core/wm.c`.
3. `wm_keyboard_byte` looks up `g_focus_id`, finds the
   focused window, calls `deliver_key()` which pushes
   the event into the window's per-process ring.
4. The window's wmclient `wm_poll_event` loop sees the
   key and dispatches it.

`g_focus_id` is set by two paths: explicit
`gui_raise_window()` calls from wsd (on click or on
`compose_all`'s "topmost decorated" decision), and a
short-circuit inside the kernel WM's own
`wm_pointer_down` for click-to-focus on the kernel-side
input shadow.

A QMP `{"execute":"input-send-event", ...}` for a key
goes through path 1–4 cleanly when there's already a
focused window. The problem is *initial* focus: when
Doom first opens, wsd has not yet called
`gui_raise_window` on it (compose_all does that, but
only after the next damage event with the right shape),
and the kernel WM's click-to-focus path requires a
left-click *inside the kernel-shadow rectangle* — which
in turn requires wsd to have told the kernel WM where
that rectangle is for the new window. Until that
handshake completes, QMP keys go to whichever window was
most-recently focused — usually the launcher — and Doom
sees nothing.

A real user works around this without noticing because
they move the mouse over the Doom window before pressing
a key. The mouse-over sequence (motion → click for
focus → key) drives all three paths in the right order.
QMP-injected key events without prior QMP-injected
mouse interaction skip the focus-bootstrap step.

We *did* try the QMP virtio-tablet click — `click_at(qmp,
420, 300)` aimed at the centre of Doom's window. It got
the cursor into the right place visually but didn't
satisfy the kernel-WM hit-test, presumably because wsd's
focus-shadow geometry hasn't been propagated to the
kernel WM by the time the test sends the click. Chasing
that is its own chapter of work — wsd's focus-shadow
update is asynchronous and there's no QMP-visible "focus
settled" event we can wait for. Better to defer than to
add a synthetic 2-second sleep that will be flaky on
slow CI runners.

The right fix lands in Part XIX, as a small focus-bus
extension that lets a test sync-wait on "window X has
focus". When that ships, this chapter's `manual smoke`
section can be deleted and the script can grow back the
Enter → menu → game → walk → pixel-diff loop the original
prose called for.

---

## Manual smoke procedure (Phase 1 acceptance)

After a green `python3 scripts/test_doom_plays.py`, the
human-side acceptance is:

```sh
$ make run                       # boots to desktop
# in a real QEMU SDL/Cocoa window:
# 1. click the launcher (bottom-left)
# 2. click "Terminal"
# 3. in the new gui_term, type:  doom <Enter>
# 4. title screen renders.  Press Enter.
# 5. main menu shows.  Press Enter ("New Game").
# 6. episode select shows.  Press Enter (Knee-Deep in the Dead).
# 7. skill select shows.   Press Enter (Hurt me plenty).
# 8. E1M1 loads.  Walk forward with W.  Fire with F or Ctrl.
# 9. Esc → quit.
```

All nine steps must succeed on the next-to-last commit
before merging anything Phase 1 touched. They cover the
chapter-130b timed-release fix (step 8 — sustained motion),
the chapter-129 FP-at-EL0 path (step 6 — the episode-select
fade uses the renderer's float-heavy palette interpolation),
and the chapter-108d input plumbing end-to-end (steps 4–7).

When the GUI-input harness lands in Part XIX, this section
becomes a one-line "see `scripts/test_doom_plays.py`" and
this whole document gets a "fully automated" stamp.

---

## What this unlocks

In the strict
[apps-must-use-features](../../../memories/apps-must-use-features)
sense, chapter 130c unlocks:

- **`/bin/doom`** is now the canonical exerciser of the
  chapter-130b timed-release input shim. Every menu Enter,
  every step forward, every shot fired is a unit test for
  that fix. (Existing chapter `_dbg_doom_input_timing.py`
  was the dev-time scope; chapter 130c's manual procedure
  is the user-time scope.)
- **`scripts/test_doom_plays.py`** added to the sweep —
  catches WAD-load, render-init, and damage-path
  regressions automatically. Reads
  `nonblack_pct(title_region)` as its single hard signal.
- **The "Phase 1 done" marker** in the chapter 128 plan
  flips. Phase 1 set out to prove the platform was real
  enough to run real software unmodified —
  `vendor/doomgeneric` is unmodified upstream code, the
  shim adapts only what the porting interface required, and
  the result plays. Phase 2 (real GCC builds it) follows on
  this base.

No new apps. No new kernel features. Just the regression
contract and the closing-out documentation for an arc
that started six chapters back.

No new apps. No new kernel features. Just the regression
contract and the closing-out documentation for an arc
that started six chapters back.

---

## Looking ahead to Phase 2

Phase 2 begins with [131a — binutils target
setup](131a-binutils-target.md): teach binutils about an
`aarch64-osdev` target triple, cross-build
`aarch64-osdev-as` and `aarch64-osdev-ld` on the host,
and verify those host-built tools produce ELF output our
kernel will load.

Phase 1's discipline carries forward: every libc gap real
binutils hits gets the same treatment Doom's gaps got —
identify the call, add the minimum implementation to
libc, re-run, repeat. The chapter-128-series additions
were tuned for Doom; binutils exercises different corners
(a lot more `<unistd.h>`, real `dirent.h` walks,
`mkstemp`-style atomic file creation). Each addition gets
its own short chapter under sub-part B.

The end state for Phase 2 is a `/bin/aarch64-osdev-gcc`
that — given the cross-built binutils — produces a new
`/bin/doom` from source on the guest itself. When that
rebuilt Doom plays through the manual procedure above,
Part XVIII is done.

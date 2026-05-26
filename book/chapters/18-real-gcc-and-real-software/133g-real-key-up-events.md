# Chapter 133g — Real key-up events

> **Milestone in this chapter:** retire the 250 ms timed-release
> input shim from chapter 130b and replace it with real key-up
> events from `virtio_input`.
> **Code referenced:**
> - [kernel/device/virtio_input.c](../../../kernel/device/virtio_input.c)
>   (parallel 32-slot release ring)
> - [userspace/doom/doomgeneric_osdev.c](../../../userspace/doom/doomgeneric_osdev.c)
>   (delete `HOLD_RELEASE_MS`)
> - [scripts/test_doom_plays.py](../../../scripts/test_doom_plays.py)
>
> **At the end of this chapter** you will have Doom's movement
> keys stop the same tick the user releases them rather than
> coasting for a quarter second, real `GUI_EVENT_KEY_UP`
> events plumbed through `virtio_input.c`, and the
> chapter-130b timer shim deleted. Prerequisites: chapter 30
> (virtio-input keyboard), chapter 108d (kernel WM focus
> model), chapter 130b (the stopgap whose retirement this
> chapter is about).

---

## What you'll do in this chapter

1. Extend `kernel/device/virtio_input.c` with a parallel
   32-slot release ring sitting next to the existing
   ASCII byte ring. On every `KEY_VAL_RELEASE` for a
   non-modifier key, translate the evdev code to the same
   GUI key code (ASCII byte 0..255, or one of
   `GUI_KEY_UP`/`DOWN`/`LEFT`/`RIGHT`/`HOME`/`END`/`PGUP`/
   `PGDN`) the matching press would have produced, and
   push it. Modifier keys (`KC_LSHIFT`, `KC_LCTRL`, …)
   stay on their existing modifier-state path; their
   release flips the shift / ctrl tracker bit and never
   becomes a key event.
2. Add `virtio_input_try_get_release(uint32_t *out)` to
   `kernel/device/virtio_input.h`, analogous to
   `virtio_input_try_getc`. One uint32_t per call; 1 on
   success, 0 on empty.
3. Add `#define GUI_EVENT_KEY_UP 7` to `kernel/core/wm.h`
   and mirror it in `userspace/libc/syscall.h`. The
   `arg0` field carries the same GUI key code the matching
   `GUI_EVENT_KEY` delivered. Modifier keys never appear
   in either type — their effect is folded into the byte
   carried by the press.
4. In `kernel/core/wm.c`, add a per-key held-state table
   `static uint8_t g_keys_held[0x110]` indexed by GUI key
   code (ASCII range + the `GUI_KEY_*` extended range).
   Set the bit in `deliver_key` (so every press flips it
   to 1, including auto-repeats which are idempotent),
   clear it in the new `wm_keyboard_release`. The held-
   state table is the single source of truth for "what
   keys does the currently-focused window think it owns".
5. Add `int wm_keyboard_release(uint32_t key)` that pushes
   a `GUI_EVENT_KEY_UP` event into the focused window's
   event ring and clears `g_keys_held[key]`. Spurious
   releases (no matching press visible to this focused
   window) drop silently.
6. Route every `g_focus_id = X` mutation in `wm.c`
   through a new `static void set_focus(int32_t new_id)`
   helper. The helper iterates `g_keys_held`, pushes a
   `GUI_EVENT_KEY_UP` for every still-held key into the
   previously-focused window's ring, clears the table,
   then assigns `g_focus_id = new_id`. This makes
   alt-tabbing out of a game during sustained motion
   produce a clean release to the outgoing window;
   without it, `gamekeydown[]` in the now-defocused Doom
   would stay true forever and the player would
   sleepwalk into a wall.
7. Drain the release ring in
   `kernel/core/syscall.c::pump_input_into_wm` after the
   existing byte-stream drain and the
   `wm_flush_pending_keys` call. Same idempotent shape
   as the press drain — pump on every yield, no IRQ
   wiring needed.
8. Delete the timed-release machinery from
   `userspace/doom/doomgeneric_osdev.c`: `g_press_at_ms`,
   `HOLD_RELEASE_MS`, `emit_stale_releases`. Add a
   `GUI_EVENT_KEY_UP` case to `pump_events`. Keep `g_held`
   purely to dedup virtio-keyboard's `KEY_VAL_REPEAT`
   events (which surface as additional `GUI_EVENT_KEY`
   presses for an already-held key; the shim swallows
   them so DoomGeneric sees one press, one release).

---

## Why now

Two reasons. The bookkeeping reason: chapter 130b shipped
a 250-ms stopgap that the chapter itself called a stopgap.
The technical reason: the stopgap is incorrect for the
FPS use case the chapter was unlocking.

Re-read the chapter-130b postscript and the asymmetry
jumps out. `gamekeydown[k]` was being cleared *too
quickly* by the original chapter-130a shim (synthetic
release back-to-back with press, before `G_BuildTiccmd`
could see the held state); the 130b fix was to clear it
*too slowly* by 250 ms. The lower-bound argument in 130b
(motion needs ≥ 3-4 ticks of `gamekeydown[k] == true`,
~100 ms) is real and unchanged here; the upper-bound
argument (250 ms is the smallest value larger than QEMU's
typematic initial delay) is an artefact of working around
the missing release events. Once releases reach userspace
the upper bound disappears.

In play, the 250-ms latency feels like ice physics. A
tap of W moves the player forward by about three tiles
when only one was intended. Strafing diagonally and then
releasing one key means the player continues drifting in
the released direction for two ticks. A quarter-second
isn't long for paperwork, but DoomGeneric's tick is
28.6 ms — 250 ms is nine ticks of unintended motion per
key lift. That's what playing Doom over the chapter-130b
shim feels like.

The fix is mechanical — drop the early-return in
`handle_event`, derive the GUI key code on release the
same way press does on press, plumb the event through.
The interesting part is the focus-change synthesis
(step 6 above), which is the only place the change
adds new behaviour rather than restoring information
that was already on the wire.

## Run it / Test it

The existing render-pipeline regression
`scripts/test_doom_plays.py` is the gate. It still passes
on the post-change build:

```
doom_plays: shell prompt visible
doom_plays: sent 'doom' on serial
doom_plays: V_Init reached; title screen rendering
doom_plays: title region non-black = 100.0%
doom_plays: PASS — title screen rendered
    (saved /tmp/doom_plays_title.ppm)
```

A passing render test is necessary but not sufficient
— it doesn't exercise the input path. The input half is
checked manually with the chapter-130c nine-step
procedure (launcher → terminal → doom → title → New
Game → episode → skill → E1M1 → walk → fire). After this
chapter, step 8 (walk forward, release, observe motion
stops) is the unit test for the new event type. The
acceptance feel is:

- Tap W. Player walks forward exactly while the key is
  held. Lift the key. Player stops on the same tick
  (within one frame of the screen update).
- Strafe diagonally with W + D. Release W only. Player
  immediately stops the forward component and continues
  pure strafe. Release D. Player stops dead.
- Hold F. Pistol fires at its native cadence. Release
  F. Firing stops on the next tick.

All three were the failure modes the timed-release shim
had no good answer for; all three work cleanly once
`GUI_EVENT_KEY_UP` is reaching DoomGeneric's per-tick
`gamekeydown[]` reads in `G_BuildTiccmd`.

A focus-change unit test that isn't part of the automated
suite is worth running by hand once:

1. Launch doom from the launcher.
2. Hold W (player walks forward).
3. While W is still held, click somewhere outside the
   Doom window — say, the launcher's clock, or a
   gui_term window. Keyboard focus moves away from Doom.
4. Release W. Without focus-change synthesis Doom would
   not see the release (the release goes to the new
   focus, not to Doom) and the player would keep
   walking until they hit a wall. With the synthesis
   step in `set_focus`, the old Doom focus saw a
   `GUI_EVENT_KEY_UP` for W at the moment focus moved,
   `gamekeydown[W] == false` from that moment, the
   player stopped on the next tick.

This last test isn't automated because it requires
mouse-driven focus changes that the
`type_key`-style QMP injection (chapter 130c) doesn't
model. It's a smoke procedure; if anyone ever notices
"the Doom player walks into walls when I switch
windows", the bug is in this synthesis path.

## Pitfalls

### Pitfall — modifier release through the release ring

**Symptom:** during early bring-up of this chapter the
release ring was being filled by every release including
shift / ctrl. The shim then saw spurious
`GUI_EVENT_KEY_UP` events for keys it had never seen a
press for, and (since `g_held[]` was 0 for them) silently
dropped them. No user-visible bug, but the path was
pushing entries onto the release ring that no consumer
would ever do anything with — wasteful, and it hid a
real issue: that releasing the *shift* key during a held
`W` was about to surface as a phantom up-arrow.

**Cause:** the early-return for modifier releases lived
in the *byte-stream* path
(`if (value == KEY_VAL_RELEASE) return;`); when the
release ring was added next to it, the new code ran
**before** the modifier guard, which had been the
single early-return for the whole function.

**Fix:** guard non-modifier-only at the release-ring
push site:

```c
if (value == KEY_VAL_RELEASE) {
    if (code == KC_LSHIFT || code == KC_RSHIFT ||
        code == KC_LCTRL  || code == KC_RCTRL  ||
        code == KC_LALT   || code == KC_RALT   ||
        code == KC_CAPSLOCK)
        return;
    uint32_t k = code_to_gui_release(code);
    if (k) rel_ring_push(k);
    return;
}
```

The modifier-state trackers (`g_shift_down`,
`g_ctrl_down`) already update *above* this block on the
unconditional `down = (value == KEY_VAL_PRESS || …)`
line, so this guard suppresses only the spurious-event
half — modifier state still tracks correctly.

### Pitfall — focus-change leaves keys "held" on the new focus

**Symptom:** the first cut of `set_focus` just assigned
`g_focus_id = new_id` and didn't synthesise releases.
Manual test: launch doom, hold W, alt-tab out by
clicking on another window, then release W. The new
focus (gui_term, say) received the release event because
the release ring drain pumps releases into whatever the
*current* focused window is at drain time, not at press
time. So the gui_term saw `GUI_EVENT_KEY_UP` for a `w` it
had never seen a press for (silently dropped by its
shim) — and Doom's `gamekeydown[W]` stayed true forever,
because Doom never saw the release. The Doom player
walked into the south wall and kept pushing.

**Cause:** keyboard focus is a kernel-side property that
shifts the moment the user clicks a window, but the
held-key state belonged to the *old* focused window,
not to the input device. Without explicit handoff,
releases reach the wrong window.

**Fix:** synthesise releases to the *outgoing* focus at
focus-change time. The implementation in
`synthesize_releases_to` walks `g_keys_held[]`, pushes a
`GUI_EVENT_KEY_UP` for every set bit into the outgoing
window's event ring, and clears the table — so the new
focus starts from a clean held-state. Doom sees the
release of `W` *at the moment the user clicked away*,
not when they eventually let go.

This makes focus-change semantically equivalent to "the
user lifted every key they were holding, *then* clicked
the new window." That matches user intuition (no game
they've played continues moving after the window
loses focus) and prevents the gamekeydown-stuck-on-true
trap.

## What's deferred

- **Per-window keyboard routing remains kernel-only.**
  wsd handles mouse via shadow windows and
  `SYS_GUI_DELIVER_EVENT`; keyboard still flows
  exclusively through the kernel WM's `g_focus_id` and
  `wm_keyboard_byte` / `wm_keyboard_release`. Wsd never
  sees a `GUI_EVENT_KEY_UP`. If a future chapter moves
  keyboard routing to userspace too (so e.g. wsd could
  consume a global key chord to switch windows without
  kernel involvement), the release path needs to mirror
  the press path's eventual wsd-injection wire.
- **Shifted-vs-unshifted asymmetry on release.** The
  release ring emits the *unshifted* identity of the
  physical key, by design — Doom's `gui_to_doom` upper-
  cases anyway, and using the unshifted ASCII matches
  what `gamekeydown[]` indexing by raw key wants. But
  this means an app that pressed `'A'` (Shift-A) and
  then released without re-pressing shift sees
  `GUI_EVENT_KEY{arg0='A'}` followed by
  `GUI_EVENT_KEY_UP{arg0='a'}`. Apps that pair-key in a
  case-sensitive table would need to track shift state
  themselves. Not a problem for any current consumer.
- **`KEY_VAL_REPEAT` is invisible to apps as a separate
  event.** Virtio-keyboard emits PRESS, REPEAT, REPEAT,
  …, RELEASE. The kernel delivers every PRESS and
  REPEAT as `GUI_EVENT_KEY` (so notepad gets auto-repeat
  in textboxes, matching every other modern OS). DoomGeneric
  dedups the repeats via the `g_held[]` table in its
  shim. A future chapter that wants to distinguish "first
  press" from "auto-repeat" at the userspace surface
  would need a third event type or an event flag; not
  worth the complexity until something actually needs it.
- **No release-ring overflow signal.** A 32-slot ring is
  ample for human-driven keyboards (QEMU never queues
  more than two or three releases between yields), but
  if a future input source bursts (a macro recorder, a
  test harness driving the device directly), full-ring
  drops would be silent. Adding overflow-counter
  diagnostics is a one-line change in
  `rel_ring_push`; deferred until a real consumer
  needs it.

## What this unlocks

Per the standing apps-must-use-features rule:

- **`/bin/doom`** is the canonical consumer of
  `GUI_EVENT_KEY_UP`. The chapter-130b timed-release
  machinery is gone; release events drive the
  `gamekeydown[]` clear directly. Movement feels
  responsive instead of icy.
- **`scripts/test_doom_plays.py`** is unchanged — still
  the render-side regression. It happens to be the
  cheapest way to confirm this chapter didn't break
  what already worked.
- **Future game ports get a real release event.** Any
  port of a tick-polled engine (Heretic, Hexen, a 2D
  platformer, anything SDL-shaped) consumes
  `GUI_EVENT_KEY_UP` directly. The shim becomes a
  single-purpose adapter — translate the GUI key code
  to the engine's internal keysym — instead of having to
  reinvent a timed-release fiction.
- **Per-window held-key state is now plumbable.** The
  kernel WM knows what keys each focused window thinks
  it owns; an app can implement its own multi-key
  chord state without inventing a heuristic for "what
  if the user alt-tabbed mid-chord".

No new app added; this chapter's discipline question is
"does an existing app use the new feature", and the
answer is `/bin/doom` (chapter 130a / 130c). The shim
shrunk by about 25 lines, and the kernel grew by about
80 lines split across virtio_input.c, wm.c, syscall.c,
and the two header mirrors.

## What's next

Section 18 closes here for real this time — chapter
133f was the closing of the "rebuild Doom in-guest"
arc, and this chapter is the closing of the deferred
input issue that chapter 130c flagged as "not 130c's
scope, follows in a later input-system extension."
Section 19 opens on whatever the next roadmap item is.

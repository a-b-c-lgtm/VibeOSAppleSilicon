# Chapter 130b — Staging the WAD (so Doom actually plays)

> **Milestone in this chapter:** put a WAD on disk where Doom
> can find it, without committing the file to the repo.
> **Code referenced:**
> - [Makefile](../../../Makefile) (the `$(DATA_DISK)` rule and
>   its `$(wildcard ...)` prerequisite)
> - [scripts/mkosfs2.py](../../../scripts/mkosfs2.py)
> - [userspace/doom/doomgeneric_osdev.c](../../../userspace/doom/doomgeneric_osdev.c)
>   (default WAD path moved to `/data/doom1.wad`)
> - [scripts/test_doom.py](../../../scripts/test_doom.py)
>
> **At the end of this chapter** you will have Doom that
> *runs* instead of Doom that exits with "Game mode
> indeterminate": `assets/wads/*.wad` is gitignored, the
> Makefile silently formats an empty data image when no WAD
> is present, and the test script picks its acceptance mode
> by checking the host for the file. Prerequisites: chapter
> 130a (Doom port), chapter 81 (OSFS-2 layout).

---

## What you'll do in this chapter

1. Document the licensing trade-off and add `assets/wads/*.wad`
   to `.gitignore` so a reader's optional copy never gets
   committed.
2. Extend the Makefile's `$(DATA_DISK)` rule with a
   `$(wildcard ...)` prerequisite so `mkosfs2.py` seeds
   `doom1.wad` into the data image when it's present and
   silently formats an empty image when it isn't.
3. Move the WAD path the shim defaults to from
   `/data/wads/doom1.wad` to `/data/doom1.wad` (OSFS-2 is flat
   today; subdirectories land in chapter 85).
4. Update `scripts/test_doom.py` so it picks its acceptance
   mode by checking whether the WAD is on the host.
5. Run the test in both modes (WAD present / absent) and the
   full regression sweep.

## Why now — what 130a left on the table

130a got `doom.elf` built, linked, loaded, and executing all
the way through `D_IdentifyVersion`. That was the right
stopping point for that chapter: every libc gap was filled
and FP-at-EL0 was proven on someone else's code. But the
on-disk state of the guest meant `D_IdentifyVersion` never
found an IWAD, so Doom's third action after `main` was
`I_Error("Game mode indeterminate.  No IWAD file was found.
...")` and `exit(1)`.

A reader at this point reasonably types "`doom`" at the
shell, watches the window flash black for half a second, sees
an error on serial, and asks: where's the game? This chapter
answers that.

Three small edits get from "Doom runs and exits" to "Doom
runs and plays":

1. Put a WAD on the host at `assets/wads/doom1.wad`.
2. Teach the Makefile to seed it into the OSFS-2 data image
   when it's present.
3. Point the Doom shim's default `-iwad` argument at the
   on-disk path.

Plus one piece of test work and one `.gitignore` line.

---

## The licensing wrinkle (and why the WAD isn't shipped)

The Doom shareware WAD (`doom1.wad`, first nine maps of
*Knee-Deep in the Dead*) was distributed freely by id
Software in 1993 and is still legally downloadable from many
mirrors. The full retail WADs (`doom.wad`, `doom2.wad`,
`tnt.wad`, `plutonia.wad`) are not redistributable at all —
they're commercial product you have to buy.

Both are treated the same way here: **the repo never contains
a WAD**. The reader supplies their own. The `.gitignore`
forbids it from accidentally being committed:

```gitignore
# Chapter 130b: optional Doom IWAD asset, drop your own copy at
# assets/wads/doom1.wad (any Doom-engine WAD works: doom1.wad,
# doom2.wad, tnt.wad, plutonia.wad, heretic.wad, hexen.wad).
# Not committed because (a) doom1.wad is freely *playable* but its
# redistribution licence is jurisdiction-dependent, (b) commercial
# WADs cannot be redistributed at all.
assets/wads/*.wad
assets/wads/*.WAD
```

The reader workflow becomes:

```sh
mkdir -p assets/wads
curl -o assets/wads/doom1.wad <some-mirror>/doom1.wad
make
```

If they skip step two, everything still builds — the WAD is
*optional*. Doom will boot, fail to find a WAD, and exit
the same way it did at the end of chapter 130a. The
`scripts/test_doom.py` regression script has two acceptance
modes for exactly this reason (more below).

---

## OSFS-2 is a flat filesystem

This was the surprise. The natural place for a WAD is
`/data/wads/doom1.wad` — that's where the chapter 130a
shim's hard-coded default argv pointed. But OSFS-2, as
introduced in chapter 81, has exactly one directory: the
root. The on-disk inode layout supports `type=DIR`, and the
root inode does live in `inode[1]`, but **the format tool
`scripts/mkosfs2.py` only writes one directory inode and
only one block of dirents**:

```python
# scripts/mkosfs2.py (chapter 81-vintage)
def format_image(out: Path, files):
    if len(files) > DIRENTS_PER_BLOCK:
        sys.exit(f"too many seed files (max {DIRENTS_PER_BLOCK})")
    ...
```

There's no `mkdir`-style verb in the formatter and no way to
spell a path with a `/` in it. Adding a `wads/` subdirectory
would mean writing real directory creation into the
formatter, which would mean writing the full directory-entry
manipulation that the kernel will get in chapter 85. That's
not in scope here. The chapter-130 series is about getting
real software to run, not about extending the filesystem.

Two choices, then:

- Extend `mkosfs2.py` with `mkdir` support (a chapter of
  work). Defer to chapter 85, where the kernel learns
  subdirs too.
- Put the WAD at the OSFS-2 root: `/data/doom1.wad`. One
  line of formatter args, one line of shim default-path
  change.

Go with option two. The path looks slightly less tidy than
`/data/wads/doom1.wad`, but the alternative is a flag day
across the formatter and the kernel for cosmetic reasons.
When chapter 85 lands and the kernel can walk subdirs, the
formatter can grow `mkdir` and this chapter's
`/data/doom1.wad` migrates to `/data/wads/doom1.wad`
trivially.

### Does the WAD fit?

OSFS-2 file size limit is bounded by the inode layout:

```text
direct[16]            = 16 ×  4 KiB =    65 536 bytes
single-indirect[1024] = 1024 × 4 KiB = 4 194 304 bytes
                                   ------------------
                       max file size = 4 259 840 bytes
                                       (≈ 4.06 MiB)
```

`doom1.wad` is 4 196 020 bytes. After exhausting the 16
direct pointers we still need
`(4196020 - 65536) / 4096 = 1008.4` → 1009 indirect blocks,
well under the 1024-entry ceiling. It fits with about 64 KiB
to spare. Doom 2's full WAD is ~14 MiB and would *not* fit
on OSFS-2 without double-indirect; chapter 81's bound is real
and visible here for the first time.

---

## The Makefile wiring

The DATA_DISK rule in chapter 81 was deliberately
parameterless: it just formatted an empty image.

```makefile
# chapter 81 — pre-130b
$(DATA_DISK): scripts/mkosfs2.py
	@mkdir -p $(BUILD)
	python3 scripts/mkosfs2.py $(DATA_DISK)
```

The 130b replacement uses a `wildcard`-guarded prerequisite
so the build still works when the WAD isn't present:

```makefile
# chapter 130b
DOOM_WAD := assets/wads/doom1.wad
DOOM_WAD_SEED := $(wildcard $(DOOM_WAD))

$(DATA_DISK): scripts/mkosfs2.py $(DOOM_WAD_SEED)
	@mkdir -p $(BUILD)
	python3 scripts/mkosfs2.py $(DATA_DISK) \
	    $(if $(DOOM_WAD_SEED),doom1.wad=$(DOOM_WAD_SEED))
```

Three things worth flagging:

- **`$(wildcard ...)` instead of literal path** — if you
  listed `assets/wads/doom1.wad` directly as a prerequisite,
  Make would complain "No rule to make target
  `assets/wads/doom1.wad`" for readers who haven't supplied
  the WAD. Wrapping it in `$(wildcard)` makes the
  prerequisite *evaluate to the empty string* when the file
  is absent, and Make accepts an empty prereq list.
- **`$(if $(DOOM_WAD_SEED),...)`** for the recipe argument,
  same reason: when the WAD isn't present, `mkosfs2.py` gets
  invoked with zero seed arguments and produces an empty
  formatted image.
- **The Makefile rule re-runs only when the WAD's mtime
  changes**, because `$(DOOM_WAD_SEED)` *is* a real
  prerequisite when the WAD exists. Drop in a different WAD
  → `make` rebuilds `data.img` automatically.

This pattern is reusable for any optional asset. Future
chapters use the same shape for fonts beyond the default set,
optional CA-cert bundles, and so on.

---

## The shim default-path change

Chapter 130a's `userspace/doom/doomgeneric_osdev.c::main`
synthesised a default argv pointing at the (then-aspirational)
subdirectory path:

```c
/* chapter 130a */
static const char *defaults[] = {
    "doom", "-iwad", "/data/wads/doom1.wad", "-mb", "6"
};
```

After 130b that becomes:

```c
/* chapter 130b — WAD lives at /data/doom1.wad (root of the
 * OSFS-2 mount).  mkosfs2.py is flat (no subdirs); the
 * earlier-planned `/data/wads/doom1.wad` would have needed
 * subdirectory support we don't have. */
static const char *defaults[] = {
    "doom", "-iwad", "/data/doom1.wad", "-mb", "6"
};
```

This is the only line of C that changes in this chapter.
Everything else (argv-synthesis, `M_CheckParm` walking,
window creation in `DG_Init`, the BGRA blit in `DG_DrawFrame`)
is untouched from 130a. A user can still override on the
command line:

```sh
/$ doom -iwad /data/doom1.wad -mb 6 -warp 2
```

The shim just supplies the same defaults the desktop
launcher would have used if there'd been a desktop-launcher
entry for Doom (there isn't yet — chapter 130c material).

---

## Testing the difference

`scripts/test_doom.py` from 130a treated *any* DOOM banner OR
the "Game mode indeterminate" message as PASS. After 130b
that's too loose: with the WAD on disk, "indeterminate"
would mean the seed didn't take, and that's worth knowing.

The test now picks its acceptance mode by checking whether
the WAD is present on the *host* (not the guest):

```python
wad_present = os.path.exists(
    os.path.join(ROOT, "assets/wads/doom1.wad"))
```

If the WAD is present, the test requires at least one of
these serial-log markers, all of which appear *after*
`D_IdentifyVersion` has matched a known IWAD signature:

- `DOOM Shareware`, `DOOM Registered`, `DOOM 2` — banners
  from `D_IdentifyVersion`
- `V_Init`, `R_Init` — phase markers from `I_Init` /
  `D_DoomMain` after WAD parsing is done

If the WAD is absent, the old "indeterminate" path is still
accepted. Either mode treats kernel panic, EL0 sync abort,
or unresolved-symbol trap as a hard FAIL.

The full captured serial transcript lands at
`/tmp/test_doom_serial.log` for post-mortem.

A successful run with the WAD present looks like this
(grepping the serial log):

```text
[doom] window created 320x200 + 480x140
DoomGeneric v0.1
Z_Init: Init zone memory allocation daemon.
heap size: 0x927c00 = 9600000 bytes
using . for save and config
V_Init: allocate screens.
M_LoadDefaults: Load system defaults.
```

That `Z_Init` line is the smoking gun — it's printed by
`Z_Init` only after `M_FindResponseFile` and
`D_IdentifyVersion` both succeeded. Before 130b you'd never
see it; the `I_Error` from `D_IdentifyVersion` killed the
process two function calls earlier.

---

## What gets exercised

This chapter is mostly plumbing, but it exercises one
genuinely new code path end-to-end. Here is what runs
once the WAD is in place:

- **`W_AddFile` / `W_InitMultipleFiles`** — DOOM's WAD
  parser. Reads the 12-byte IWAD header, walks the
  `1264`-entry lump directory, builds the in-memory lump
  cache. This is the first time real on-disk OSFS-2 reads
  cross the 4 KiB direct/indirect boundary in a chapter
  test: the indirect block is hit on every read past
  byte 65 536.
- **`malloc(6 MiB)` for the zone allocator** — exercises the
  user heap from milestone 17 plus `sbrk()` growth. The
  zone is `-mb 6` (megabytes) in the shim default; chapter
  130a settled on this size, and the upstream default of 16
  MiB would have stressed but not broken the heap.
- **`fopen` / `fread` on `/data/doom1.wad`** — first
  4 MiB-class read from OSFS-2 in any test. The block cache
  from chapter 82 caches the indirect block but each
  4 KiB data block lookup still goes through OSFS-2's
  bmap path. Worth knowing when chapter 89's mmap lands:
  this is the path that will *immediately* benefit from
  zero-copy file reads.
- **The window from `DG_Init`** — paints once with `wm_present`
  before Doom's first render frame. The wmclient negotiated
  in chapter 108d is in the boot path of every doom run,
  so any wm regression shows up here too.

---

## Applied to / What gets exercised in tests

- **Existing app modified:** `userspace/doom/doomgeneric_osdev.c`
  — one-line default-argv path change.
- **New app:** none. The doom binary is the same one
  130a built.
- **Existing test upgraded:** `scripts/test_doom.py` gained
  WAD-aware acceptance and now dumps the full serial log
  to `/tmp/test_doom_serial.log` for inspection.
- **New test:** none. The WAD-present / WAD-absent split
  inside `test_doom.py` covers both deployment scenarios
  with a single script.

---

## What this unlocks

Doom that plays. From the shell:

```text
/$ doom
[doom] window created 320x200 + 480x140
DoomGeneric v0.1
Z_Init: Init zone memory allocation daemon.
...
V_Init: allocate screens.
```

…followed by the actual game window, accepting input from
the keyboard (via the chapter-130a `DG_GetKey` press+release
synthesis) and rendering frames at whatever rate the
software renderer + BGRA blit can sustain.

The next pieces of section 18 build on this foundation
without touching the Doom port: chapter 130c (desktop
launcher entry so Doom can be started from the start menu),
chapter 131 (audio — DG's `I_StartupSound` is currently
stubbed, and chapter 96's virtio-snd is wired and waiting).

---

## Postscript — the in-game movement bug

> **Update (chapter 133g):** the timed-release shim described
> below was retired in chapter 133g once real `GUI_EVENT_KEY_UP`
> events were plumbed through `virtio_input.c` → `wm.c` →
> userspace. The shim is preserved here as the historical fix
> that closed chapter 130b's manual-play acceptance gate;
> after 133g the doom shim consumes release events directly
> and `HOLD_RELEASE_MS` no longer exists in the source tree.
> Read this section for the diagnostic story (it remains the
> canonical example of how press-only input breaks per-tick
> game loops); read chapter 133g for the architecturally
> correct fix.

The first time the chapter-130b build is run interactively,
something embarrassing happens. The title screen renders, the
menu accepts arrow keys and Enter, "New Game" starts the
first level… and the player won't move. Arrow keys do
nothing. F (fire) does nothing. Space (use) does nothing.
Esc still opens the menu though, and the menu still works.

The bisection is fast once you know to look for it:

- **Menus work; gameplay doesn't.** That's a tell. DOOM has
  two completely different input pipelines for those two
  cases.
- `M_Responder` (menu) handles `ev_keydown` *synchronously*
  the moment the event reaches `D_PostEvent`. A matching
  `ev_keyup` arriving microseconds later has nothing left
  to undo.
- `G_Responder` (gameplay) handles `ev_keydown` by setting
  `gamekeydown[k] = true` and `ev_keyup` by setting it to
  `false`. The persistent array is then *read once per tick*
  by `G_BuildTiccmd` to assemble the player's movement
  command.

The chapter-130a shim emitted a synthetic release back-to-back
with every press, because chapter 30's input layer drops key
releases at the virtio_input layer. For menus this is harmless
(the synchronous press handler already did its work). For
gameplay it's catastrophic: the press AND release both land in
the same `pump_events` drain, `gamekeydown[k]` flips to true
then back to false, and when the 35 Hz tick comes around to
read it, the answer is "no key held → no motion."

### Fix: timed release in the shim

The kernel input path can't tell when the user *really* let
go of the key, so the shim invents its own definition: "a key
is held until the shim hasn't seen a fresh press for it in
`HOLD_RELEASE_MS` (250 ms)." Implementation in
`userspace/doom/doomgeneric_osdev.c`:

```c
static uint32_t g_press_at_ms[256];
static uint8_t  g_pressed[256];
#define HOLD_RELEASE_MS 250

static void on_doom_key(unsigned char k)
{
    if (!k) return;
    if (!g_pressed[k]) {
        kq_push(1, k);      /* first press: emit one DOWN */
        g_pressed[k] = 1;
    }
    /* Every press (first or repeat) refreshes the timer. */
    g_press_at_ms[k] = (uint32_t)uptime_ms();
}

static void emit_stale_releases(void)
{
    uint32_t now = (uint32_t)uptime_ms();
    for (int k = 1; k < 256; k++) {
        if (g_pressed[k] &&
            (now - g_press_at_ms[k]) >= HOLD_RELEASE_MS) {
            kq_push(0, (unsigned char)k);
            g_pressed[k] = 0;
        }
    }
}
```

`emit_stale_releases()` is called at the end of `pump_events()`
*and* at the top of `DG_GetKey()`, so even if no new GUI input
arrives the shim still pushes the deferred release when its
time comes.

### Why 250 ms

Two constraints fix the number from below and above:

- **Lower bound:** DOOM's tick is 28.6 ms. A single press has
  to keep `gamekeydown[k] = true` across at least one tick;
  realistically 3-4 ticks so motion is visible. → ≥ 100 ms.
- **Upper bound:** Host typematic auto-repeat. QEMU's virtio
  keyboard repeats held keys roughly every 30 ms after an
  initial 200-250 ms delay. If the shim's timer is shorter
  than that initial delay, it will spuriously release between
  the first press and the first repeat, and the player will
  see a tiny pause at the start of every held-key motion.
  → ≥ 250 ms.

250 ms is the smallest value that satisfies both. Going
much higher makes single taps feel "sticky" — a quick tap
of "fire" gives you about a third of a second of held-fire,
which is enough to chain pistol shots when you wanted one.
That's a fair price to pay for movement that actually works.

### Why the original shim shipped broken

This bug is invisible to `test_doom.py` because the harness
never gets past the title screen — `V_Init` is enough to PASS.
Adding "verify the player moved" to the automated test would
need framebuffer pixel diffing across frames, which is a
chapter-sized project of its own (a future "GUI integration
test harness"). For now the test verifies that DOOM *loads*;
the postscript fix verifies that DOOM *plays*; those two
properties get checked at different layers.

See `scripts/_dbg_doom_input_timing.py` for a diagnostic
helper that drives DOOM through menu → game with
realistic inter-keystroke spacing. The script's docstring
keeps the chapter-130b origin story; once chapter 133g
landed, the script became a general "does Doom respond to
keystrokes" diagnostic and the timed-release angle of its
explanation is historical.

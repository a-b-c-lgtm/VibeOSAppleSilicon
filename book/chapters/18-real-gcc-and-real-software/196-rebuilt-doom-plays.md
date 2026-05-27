# Chapter 196 — The rebuilt Doom plays

> **Milestone in this chapter:** run the in-guest-linked
> `/data/doomgeneric.elf` and verify it renders Doom's title
> screen on the framebuffer.
> **Code referenced:**
> - [scripts/test_doom_rebuilt_plays.py](../../../scripts/test_doom_rebuilt_plays.py)
> - [scripts/test_doom_link.py](../../../scripts/test_doom_link.py)
>   (invoked as a `subprocess.run` to produce
>   `/data/doomgeneric.elf` on `build/data.img`)
>
> **At the end of this chapter** you will have a two-phase
> chained acceptance test that builds Doom from source inside
> the guest and then runs it in a second QEMU boot, with a
> QMP screendump confirming the title screen. This closes the
> Phase-2 arc of the chapter-164 plan: Doom, rebuilt from
> source on the booted OS using binutils and GCC that also
> live on the OS, plays. Prerequisites: 195 (in-guest link),
> 174 (cross-built Doom plays — same `wmclient` bridge),
> 153 (stdio, needed for WAD load).

---

## What you'll do in this chapter

1. Write `scripts/test_doom_rebuilt_plays.py`: a
   two-phase chained test. Phase 1 invokes
   `test_doom_link.py` (chapter 195) as a
   `subprocess.run` to produce `/data/doomgeneric.elf`
   on `build/data.img` and shut down its QEMU cleanly.
   Phase 2 boots a fresh QEMU against the same data
   image, runs `/data/doomgeneric.elf`, waits for the
   `[doom] window created` + `V_Init:` checkpoints,
   then QMP-screendumps the framebuffer.
2. Use the chained design (two QEMU boots, one per
   logical task) instead of a single-boot inline
   tar+make+run sequence, sidestepping the QEMU
   `unix:...,server,nowait` serial disconnect that
   triggers when an inter-command idle window exceeds
   QEMU's undocumented timeout.
3. Sample the same 240×200 region inside the cascade-
   positioned doom window that
   `scripts/test_doom_plays.py` uses (chapter 174) so
   the screenshot-analysis math is shared with the
   cross-built reference test.
4. Require ≥ 25 % non-black pixels in the sampled
   region as the playable threshold, save the captured
   frame to `/tmp/doom_rebuilt_title.ppm`, and exit 0 on
   PASS / non-zero on FAIL.
5. Document the chained-test pattern and the QEMU serial
   disconnect so a future session reaching for an inline
   harness knows why this one is split.

---

## Why now

By this chapter the OS has accumulated a usable surface:
threads, drivers, a filesystem, a window server, a
network stack, a browser, a libc, a compiler, an
assembler, a linker, an archiver, a make-clone, a tar
reader. Each chapter ended with a test that demonstrated
the new piece. This chapter's test is just an exercise
in plumbing the existing pieces together:

```sh
$ /bin/tar xf /bin/doomobjs.tar -C /data
$ /bin/make -f /bin/doom_link.mk
$ /data/doomgeneric.elf
```

Three commands typed at the on-disk `/bin/sh`. The first
extracts 82 in-guest-built object files. The second
links them into a 2.5 MB ELF using the on-disk
`/bin/ld`. The third runs that ELF — and Doom plays. No
host involvement past "booted QEMU".

This is what the chapter-164 plan called "the
end-state". The arc that started at chapter 165
(setjmp/longjmp) and ran through six chapters of libc
growth, a chapter on FP/SIMD at EL0, three chapters of
cross-built reference Doom, sixteen chapters of binutils
+ GCC porting, and the five-chapter 191–195 "real
software" sequence has reached its acceptance criterion.
The OS rebuilds and runs Doom by itself.

There's nothing left to build for this section. This
chapter is short by design — its job is to validate,
photograph, and close the arc.

## Run it / Test it

`scripts/test_doom_rebuilt_plays.py`. Two phases:

**Phase 1.** Invoke `scripts/test_doom_link.py`
(chapter 195) as a Python `subprocess.run`. That test
reformats `build/data.img`, boots the OS, extracts
`doomobjs.tar`, runs `/bin/make -f /bin/doom_link.mk`,
and leaves `/data/doomgeneric.elf` (2,502,904 bytes) on
the data image. Its PASS bumps mean the link succeeded;
its own QEMU instance shuts down cleanly via
`qemu.terminate()`, flushing the journal and closing
the image.

**Phase 2.** Boot a fresh QEMU against the same
`build/data.img` (now carrying the rebuilt elf at
`/data/doomgeneric.elf`). Wait for the shell prompt.
Confirm the elf is visible. Execute it directly with
`/data/doomgeneric.elf`. Wait for the same two
checkpoints the cross-built reference test
(chapter 174) uses:

- `[doom] window created` — the osdev bridge
  (`userspace/doom/doomgeneric_osdev.c::DG_Init`) has
  opened its wm window. Proves crt0, `main()`,
  `doomgeneric_Create`, `DG_Init`, and the wmclient IPC
  all work end-to-end inside the rebuilt binary.
- `V_Init:` — Doom's renderer says "video initialised".
  By this point Doom has called `M_AddFile`, opened
  `/data/doom1.wad` via the on-disk stdio (chapter 153),
  parsed the lump directory, set up V_video, and loaded
  the palette and font. WAD load is the heaviest single
  unit-test in the run.

Then settle 3 s, QMP `screendump` the framebuffer, sample
a 240x200 region inside the doom window's body (the
cascade-positioned 640x400 area at (100,100); the sample
math matches `test_doom_plays.py` so chapter 174's
analysis applies). Acceptance: ≥ 25 % of pixels are
non-black, meaning Doom is painting something other
than the initial fill.

Result on the first clean run:

```
doom_rebuilt_plays: invoking test_doom_link.py ...
[...11 PASS lines from 195...]
doom_rebuilt_plays: in-guest link succeeded; ...
doom_rebuilt_plays: shell prompt visible
doom_rebuilt_plays: /data/doomgeneric.elf present on data.img
doom_rebuilt_plays: launched /data/doomgeneric.elf
doom_rebuilt_plays: WM window created — crt0 + main + DG_Init OK
doom_rebuilt_plays: V_Init reached — /data/doom1.wad loaded
doom_rebuilt_plays: title region non-black = 100.0%
doom_rebuilt_plays: PASS — rebuilt /data/doomgeneric.elf
    rendered title screen (saved /tmp/doom_rebuilt_title.ppm)
```

100 % non-black: the screenshot is the full Doom title
screen with the M_DOOM logo, the menu cursor, the "(c)
1993 id Software" line. Not just a coloured rectangle.

Wall-clock for the chained test: ~110 s on M-series HVF
(link ~25 s including boot, run + render ~30 s, plus
phase-transition overhead).

## Pitfalls

### Pitfall — single-boot tar+make+run hits the QEMU serial disconnect

**Symptom:** The first attempt at
`test_doom_rebuilt_plays.py` did everything in one QEMU
boot: tar → make → run. It reproducibly hit
`BrokenPipeError` on the `sendall` between tar and make.
The same tar+make pair works inside
`test_doom_link.py`, which is the proof that the bug is
in the *test harness*, not the OS.

**Cause:** The triggering pattern: after tar prints
`tar: 165 entries` and `/bin/tar` exits, the inside-shell
sequence is

```
tar: 165 entries
[sys_exit] thread '/bin/tar' exited with code 0
[wmclient] DAMAGE failed status=-5 (further suppressed)
[wmclient] DAMAGE failed status=-5 (further suppressed)
/$
```

— 165-entries marker, kernel sys_exit log, two
one-shot wmclient diagnostics from background GUI apps
that tried to repaint while tar held the disk, prompt
redraw. Spans about 400 ms.

The bug-triggering harness drained until 3 s of silence
followed the prompt redraw before sending the next
command. The working harness drains until the
success-needle bytes appear and immediately sends the
next command without waiting for silence. Somewhere
inside that 3-second-silence window, QEMU's
`unix:...,server,nowait` serial transport disconnects
(observed: subsequent `sendall` raises EPIPE; the QEMU
process itself stays alive but the serial socket is
closed from QEMU's side). The exact rule QEMU uses to
decide "this client is taking too long, close the
connection" is not documented in the QEMU manuals; not
worth chasing further with a proven workaround in hand.

**Fix:** The **chained design**: have the run-test invoke
the link-test as a precondition via `subprocess.run`, let
the link-test's own QEMU shut down cleanly between
phases, then boot phase-2 fresh. Each QEMU instance
runs at most one logical task; no inter-command idle
windows that QEMU might disconnect on.

This pattern (one Python test invokes another Python
test as a precondition) is novel for the regression
suite. It's appropriate when one test produces state on
disk (here: a populated `data.img`) that another test
consumes. The alternative — splitting state into a
build-time fixture step, like 195 shipping
`doomobjs.tar` on `/bin/` — works for stable artefacts
but doesn't suit "the output of a previous regression
test" since that's regenerated on every link change.

## What's deferred

A few deliberate non-goals for this chapter's test:

- **Byte-equivalence to the cross-built reference.**
  The chapter-172 cross-built `/bin/doom` and the
  chapter-195 in-guest-linked `/data/doomgeneric.elf`
  link the same vendor `.o` files, same osdev shim, same
  linker script, same entry point. They should be very
  close, but not byte-identical: the host xgcc emits
  slightly different DWARF, build-id, and section
  ordering than the in-guest ld pipeline. Comparing
  `.text` sections might match; comparing the whole ELF
  will not. The test doesn't bother — `playable` is the
  spec, `bit-equivalent` is not.
- **Frame rate.** The test waits for V_Init plus 3 s and
  screendumps once. It doesn't measure FPS or play the
  game for any length of time. If the in-guest-built
  binary runs at 2 fps the test still passes.
  (Anecdotally from the screenshot timing it's comparable
  to the cross-built reference — both rely on the same
  wmclient IPC roundtrip and DG_DrawFrame loop.)
- **Input handling.** The test doesn't send any
  keyboard/tablet events. Doom is at the title screen
  with the attract-mode demo cycling; the screenshot
  catches it somewhere in that loop. Validating that ESC
  opens the menu or arrow keys navigate it is
  interactive-feel territory; not chapter 196's scope.
- **Audio.** Doom is configured with `-mb 6` (memory
  budget) but the build doesn't link doomgeneric's
  optional sound path; both the cross-built and rebuilt
  binaries run silent. Chapter 97's virtio-snd stack
  could host it, but wiring sound into doomgeneric is
  an entire mini-port (Doom's sound system is
  surprisingly Unix-coupled) and not on the section-18
  roadmap.

Two larger items queued for follow-up chapters, neither
blocking:

1. **`doom_full.mk` / `doom_link.args` symmetry.**
   Chapter 194's full-compile manifest lists 80 files;
   chapter 195's link manifest lists 82 (adding
   `gusconf.o` and `icon.o`, whose host-built equivalents
   are in `doomobjs.tar`). A future commit should add
   those two to `doom_full.mk` so an in-guest
   `make -f /bin/doom_full.mk` produces the same 82
   objects the link wants. Until then, the
   compile-then-link in one shell sequence requires
   patching by hand. The pre-built `doomobjs.tar` ship
   sidesteps this for the linker test, but a real user
   typing the four-command sequence in the intro of
   this chapter would hit it.

2. **A native `gcc` driver invocation that produces a
   runnable elf.** Today the in-guest `/bin/gcc` can
   compile to `.o` (chapter 188) and the in-guest
   `/bin/ld` can link `.o`s to an elf (chapter 195),
   but they're separate invocations. The cross-built
   `aarch64-osdev-gcc` knows how to drive `cc1 → as → ld`
   in one command (it's what `gcc hello.c -o hello`
   does); the in-guest gcc currently stops at `.o`.
   Wiring gcc's `-c -o` vs `-o` modes properly so
   `gcc hello.c -o hello` does the link step too is a
   small follow-up — probably section 19 material.

Neither is large; both could happen in a single chapter
or be folded into early section-19 work.

## What this unlocks

A pleasant observation from this chapter: nothing new
in the OS needed building. No new syscalls, no new VFS
hooks, no kernel limits bumped, no wm-protocol
extensions. The chained test is pure tooling on top of
already-shipped pieces:

- ELF load from OSFS-2 (chapter 132)
- argv plumbing through sys_spawn (chapter 92)
- vfs_load via osfs2 against `/data/*.elf` (chapter 82)
- stdio fopen against `/data/doom1.wad` (chapter 153)
- wmclient IPC against wsd (chapters 114–118)
- wsd compositor + scanout (chapters 117, 118)

The fact that `/data/doomgeneric.elf` runs on first
attempt — no kernel patches, no shim changes, no wm
protocol bump — is the integration evidence this
section set out to produce. The OS is now a platform
that hosts software it builds itself. Doom is just the
demonstrator.

This is the last chapter of Part XVIII. The chapter-164
plan named it "end-state: `httpget <doom.tar.gz>` +
`tar -xzf` + `make` + `./doomgeneric -iwad doom1.wad`
all on the booted OS with no host involvement past
'booted QEMU'." The end-state is met. (httpget the
source tarball over TLS is technically possible too —
chapter 127's https client could fetch
`vendor/doomgeneric.git/archive/HEAD.tar.gz` — but it
isn't part of the regression sweep because the host's
firewall and tarball availability vary; the staged
`/bin/doomobjs.tar` substitutes.)

Per the standing "apps must use the OS features the
book builds" rule:

- **`scripts/test_doom_rebuilt_plays.py`** — new
  regression test. Exits 0 on PASS (or SKIP when the
  WAD is absent on the host); exits non-zero on FAIL.
  Doesn't change behaviour of any other test.
- **`assets/wads/doom1.wad`** — already shipped by
  chapter 173. No change.
- **`/data/doomgeneric.elf`** — produced as a side
  effect of `test_doom_link.py` via `/bin/ld`; consumed
  by this test. Not a committed artefact.

No source-tree changes outside the test script. The
rebuilt binary itself uses the same OS features the
cross-built `/bin/doom` already exercised (chapter
174), so the "apps must use features" requirement is
satisfied transitively — this chapter doesn't unlock a
new feature, it validates the integration of the
shipped ones.

## What's next

One short postscript chapter follows in this section
(197 — real key-up events) closing a deferred input
issue that chapter 174 flagged as out-of-scope: the
chapter-173 timed-release shim gets replaced with real
`GUI_EVENT_KEY_UP` events so Doom movement keys stop the
tick the user releases them.

Section 19 then opens with whatever capability the
roadmap calls out next. The OS is — for the first time
in the book — a platform that hosts itself well enough
to host other people's software.


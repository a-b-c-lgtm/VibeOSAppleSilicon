# Chapter 195 — In-guest Doom link, `doomgeneric.elf` produced

> **Milestone in this chapter:** link the 82 vendor `.o` files
> plus a small runtime archive into one Doom ELF, inside the
> guest, using `/bin/ld`.
> **Code referenced:**
> - The runtime archive `/bin/libdoomrt.a` (bundling `crt0.o`,
>   `doomgeneric_osdev.o`, `setjmp.o`, `cstring.o`,
>   `wmclient.o`)
> - `/bin/doom_link.args` (binutils `@file` response file)
> - [scripts/test_doom_link.py](../../../scripts/test_doom_link.py)
>
> **At the end of this chapter** you will have a single
> `/data/doomgeneric.elf` (~2.5 MB) linked end-to-end inside
> the guest in ~12 s under HVF, with `test_doom_link.py` at
> **PASS 11 / FAIL 0**. Prerequisites: chapter 194 (full
vendor compile), 180 (`/bin/ld` is real binutils), 190
> (OSFS-1 subdirs).

---

## What you'll do in this chapter

1. Bundle the five osdev runtime objects (`crt0.o`,
   `doomgeneric_osdev.o`, `setjmp.o`, `cstring.o`,
   `wmclient.o`) into one cross-built archive
   `/bin/libdoomrt.a` (~198 KB) using host
   `aarch64-elf-ar rcs`, so `/bin/ld` pulls only the
   members Doom actually references.
2. Use binutils' `@file` response-file feature: ship the
   82 vendor `.o` paths as `/bin/doom_link.args` and let
   `/bin/ld` splice them itself, so the recipe stays under
   the chapter-91 `THREAD_ARGS_MAX=128` /
   `MAX_SPAWN_ARGV=16` kernel limits with zero kernel
   changes.
3. Ship four fixtures on OSFS-1 (`osdev.ld`,
   `libdoomrt.a`, `doom_link.mk`, `doom_link.args`) and a
   pre-built `doomobjs.tar` (6.4 MB) of the cross-built
   `.o` files so the link can be re-run in ~12 s instead
   of redoing the 25-min compile.
4. Make `wm_window_dirty` in `userspace/libgui/wmclient.c`
   one-shot per process so background GUI clients don't
   flood the serial channel with `DAMAGE failed status=-5`
   while heavy disk I/O is in flight.
5. Write `scripts/test_doom_link.py` (11 expectations, 4
   phases) and keep the diagnostic harness
   `scripts/_dbg_doom_link_tar.py` per the debug-scripts
   policy.

---

## Why now

Chapter 194 compiled 77 vendor `.c` files in-guest into
77 ELF AArch64 `.o` files — but stopped there. The link
step was deferred for two reasons: the
[`userspace/doom/doomgeneric_osdev.c`](../../../userspace/doom/doomgeneric_osdev.c)
shim depends on `wmclient` (window-server IPC client), and
the resulting link line was going to blow past the
chapter-117 `MAX_SPAWN_ARGV=16` / chapter-91
`THREAD_ARGS_MAX=128` kernel limits on argv size.

This chapter solves both problems and produces an
executable. The win is bigger than just "doomgeneric
links": it's the first time the OS has produced a
multi-megabyte executable using its *own* linker from its
*own* object files. Up to chapter 194 the largest thing
`/bin/ld` had produced in-guest was a 4 KB
`hello` (chapter 187).

## The two design choices

### Choice 1 — bundle the runtime as an archive, not a list

The osdev shim itself isn't a vendor file. It's project
code, and it pulls in `wmclient.o` + `setjmp.o` +
`cstring.o` + `crt0.o`. Five files. They could ship as
five separate `.o` blobs on `/bin` and be listed on the
link line, but that's noisy. Bundle them into one
archive:

```
/bin/libdoomrt.a   (198 KB)
├── crt0.o                    _user_start, argv parsing, _exit
├── doomgeneric_osdev.o       wmclient bridge for DoomGeneric
├── setjmp.o                  Doom's z_zone error path uses setjmp
├── cstring.o                 freestanding memcpy / memset / strlen
└── wmclient.o                window-server IPC client
```

Built on the host with `aarch64-elf-ar rcs` because this
step produces the archive, not consumes it (consuming is
what `/bin/ld` does in-guest). The chapter-180 binutils
ld already speaks `--whole-archive` and the default
"pull-symbols-as-needed" mode, so a positional
`libdoomrt.a` on the link line behaves like any other
archive.

Why an archive? Two reasons:

1. **Link economy.** ld only extracts members whose
   symbols are referenced — Doom doesn't call
   `strncasecmp` from the on-disk `cstring.o`, so it
   doesn't end up in the ELF.
2. **Command-line economy.** Adding five paths to the
   link line widens the argv; one archive path keeps it
   narrow. (Choice 2 below makes the argv-width issue
   moot, but at the time of design the `@file` answer
   hadn't been picked yet.)

The "force crt0 to appear first in the archive" problem
is handled by `ENTRY(_user_start)` in the linker script
(below): ld walks for the entry symbol, finds
`_user_start` in `crt0.o`, extracts the member, and
satisfies the entry-point relocation. From that point
forward everything else falls out of normal
symbol-resolution order.

### Choice 2 — use binutils `@file` for the 82 vendor paths

The 82 vendor object paths are `/data/src/am_map.o`,
`/data/src/d_event.o`, …, all 82. Average length about
20 characters. Total: **1,650 bytes** on the link line.

The chapter-91 kernel caps the argv buffer per spawned
thread at 128 bytes (`THREAD_ARGS_MAX`) and the count at
16 entries (`MAX_SPAWN_ARGV`). The link recipe would need
1,650 bytes and 84 entries (ld + -T + osdev.ld + -o +
output + 82 inputs + libdoomrt.a). Both limits broken by
an order of magnitude.

Bumping both limits is one option. This chapter doesn't,
because:

- The kernel limits are conservative for a reason.
  Bumping them globally to support one link recipe taxes
  every other spawn (kernel-side buffer growth, slab
  pressure).
- The POSIX-idiomatic answer is **binutils' `@file`
  response files**, implemented in libiberty's
  [`expandargv()`](https://gcc.gnu.org/onlinedocs/libiberty/Argument-Parsing.html).
  When ld sees `@/bin/doom_link.args` in its argv, it
  *opens the file* and splices each whitespace-separated
  token into its internal argv as if they'd been
  command-line args.
- The chapter-180 `/bin/ld` is real GNU binutils 2.44,
  so `expandargv()` is already there — no porting work,
  it just works.

The recipe shrinks to:

```sh
/bin/ld -T /bin/osdev.ld -o /data/doomgeneric.elf @/bin/doom_link.args /bin/libdoomrt.a
```

Eight argv slots, ~90 bytes. Fits comfortably. ld reads
the 1.65 KB args file itself from `/bin/doom_link.args`
in its own address space. **No kernel changes were
made for this chapter.** That `@file` is the right answer
when argv would otherwise explode is one of those quiet
binutils features that pays off years after it was
written.

## The fixtures shipped on `/bin/`

OSFS-1 (chapter 26 / 190) has a hard 19-byte filename
cap. The fixture names were chosen to fit:

| File | Bytes | What |
| --- | --- | --- |
| `osdev.ld` | 8 | linker script (`ENTRY(_user_start)`, USER_LOAD_ADDR=0x1000100000) |
| `libdoomrt.a` | 11 | runtime archive (5 members) |
| `doom_link.mk` | 12 | one-rule makefile invoking the link |
| `doom_link.args` | 14 | 82 `/data/src/*.o` paths, one per line |
| `doomobjs.tar` | 12 | pre-built 6.4 MB tarball of the 82 objects |

The first iteration used `doomgeneric_objs.tar` (20
bytes) and `mkosfs` rejected it with `name too long (>
19 bytes)`. Renamed before commit. Whenever a new fixture
ships, count bytes (not characters) and stay ≤ 19.

`doom_link.mk` is the smallest interesting makefile in the
book so far:

```make
LD      = /bin/ld
LDFLAGS = -T /bin/osdev.ld
OUTPUT  = /data/doomgeneric.elf
RUNTIME = /bin/libdoomrt.a
ARGS    = /bin/doom_link.args

all: $(OUTPUT)

$(OUTPUT):
	$(LD) $(LDFLAGS) -o $(OUTPUT) @$(ARGS) $(RUNTIME)

clean:
	rm -f $(OUTPUT)
```

`@$(ARGS)` after variable expansion is `@/bin/doom_link.args`.
The chapter-194 `/bin/make` buffer sizes (MK_MAX_VAL=4096,
recipe expansion buffer 2048) are oversized for this — the
expanded recipe is ~90 chars — so no further bumps were
needed.

## The host build of the .o files

The 82 vendor `.o` files would take 25 minutes to compile
in-guest (chapter 194 showed exactly that). To get a
fast linker regression test that doesn't redo the compile
on every run, the objects are cross-built on the host:

```make
# Makefile excerpt
DOOMGENERIC_OBJS_TAR := $(BUILD)/doomobjs.tar
$(DOOMGENERIC_OBJS_TAR): $(DOOM_VENDOR_OBJS)
	python3 scripts/mktar.py $@ $(BUILD)/vendor/doomgeneric src
```

`scripts/mktar.py` packs `$(BUILD)/vendor/doomgeneric` as
`src/` inside the tarball. The guest extracts with
`/bin/tar xf /bin/doomobjs.tar -C /data`, ending up with
`/data/src/am_map.o`, `/data/src/d_event.o`, etc — exactly
the paths `doom_link.args` references.

This is the same "host cross-build the heavy bits,
in-guest do the interesting work" pattern chapter 172
used for the cross-built Doom reference binary. The point
of 195 isn't to re-test the compile (194 did that). It's
to test the link.

`doom_link.args` lists **82** objects. `doom_full.mk`
(chapter 194) lists **80**. The two missing in 194 are
`gusconf.o` and `icon.o` — a drift between fixture and
host build that 194 quietly tolerated because it only
needs to compile, not link. The link step exposes the
drift as undefined references (`mus_pitch`, the icon
glyph table). For 195, both paths get added to the args
file; chasing the symmetric fix to `doom_full.mk` is
queued for 196.

## Pitfalls

### Pitfall — `[wmclient] DAMAGE failed status=-5` DoS

**Symptom:** The test consistently failed at step 2b with
`BrokenPipeError` on the second `send_cmd` after the tar
extract. The tar extracted fine (165 entries, exit 0).
`/data/src/` was populated. Yet the next `send_cmd` would
fail to write to the QEMU unix socket.

A diagnostic harness (`scripts/_dbg_doom_link_tar.py`)
captured the post-tar serial stream and showed why:

```
src/z_zone.o
tar: 165 entries
[sys_exit] thread '/bin/tar' exited with code 0x0000000000000000
/$ [wmclient] DAMAGE failed status=-5
[wmclient] DAMAGE failed status=-5
[wmclient] DAMAGE failed status=-5
...        (a few hundred more)
[wmclient] DAMAGE failed status=-5
```

**Cause:** Background GUI apps (taskbar, launcher, clock)
had been attempting to repaint while `/bin/tar` hammered
the disk. With wsd's compositor occasionally returning
`WM_ERR_NOTIMPL (-5)` for damage requests while its
scanout slot was transiently unavailable, every retry
printed a one-line diagnostic. With 5+ background apps
retrying continuously, the rate climbed to hundreds of
lines per second and saturated the serial channel.

The chapter-130 book entry noted this as "DAMAGE failed
status=-5 retry chatter" and recommended draining the
serial buffer for 2 s after boot to work around it.
That works for short tests, but a 15-second tar extract
followed by another 12-second link gives the spam plenty
of new ground to flood.

**Fix:**
[`userspace/libgui/wmclient.c::wm_window_dirty`](../../../userspace/libgui/wmclient.c)
now uses a one-shot `static int once` to print the first
failure (with `(further suppressed)` appended) and
silently return -1 for the rest of the process's lifetime.
The function's contract is unchanged — callers still see
the -1 return code and can act on it — only the noise is
suppressed. This kills the spam for every wmclient
binary on the system, since they all link the same
`libgui.a` copy.

Lesson: when an API returns a recoverable error code that
callers already handle, the API should not print to a
shared output channel on every failure. Either return the
error silently and let the caller decide whether to log,
or rate-limit at the source. Both work; spamming
unconditionally does not.

### Pitfall — OSFS-1 19-byte filename overflow

**Symptom:** The first build attempt shipped
`doomgeneric_objs.tar` (20 bytes) and mkosfs correctly
rejected it:

```
ValueError: name too long (> 19 bytes): doomgeneric_objs.tar
```

**Cause:** OSFS-1 (chapter 26) packs filenames into a
fixed 19-byte field in the directory entry.

**Fix:** Renamed to `doomobjs.tar` (12 bytes). All four
195 fixtures verified under the limit before commit. The
19 limit is bytes (UTF-8 in principle, but in practice all
the fixture names are ASCII).

### Pitfall — fixture/host `.o` count drift

**Symptom:** First link attempt produced undefined
references for `mus_pitch` (used by `s_sound.c`,
satisfied by `gusconf.c`) and the icon glyph table (used
by `i_video.c`, satisfied by `icon.c`).

**Cause:** `doom_link.args` originally mirrored
`doom_full.mk`'s 80 files exactly. The host build
includes both `gusconf.o` and `icon.o`; the chapter-194
`doom_full.mk` quietly didn't, but at compile-time the
missing references never surfaced (each `.c` file
compiles independently). The link step is where these
drifts become visible.

**Fix:** For 195, add `/data/src/gusconf.o` and
`/data/src/icon.o` to `doom_link.args`. The symmetric fix
to `doom_full.mk` (so the in-guest 194 compile produces
the same 82 objects 195 expects) is queued for 196.

### Pitfall — `/bin/ld` emits a benign RWX warning

**Symptom:** After fixing the wmclient spam, the test got
further: make completed, ld produced the ELF, but the
test failed at step 3b's "no link errors" check because
of this line in the captured output:

```
/bin/ld: warning: /data/doomgeneric.elf has a LOAD segment with RWX permissions
```

**Cause:** This is a chapter-155 carry-over: the linker
script (`userspace/linker_user.ld`, copied to OSFS as
`/bin/osdev.ld`) packs `.text` and `.data` into a single
PT_LOAD for simplicity. ld 2.39+ warns about this
because RWX-loaded segments are a hardening anti-pattern
on hosted platforms. For the flat freestanding model used
here it's harmless.

**Fix:** The test's original check was `b"ld: " not in
out` — which the warning string trivially matches. Walk
lines and skip any whose `ld:` tail starts with
`warning:`. Real ld errors (`ld: error:`, `ld: undefined
reference`, etc) still trip the check.

A future cleanup would be to split the linker script so
`.text` is PT_LOAD-R-X and `.data` is PT_LOAD-RW-, but
that's a chapter-155 follow-up, not a 195 fix.

### Pitfall — `/bin/wc` doesn't accept `-c`

**Symptom:** The size-sanity check in step 4b uses
`/bin/wc`. The first iteration passed `-c` for byte-only
output:

```
/bin/wc -c /data/doomgeneric.elf
wc: cannot open -c: No such file or directory
```

**Cause:** The chapter-148 `/bin/wc` is the simple
lines/words/bytes counter — no `-c`/`-l`/`-w` flags. It
prints `<lines> <words> <bytes> <path>` for any path
given as `argv[1]`.

**Fix:** The test now invokes wc without a flag and parses
the third column. (A future `/bin/wc` could grow `-c`,
but the four-column output is exactly what other tests
already use, so changing the parser was cheaper than
changing wc.)

## Run it / Test it

`scripts/test_doom_link.py` is **PASS 11 / FAIL 0**, four
phases. Standard boot-to-prompt preamble, then:

```python
# Phase 1: fixtures shipped
/bin/ls /bin/doom_link.mk      # PASS 1a
/bin/ls /bin/doom_link.args    # PASS 1b
/bin/ls /bin/libdoomrt.a       # PASS 1c
/bin/ls /bin/doomobjs.tar      # PASS 1d

# Phase 2: extract pre-built objects
/bin/tar xf /bin/doomobjs.tar -C /data      # PASS 2a
/bin/ls /data/src/doomgeneric.o             # PASS 2b

# Phase 3: link
/bin/make -f /bin/doom_link.mk              # PASS 3a (make: built 'all')
                                            # PASS 3b (no ld:/make: errors)

# Phase 4: artifact sanity
/bin/ls /data/doomgeneric.elf               # PASS 4a
/bin/wc /data/doomgeneric.elf               # PASS 4b (500K..50M)
```

Producing the ELF, plus the size sanity check, is enough
to call the link successful. Running the binary is
chapter 196's job.

Final smoke test result:

```
PASS: boot: reached shell prompt
PASS: step 1a: /bin/doom_link.mk shipped on OSFS-1
PASS: step 1b: /bin/doom_link.args shipped on OSFS-1
PASS: step 1c: /bin/libdoomrt.a shipped on OSFS-1
PASS: step 1d: /bin/doomobjs.tar shipped on OSFS-1
PASS: step 2a: /bin/tar extracted vendor objs without errors
PASS: step 2b: /data/src/ populated with .o files
PASS: step 3a: /bin/make completed link rule
PASS: step 3b: no link errors
PASS: step 4a: /data/doomgeneric.elf produced
    (doomgeneric.elf = 2502904 bytes)
PASS: step 4b: /data/doomgeneric.elf size is plausible

PASS: 11
FAIL: 0
```

Wall-clock: ~12 s under HVF on M-series.

Regression sweep held after this chapter. All prior
in-guest toolchain tests still pass after the wmclient
change:

- `test_make_v2.py` 9/0
- `test_tar.py` 8/0
- `test_make_port.py` 14/0
- `test_gcc_hello.py` 10/0
- `test_gcc_stdio.py` 7/0
- `test_gcc_bf.py` 6/0
- `test_gcc_sys_stat.py` 6/0
- `test_doom_pilot.py` 8/0
- `test_doom_full.py` 8/0 (held — 25 min, not in
  every-commit sweep)
- `test_doom_link.py` 11/0 (this chapter)

68 + 11 = **79/0** across the in-guest toolchain
regression sweep (194's 25-min compile run on demand).

## What this unlocks

The OS now produces multi-megabyte executables from its
own linker, fed by its own object files, with zero kernel
changes to the spawn/argv path — the `@file` response-file
feature absorbed the whole link line's width. Running the
produced binary is the next step (chapter 196); the
rebuilt Doom playing inside the desktop closes the
guest-gcc bring-up loop.

Per the standing "apps must use the OS features the book
builds" rule:

- **`userspace/libgui/wmclient.c`** — one-line behaviour
  change: `wm_window_dirty` printf is now one-shot per
  process. Every wmclient-using binary in the system
  (taskbar, launcher, notepad, paint, pixapp, browser,
  doomgeneric, gui_term, etc.) gets quieter serial logs
  under heavy disk I/O.
- **`assets/osfs/doom_link.mk`** — new fixture, ships to
  `/bin/doom_link.mk` on OSFS.
- **`assets/osfs/doom_link.args`** — new fixture, 82
  paths, ships to `/bin/doom_link.args` on OSFS.
- **`build/userspace/doom/libdoomrt.a`** — new build
  artifact, cross-built from existing userspace `.o`
  files, ships to `/bin/libdoomrt.a` on OSFS.
- **`build/doomobjs.tar`** — new build artifact, 6.4 MB
  tarball of host cross-built `.o` files, ships to
  `/bin/doomobjs.tar` on OSFS.
- **`scripts/test_doom_link.py`** — new regression test,
  11 expectations.
- **`scripts/_dbg_doom_link_tar.py`** — diagnostic
  harness used to capture the wmclient spam (kept per
  the debug-scripts-policy memory rule).

No existing app's behaviour changed except for the
wmclient noise reduction. The doomgeneric vendor sources
under `userspace/vendor/doomgeneric/` are unmodified.

## What's next

Chapter 196 — run `/data/doomgeneric.elf` inside the
guest. Open questions:

1. Does it find a WAD? Chapter 173 staged
   `/bin/doom1.wad`; the in-guest binary will need to
   open that through the chapter-153 stdio (`fopen`,
   `fseek`, `fread`).
2. Does the wmclient bridge paint frames? The bridge
   symbols all resolved at link, but runtime is a
   different test. Doomgeneric calls `DG_DrawFrame`
   every frame; the shim turns that into a
   `wm_window_dirty` call against the per-window FB.
3. Frame rate? On SMP-2 HVF, doomgeneric's main loop
   plus the IPC roundtrip will determine playability.
   Anything above ~10 fps will feel like Doom; below
   that and an optimisation chapter is queued.

Also queued: chase the `gusconf.o` / `icon.o` drift in
`doom_full.mk` so a fresh in-guest 194 compile would
produce all 82 objects 195 expects to find.


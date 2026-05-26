# Chapter 133c — In-guest Doom rebuild, the pilot

> **Milestone in this chapter:** prove the in-guest `make +
> gcc` pipeline on three carefully-chosen DoomGeneric vendor
> sources before scaling to the full set.
> **Code referenced:**
> - [assets/osfs/doom_pilot.mk](../../../assets/osfs/doom_pilot.mk)
> - [scripts/test_doom_pilot.py](../../../scripts/test_doom_pilot.py)
>
> **At the end of this chapter** you will have three vendor
> objects (`m_random.c`, `m_bbox.c`, `m_fixed.c`) compiled
> from a tarball extracted at boot, exercising cpp's plain,
> `-I`, and `-isystem /bin` lookups in one run, with
> `test_doom_pilot.py` at **PASS 8 / FAIL 0**.
> Prerequisites: chapter 132g (`/bin/gcc`), 132j (`sys/`
> headers on OSFS-1), 133a (`/bin/tar`), 133b (expanded
> `/bin/make`).

---

## What you'll do in this chapter

1. Pick three DoomGeneric vendor sources — `m_random.c`
   (zero includes), `m_bbox.c` (same-dir + one system
   header), `m_fixed.c` (chained libc headers) — so the
   compile path exercises cpp's plain, `-I`, and
   `-isystem /bin` lookups in one run.
2. Write `assets/osfs/doom_pilot.mk` (~45 LoC): a
   `/bin/make` fixture whose targets and deps are absolute
   paths under `/data/src/`, sidestepping the spawn-cwd
   trap below.
3. Add `doom_pilot.mk` to `OSFS_FILES` and the mkosfs
   invocation so `/bin/doom_pilot.mk` lands at boot.
4. Write `scripts/test_doom_pilot.py` (~260 LoC): reformat
   `/data/`, extract `/bin/doomgeneric.tar`, run make,
   verify three ELF `.o` files with `e_machine == 0xB7`.
   Eight expectations across five steps.
5. Run the six earlier-chapter regressions to confirm
   60/0 still holds, then add this chapter's 8/0 to the
   sweep total.

---

## Why now

The full Doom rebuild has three big rocks:

1. Compile every vendor `.c` file — 82 files, lots of libc
   surface, lots of opportunity for missing headers /
   missing symbols / printf-format gaps.
2. Link a binary — needs `libgui.a` shipped on `/bin/`,
   needs the osdev shim (`doomgeneric_osdev.c`), needs to
   actually run.
3. Stage a WAD so the binary has something to render.

Each of those is its own chapter. Before signing up for any
of them, prove the *fundamental* loop works:
tarball-on-disk → `/bin/tar xf` → `/bin/make` → `/bin/gcc`
→ ELF objects on disk. If that loop is broken, the
remaining work is all blocked.

So 133c is the pilot: same loop, but with three small
vendor files. If the pilot passes, the rest is just
iteration on libc gaps — the plumbing is sound.

## File selection

DoomGeneric's source tree (in `vendor/doomgeneric/src/`)
has 95 `.c` files. Three were picked:

| File | LoC | Includes | What it exercises |
|------|-----|----------|-------------------|
| `m_random.c` | 65 | none | Zero-include compile. Pure C, three functions, a 256-byte rnd table. The smoke test that proves `/bin/gcc` can produce an object file from a real upstream source file with no surrounding scaffolding. |
| `m_bbox.c`   | 54 | `m_bbox.h` → `<limits.h>` + `m_fixed.h` | Same-dir local include + system `<…>` include. Proves cpp's `-I` and `-isystem /bin` paths both work. |
| `m_fixed.c`  | 62 | `<stdlib.h>`, `doomtype.h` → `<strings.h>`, `<inttypes.h>`, `<limits.h>` | The full libc-bracket include search. Proves a transitive chain through libc headers compiles cleanly. |

Total time per file is roughly the same: cc1 + as + xgcc
spawn cycle dominates. Together they exercise enough of
the toolchain that if these three work, the other 79 will
mostly work too.

## The fixture: `/bin/doom_pilot.mk`

```make
CC = /bin/gcc
CFLAGS = -O0

DIR = /data/src

OBJS = $(DIR)/m_random.o \
       $(DIR)/m_bbox.o \
       $(DIR)/m_fixed.o

all: $(OBJS)

$(DIR)/%.o: $(DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
```

Compared to chapter 133b's `mk_test.mk` smoke fixture, the
only new thing is that the pattern rule's target and dep
are both **absolute paths**. That works because chapter
133b's `mk_split_pattern` splits on `%` regardless of what
surrounds it; for target `/data/src/m_random.o` the
prefix is `/data/src/`, the suffix is `.o`, the stem is
`m_random`, and the synthesized dep is
`/data/src/m_random.c`. Nothing in `/bin/make` cares that
those are deep paths.

Why absolute paths? See the next section.

## Pitfalls

### Pitfall — `sys_spawn` does NOT propagate cwd

**Symptom:** The first cut of `doom_pilot.mk` used relative
paths (`OBJS = m_random.o m_bbox.o m_fixed.o`, pattern
`%.o: %.c`) and the test cd'd into `/data/src` before
running make:

```sh
cd /data/src
/bin/make -f /bin/doom_pilot.mk
```

The shell's cwd updated correctly — the shell uses
`SYS_CHDIR` (chapter milestone-23). But the compile then
failed:

```
/bin/gcc -O0 -c m_random.c -o m_random.o
cc1: fatal error: m_random.c: No such file or directory
```

**Cause:** The chain is:

1. `sh` (cwd `/data/src`) calls `spawn("/bin/make", "-f /bin/doom_pilot.mk")`.
2. `/bin/make` starts with cwd **`/`**, not `/data/src`.
3. `/bin/make` calls `spawn("/bin/gcc", "-O0 -c m_random.c -o m_random.o")`.
4. `/bin/gcc` starts with cwd `/`.
5. `/bin/gcc` calls `spawn("/bin/cc1", "-O0 m_random.c ...")`.
6. `cc1` (cwd `/`) opens `m_random.c` → ENOENT.

`sys_spawn` ([`kernel/core/syscall.c:361`](../../../kernel/core/syscall.c#L361))
copies the path, copies the args, calls
`user_thread_create`, and `thread_inherit_fds`. It does
**not** copy the parent's `cwd[96]` field
([`kernel/core/syscall.c:1385`](../../../kernel/core/syscall.c#L1385)).
The new thread always starts at cwd `/`, the default set
by `user_thread_create` ([`kernel/core/thread.c:478`](../../../kernel/core/thread.c#L478)).

Fork *does* propagate cwd
([`kernel/core/syscall.c:1423`](../../../kernel/core/syscall.c#L1423)),
because that's part of POSIX fork semantics; spawn is a
local primitive and was never wired up the same way. The
omission didn't surface in earlier chapters because every
prior caller of spawn either:

- Used absolute paths (`/bin/<tool>` everywhere), or
- Worked from `/` anyway (the shell's default), or
- Was a single-process binary that didn't open any files.

The pilot is the first spawn-chain that (a) wants to be
in a non-`/` directory and (b) opens files relative to
that directory.

**Fix:** Two options.

**Option A — the proper fix.** Extend `sys_spawn` to copy
the parent's cwd into the new child, the way fork already
does. One-liner inside `sys_spawn` (after the
`thread_inherit_fds` call):

```c
for (size_t i = 0; i < TS_CWD_MAX; i++)
    t->cwd[i] = thread_current()->cwd[i];
```

This is the right answer long-term and it's small. But
making it the *correct* fix means writing a regression
test that proves cwd propagates across an arbitrary
spawn chain, and that test belongs in its own chapter.

**Option B — sidestep with absolute paths.** Make
`doom_pilot.mk` write every path in full:

```make
DIR = /data/src
OBJS = $(DIR)/m_random.o $(DIR)/m_bbox.o $(DIR)/m_fixed.o
$(DIR)/%.o: $(DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
```

Now no process in the chain depends on cwd. Works today;
no kernel patch.

Chapter 133c picks **Option B**. The cwd fix is queued for
a separate, narrow chapter so the test for "spawn
propagates cwd" can be written cleanly and tied to a
single change. The pilot's job is to prove the rebuild
loop, not to fix every gap it surfaces.

### Pitfall — `/bin/ls` only reads `argv[1]`

**Symptom:** Verifying that the three pilot sources
extracted correctly:

```sh
/bin/ls /data/src/m_random.c /data/src/m_bbox.c /data/src/m_fixed.c
```

Output: only the first file.

**Cause:** `userspace/ls/ls.c:78` checks `argc < 2` and then
*only* references `argv[1]` — it never loops over
`argv[2..argc-1]`.

**Fix:** A one-line `for` loop in `ls.c` would do it, but
that would muddy this chapter; the test just calls
`/bin/ls` three times instead. Real fix queued for a
smaller chapter focused on `/bin/ls`.

## How the pilot wires up

`assets/osfs/doom_pilot.mk` is added to `OSFS_FILES` in the
top-level `Makefile`, then mapped onto OSFS-1 in the
`mkosfs` invocation:

```make
OSFS_FILES = \
  ... \
  assets/osfs/doom_pilot.mk \
  ...

build/disk.img: ...
	$(MKOSFS) ... \
	  doom_pilot.mk=assets/osfs/doom_pilot.mk \
	  ...
```

That means at boot `/bin/doom_pilot.mk` exists.
`/bin/doomgeneric.tar` already ships (chapter 133a). All
the test needs to do is reformat `/data/` (so it starts
clean), extract the tarball, and run make.

## Run it / Test it

`scripts/test_doom_pilot.py` is **PASS 8 / FAIL 0**, five
steps:

1. **Fixture sanity** (2 expectations)
   - `cat /bin/doom_pilot.mk` returns content matching
     `m_random.o` + `%.o:` + `/data/src`
   - `/bin/ls /bin/doomgeneric.tar` shows the tarball
2. **Tar extraction** (2)
   - `/bin/tar xf /bin/doomgeneric.tar -C /data` exits 0
   - All three .c sources are present under `/data/src/`
3. **Make run** (2)
   - `/bin/make -f /bin/doom_pilot.mk` completes
     (`make: built 'all'`) and exits 0
   - No `fatal error` / `undefined reference` /
     `implicit declaration` printed
4. **Output objects** (1)
   - Three .o files at `/data/src/`, each starting with the
     ELF magic `\x7FELF`
5. **Architecture bonus** (1)
   - `m_random.o` header reports `e_machine == 0xB7`
     (EM_AARCH64)

The test reformats `/data/` via `scripts/mkosfs2.py` before
the run so the extraction is always exercised end-to-end —
no carried-over state from previous runs.

Transcript (abridged):

```
[/bin/tar output]
src/am_map.c
src/am_map.h
...
src/z_zone.c
src/z_zone.h
tar: 203 entries
[sys_exit] thread '/bin/tar' exited with code 0x0000000000000000

[/bin/make output]
/bin/gcc -O0 -c /data/src/m_random.c -o /data/src/m_random.o
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
[sys_exit] thread '/bin/as' exited with code 0x0000000000000000
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
/bin/gcc -O0 -c /data/src/m_bbox.c -o /data/src/m_bbox.o
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
[sys_exit] thread '/bin/as' exited with code 0x0000000000000000
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
/bin/gcc -O0 -c /data/src/m_fixed.c -o /data/src/m_fixed.o
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
[sys_exit] thread '/bin/as' exited with code 0x0000000000000000
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
make: built 'all'
[sys_exit] thread '/bin/make' exited with code 0x0000000000000000

m_random.o: ELF
m_bbox.o:   ELF
m_fixed.o:  ELF

PASS: 8
FAIL: 0
```

Every cc1 invocation exits 0 with no diagnostics printed.
No `fatal error`, no `implicit declaration`, no
`undefined reference`. cpp's `-isystem /bin` search path
found `<limits.h>`, `<stdlib.h>`, `<strings.h>`,
`<inttypes.h>` cleanly. The chapter-133b pattern
expansion synthesised three rule instances correctly. The
chapter-133a tar extraction landed files where expected.

Regression sweep held after this chapter:

| Test | Result |
|------|--------|
| `test_make_v2.py` | 9/0 |
| `test_tar.py` | 8/0 |
| `test_make_port.py` | 14/0 (chap-126 toy makefile still works) |
| `test_gcc_hello.py` | 10/0 |
| `test_gcc_stdio.py` | 7/0 |
| `test_gcc_bf.py` | 6/0 |
| `test_gcc_sys_stat.py` | 6/0 |

Plus `test_doom_pilot.py` 8/0 for this chapter (68/0 total
including 133c).

## What this unlocks

The fundamental loop works:

> tarball on disk → `/bin/tar xf` → `/bin/make -f` →
> `/bin/gcc -c` → ELF objects on disk

That's the same shape every real upstream build uses
(autotools, hand-written, doesn't matter — they all
eventually run `make` and `make` eventually runs `cc -c`).
With the pilot green the road opens to:

1. Scale OBJS from 3 to 82 files (chapter 133d), iterating
   on whatever libc gaps surface.
2. Add a link step (chapter 133e) to produce a binary —
   which requires shipping `libgui.a` on `/bin/` so the
   osdev shim links cleanly.
3. Stage the WAD (chapter 133f) so the binary has
   something to render.

Per the standing "apps must use the OS features the book
builds" rule:

- **`assets/osfs/doom_pilot.mk`** (new, ~45 LoC): the
  pilot fixture. Absolute-paths-everywhere as a workaround
  for spawn not propagating cwd.
- **`Makefile`** (edited): added `doom_pilot.mk` to
  `OSFS_FILES` and the mkosfs invocation.
- **`scripts/test_doom_pilot.py`** (new, ~260 LoC):
  end-to-end smoke test, 8 expectations.

No existing apps were modified — the chapter is purely
plumbing-validation. The user-visible app changes happen
in 133d–133e when an actual `doom` binary appears.

## What's next

Chapter 133d — scale to the full 82-file vendor compile.
Generate OBJS from `vendor/doomgeneric/src/*.c` minus the
13 host-backend variants
(`doomgeneric_{allegro,emscripten,linuxvt,sdl,soso,sosox,win,xlib}.c`,
plus `i_sdlsound`, `i_sdlmusic`, `i_allegrosound`,
`i_allegromusic`, `mus2mid`). Expect to iterate on:

- printf format gaps (`%g`, `%a`, `*`-width)
- any libc function doom uses that hasn't shipped yet
- the FILE\* abstraction from chapter 116b (stdio gaps)

Then 133e: link `doomgeneric.elf` in-guest. Requires
`/bin/libgui.a` first — that'll be its own sub-chapter.


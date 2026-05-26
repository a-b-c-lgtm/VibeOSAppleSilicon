# Chapter 133d — In-guest Doom rebuild, full vendor compile

> **Milestone in this chapter:** scale the chapter-133c pilot
> to the full 77-file DoomGeneric vendor compile, end-to-end
> inside the guest.
> **Code referenced:**
> - [assets/osfs/doom_full.mk](../../../assets/osfs/doom_full.mk)
> - [userspace/make/make.c](../../../userspace/make/make.c)
>   (`MK_MAX_VAL` 512 → 4096; expansion buffer 2 KiB → 16 KiB)
> - [scripts/test_doom_full.py](../../../scripts/test_doom_full.py)
>
> **At the end of this chapter** you will have the canonical
> 77-file DoomGeneric vendor source set compiling cleanly with
> zero compile or link errors, `test_doom_full.py` at
> **PASS 8 / FAIL 0**, and a ~25-minute wall time under HVF
> on Apple silicon. Prerequisites: chapter 133c (pilot),
> 133b (`/bin/make`), 133a (`/bin/tar`), 132g (`/bin/gcc`).

---

## What you'll do in this chapter

1. Write `assets/osfs/doom_full.mk` (~88 LoC): a
   `/bin/make` fixture with a 77-entry absolute-path OBJS
   list, `-DNORMALUNIX -I /data/src` CFLAGS to match the
   host vendor build, and a single `$(DIR)/%.o: $(DIR)/%.c`
   pattern rule that the chapter-133b engine fans out
   into 77 rule instances.
2. Bump `/bin/make`'s line-buffer ceilings
   (`MK_MAX_VAL` 512→4096, rule-header expansion buffer
   2 KiB→16 KiB, parser scratch widened) so the
   ~1500-character joined OBJS list isn't silently
   truncated.
3. Add `doom_full.mk` to `OSFS_FILES` and the mkosfs
   invocation so `/bin/doom_full.mk` lands at boot.
4. Write `scripts/test_doom_full.py` (~200 LoC) using
   *anchored* failure patterns (`make:` prefix, `cc1:`
   `fatal error`, `: error:`, `undefined reference`,
   `implicit declaration`) instead of bare substrings the
   kernel's `[sys_exit]` log line shares.
5. Confirm 68/0 across the 8-script regression sweep with
   `test_doom_full.py` 8/0 added.

---

## Why now

133c was three small files, picked to exercise the
toolchain plumbing. The bet was that if cc1 + as + xgcc +
make + tar + gcc could compose for `m_random.c`,
`m_bbox.c`, `m_fixed.c`, then they'd compose for the rest
of DoomGeneric too — modulo whatever libc gaps the larger
codebase happens to need.

This chapter cashes that bet. 77 vendor `.c` files,
compiled by the in-guest gcc, producing 77 ELF AArch64
objects on `/data/src/`. No host intervention after
`/bin/make -f /bin/doom_full.mk` starts. The only
diagnostic emitted across all 77 cc1 runs is a single
`-Wpointer-to-int-cast` warning in `p_maputl.c:849` (an
upstream quirk on 64-bit platforms, not a toolchain bug).

That's the biggest in-guest build the OS has ever run.
77 invocations of `/bin/gcc`, each spawning cc1 + as +
xgcc, is **231 user processes**. Every one of them gets
its own address space (chapter 75 CoW + chapter 73 fork),
its own argv/envp slab (chapter 116c), its own page table
walk through chapter 90's mmap-backed page cache, and
exits cleanly through chapter 78's SIGCHLD/waitpid path.
Nothing leaked, nothing wedged.

## The 77 files

DoomGeneric's `vendor/doomgeneric/src/` has 95 `.c` files.
The canonical compile set (per the host Makefile's
`SRC_DOOM` list) is 78. This chapter compiles 77 — every
one except `doomgeneric_xlib.c`, which is the X11 frontend
replaced by the osdev shim (deferred to 133e). The
sdl/allegro/soso/etc backend variants are also excluded,
matching the host build.

Game logic (`am_map`, `p_*`, `r_*`, `s_*`, `st_*`, `w_*`,
`wi_*`, `z_zone`) accounts for 50 files. Platform glue
(`d_*`, `f_*`, `g_*`, `hu_*`, `i_*`, `m_*`, `info`,
`memio`, `sha1`, `sounds`, `statdump`, `tables`, `v_video`,
`doomgeneric`) accounts for 27. Together: the entire
playable doom engine, minus only the screen+input backend.

## The fixture: `/bin/doom_full.mk`

Same shape as 133c's pilot, scaled up. The OBJS list is
77 absolute-path .o targets, joined across line
continuations:

```make
CC = /bin/gcc
CFLAGS = -O0 -DNORMALUNIX -I /data/src

DIR = /data/src

OBJS = $(DIR)/dummy.o \
       $(DIR)/am_map.o \
       $(DIR)/doomdef.o \
       $(DIR)/doomstat.o \
       ...
       $(DIR)/i_input.o \
       $(DIR)/i_video.o \
       $(DIR)/doomgeneric.o

all: $(OBJS)

$(DIR)/%.o: $(DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
```

`-DNORMALUNIX` gates the small amount of doomgeneric
platform code that prefers `unistd.h` over `windows.h`.
`-I /data/src` lets `#include "doomdef.h"` resolve within
the source dir, even though gcc's `-isystem /bin` already
catches `<stdio.h>` and friends.

The chapter-133b pattern-rule engine handles this without
any new machinery: `mk_split_pattern` splits the target on
`%` (`prefix="/data/src/"`, `suffix=".o"`), synthesises 77
rule instances at expand-time, and runs each through the
chapter-126 recipe executor. No special-casing for the
absolute paths.

## Pitfalls

### Pitfall — `/bin/make` buffer sizes from 133b are too small

**Symptom:** The build looked like it was succeeding — the
first ~25 .o files appeared on disk, no errors printed —
but most of the 77 files never had a recipe scheduled for
them. `/data/src/` ended up with only a partial set of
objects.

**Cause:** The 133b version of `/bin/make`
([`userspace/make/make.c`](../../../userspace/make/make.c))
had:

```c
#define MK_MAX_LINE   512
#define MK_MAX_VAL    512
```

`MK_MAX_LINE` is the per-line read buffer (one logical
line after continuation-join). `MK_MAX_VAL` is the
per-variable storage. Both 512 bytes.

DoomGeneric's OBJS list, after joining all `\`-continued
lines, is roughly **1500 characters** (77 paths of the form
`/data/src/foo.o ` averaging ~20 chars each, plus
whitespace). The 133b reader silently truncated the line
at the first 512-byte boundary, the parser stored the
first ~25 file names into `g_vars[OBJS].value`, and the
pattern-rule expansion only synthesised 25 of the 77
targets.

**Fix:** Three changes inside `make.c`:

```c
#define MK_MAX_VAL 4096          /* was 512 */
```

…plus widening the parser's local scratch buffers that
shadow MK_MAX_VAL:

```c
/* var-assign branch */
char tmp[MK_MAX_VAL];

/* rule-header branch (before $(VAR) expansion) */
char raw[MK_MAX_VAL];

/* rule-header expansion buffer (file scope, post-expand) */
static char header[16 * 1024];   /* was MK_MAX_LINE*4 = 2048 */
```

Recipe lines themselves stay at MK_MAX_LINE=512 — recipes
never come anywhere near that limit (the longest one in
DoomGeneric is ~85 chars: `/bin/gcc -O0 -DNORMALUNIX -I
/data/src -c /data/src/foo.c -o /data/src/foo.o`).

Stack impact: ~8 KiB extra inside `mk_parse` (called
once, no recursion). `mk_expand_into` uses its own
`tmp[MK_MAX_VAL]` per recursion level; max recursion
depth is bounded to 8 in `mk_expand_into`
([`userspace/make/make.c:194`](../../../userspace/make/make.c#L194)),
so 32 KiB peak. Fits inside `USER_STACK_PAGES=16`
(64 KiB, set in
[`kernel/core/elf.c:76`](../../../kernel/core/elf.c#L76)).

After the bump, every existing chapter-133b test still
passes (`test_make_v2.py` 9/0). The 133d full compile
runs cleanly.

### Pitfall — the kernel's `[sys_exit]` log line collides with failure-assertion substrings

**Symptom:** The first cut of `test_doom_full.py`'s step
3b said:

```python
expect(b"exited with code" not in out, ...)
```

…and failed every time. The captured serial transcript
shows why:

```
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
[sys_exit] thread '/bin/as' exited with code 0x0000000000000000
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
```

**Cause:** That line is the kernel's debug print, emitted
at every `sys_exit` regardless of success. With 231 child
processes in this test, that's 231 false-positive matches
for the literal substring `exited with code`.

Real `/bin/make` failures use the prefix `make:`:

- `make: recipe for 'foo.o' exited with code 1`
- `make: spawn '/bin/gcc' failed (errno=N)`
- `make: waitpid on '/bin/gcc' failed`

Real cc1 / xgcc errors use:

- `fatal error: <header>: No such file or directory`
- `: error: <message>` (note the leading `: ` — anchors
  the match away from `column N, error` etc.)
- `undefined reference to 'X'` (from /bin/ld)
- `implicit declaration of function 'X'`

**Fix:** Anchor the assertion on the emitting tool's
prefix instead of bare substrings:

```python
expect(b"fatal error" not in out
       and b"undefined reference" not in out
       and b"implicit declaration" not in out
       and b": error:" not in out
       and b"make: recipe for" not in out
       and b"make: spawn" not in out
       and b"make: waitpid" not in out,
       "step 3b: no compile/link errors during full build")
```

General rule worth burning into your fingers: when grepping
serial output from the OS for failure signals, anchor on
the *emitting tool's prefix* (`make:`, `cc1:`, `ld:`), not
on bare substrings the kernel's logs might also contain.

## Run it / Test it

`scripts/test_doom_full.py` is **PASS 8 / FAIL 0**, four
steps:

1. **Fixture sanity** (2)
   - `/bin/doom_full.mk` shipped on OSFS-1
   - `/bin/doomgeneric.tar` shipped on OSFS-1
2. **Tar extraction** (2)
   - `/bin/tar xf /bin/doomgeneric.tar -C /data` exits
     without `cannot create` / `errno=`
   - `/data/src/doomgeneric.c` exists post-extract
3. **Make run** (2)
   - `/bin/make` prints `make: built 'all'` and exits 0
   - No compile/link errors (using the anchored patterns
     above)
4. **Spot-check .o files** (1)
   - 11 representative .o files exist under `/data/src/`:
     `m_random.o`, `m_bbox.o`, `m_fixed.o`, `am_map.o`,
     `d_main.o`, `p_setup.o`, `r_main.o`, `r_draw.o`,
     `z_zone.o`, `doomgeneric.o`, `w_wad.o`

The test streams gcc output to stdout in real time so a
human watching the run can follow the per-file progress.
30-minute hard cap, 90-second idle bail-out (in case some
later cc1 hangs).

The test re-formats `/data/` via `mkosfs2.py` before every
run, so the tar extraction is always exercised end-to-end
— no carried-over .o files giving false greens.

Transcript (heavily abridged):

```
/bin/tar xf /bin/doomgeneric.tar -C /data
[sys_exit] thread '/bin/tar' exited with code 0x0000000000000000

--- launching /bin/make -f /bin/doom_full.mk ---
/bin/gcc -O0 -DNORMALUNIX -I /data/src -c /data/src/dummy.c       -o /data/src/dummy.o
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
[sys_exit] thread '/bin/as'  exited with code 0x0000000000000000
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
/bin/gcc -O0 -DNORMALUNIX -I /data/src -c /data/src/am_map.c      -o /data/src/am_map.o
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
...
/bin/gcc -O0 -DNORMALUNIX -I /data/src -c /data/src/p_maputl.c    -o /data/src/p_maputl.o
/data/src/p_maputl.c: In function 'InterceptsOverrun':
/data/src/p_maputl.c:849:43: warning: cast from pointer to integer of different size
  849 |     InterceptsMemoryOverrun(location + 8, (int) intercept->d.thing);
      |                                           ^
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
...
/bin/gcc -O0 -DNORMALUNIX -I /data/src -c /data/src/doomgeneric.c -o /data/src/doomgeneric.o
[sys_exit] thread '/bin/cc1' exited with code 0x0000000000000000
make: built 'all'
[sys_exit] thread '/bin/make' exited with code 0x0000000000000000

PASS: 8
FAIL: 0
```

One warning, no errors. 77 .o files written to disk.
Every spot-check target present.

Regression sweep held after this chapter (68/0):

| Test | Result | Note |
|------|--------|------|
| `test_make_v2.py` | 9/0 | validated post-buffer-bump |
| `test_tar.py` | 8/0 | |
| `test_make_port.py` | 14/0 | chap-126 toy makefile |
| `test_gcc_hello.py` | 10/0 | |
| `test_gcc_stdio.py` | 7/0 | |
| `test_gcc_bf.py` | 6/0 | |
| `test_gcc_sys_stat.py` | 6/0 | |
| `test_doom_pilot.py` | 8/0 | chap-133c |
| `test_doom_full.py` | 8/0 | **this chapter** |

## What this proves about the libc

DoomGeneric exercises a much wider libc surface than the
chapter-130c host-built doom ever did, because every `.c`
file is compiled fresh against the on-disk headers (no
precompiled host libc to fall back on). Across the 77
files:

- **stdio:** `printf`, `fprintf`, `sprintf`, `snprintf`,
  `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`,
  `fputs`, `fputc`, `fgetc`, `fgets`, `feof`, `ferror` —
  all shipped from chapters 116b + 128f.
- **string:** `memcpy`, `memset`, `memmove`, `memcmp`,
  `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`,
  `strncmp`, `strlen`, `strchr`, `strrchr`, `strstr`,
  `strdup`, `strndup`, `strerror` — all from 128c + 128e.
- **stdlib:** `malloc`, `free`, `calloc`, `realloc`,
  `atoi`, `atol`, `strtol`, `strtoul`, `abs`, `qsort`,
  `bsearch`, `rand`, `srand`, `exit`, `atexit`, `getenv` —
  from 17 + 128a + 128e.
- **sys/stat:** `stat`, `fstat`, `S_ISREG`, `S_ISDIR` —
  from 117 + 132j.
- **ctype:** `isdigit`, `isspace`, `isalpha`, `tolower`,
  `toupper` — from 128c.
- **setjmp:** `setjmp`, `longjmp` — from 128a.
- **time:** `time`, `gmtime`, `localtime`, `strftime` —
  from 128d.

The fact that no `implicit declaration` warning fires from
77 random upstream `.c` files is a real test of header
completeness, not just a smoke check.

## What this unlocks

The fundamental loop from 133c — tarball → tar → make →
gcc → ELF objects — generalises. Any reasonably-sized C
codebase that uses the on-disk libc surface (a couple
dozen tools in `userspace/` and most things from the BSD /
Doom era) should now build inside the guest without
further kernel or libc work. Concretely:

1. The remaining vendor work is just `/bin/ld` plumbing
   for the link step.
2. One chapter away from a real "Doom built inside
   the OS, plays inside the OS" demo.

Per the standing "apps must use the OS features the book
builds" rule:

- **`assets/osfs/doom_full.mk`** (new, ~88 LoC including
  the 77-file OBJS list): the full-compile fixture.
  Absolute paths everywhere (133c trap), `-DNORMALUNIX`
  and `-I /data/src` to match the host vendor CFLAGS.
- **`Makefile`** (edited): added
  `assets/osfs/doom_full.mk` to `OSFS_FILES`, and the
  `doom_full.mk=...` mapping to the `mkosfs` invocation.
- **`userspace/make/make.c`** (edited): bumped
  `MK_MAX_VAL` 512→4096, widened the parser's `tmp[]` and
  `raw[]` scratch buffers, widened the rule-header
  expansion buffer 2 KiB→16 KiB. No change to recipe-line
  width (still 512). Existing chapter-133b tests pass
  unchanged.
- **`scripts/test_doom_full.py`** (new, ~200 LoC):
  end-to-end smoke test, 8 expectations, ~25 min runtime.
  Uses anchored failure patterns (`make:` prefix +
  `: error:` etc.) to avoid the kernel-sys_exit-log trap.

No existing apps were modified — the chapter is still
plumbing-validation. The user-visible "doom binary on the
desktop" milestone lands in 133e–133f when the link
step produces a binary and the WAD ships.

## What's next

**Chapter 133e — link `doomgeneric.elf` in-guest.**
Known blockers:

1. **`libgui.a` on `/bin/`** —
   `userspace/doom/doomgeneric_osdev.c` references
   wmclient / window / font symbols. Need to bundle
   libgui into a static archive with `/bin/ar` (chapter
   119) and ship it.
2. **Ship the osdev shim source** on OSFS so it compiles
   to `doomgeneric_osdev.o` alongside the vendor files.
3. **`/bin/ld` capacity** — the chapter-119 in-guest
   linker has `MAX_INPUT_OBJS` and `MAX_SECTIONS` limits
   that were sized for tiny test programs. A 78-object
   link will likely exercise them. Plan: bump the same
   way `/bin/make` got bumped.

**Chapter 133f — rebuilt doom plays.** Re-run the
chapter-130c smoke test, but with the in-guest-built
binary instead of the cross-compiled one. Compare `.text`
sections (full md5 will differ — timestamps + build-id —
but symbol layout and code bytes should match modulo
register allocation choices).


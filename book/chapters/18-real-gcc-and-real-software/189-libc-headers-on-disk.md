# Chapter 189 — `#include <stdio.h>` works in the guest

> **Milestone in this chapter:** stage the libc and GCC
> freestanding headers on the disk so a C program can
> `#include <stdio.h>` instead of forward-declaring every
> symbol it uses.
> **Code referenced:**
> - [kernel/core/osfs.h](../../../kernel/core/osfs.h),
>   [scripts/mkosfs.py](../../../scripts/mkosfs.py)
>   (directory cap 128 → 256)
> - The `gccw` shim (`-isystem /bin` alongside `-B/bin/`)
> - [userspace/libc/](../../../userspace/libc/) (24 libc
>   headers staged onto `/bin`) and the 16 GCC freestanding
>   headers
> - [scripts/test_gcc_stdio.py](../../../scripts/test_gcc_stdio.py)
>
> **At the end of this chapter** you will have `/bin/gcc`
> resolving `#include <stdio.h>`, `<stdint.h>`, and the rest
> of the libc + GCC freestanding header set, and
> `test_gcc_stdio.py` at **PASS 7 / FAIL 0**. Prerequisite:
> chapter 188 (`/bin/gcc` builds a freestanding real
> program).

---

## What you'll do in this chapter

1. Bump the OSFS-1 directory cap from 128 to 256 entries
   (`kernel/core/osfs.h` and `scripts/mkosfs.py`) so the
   image has room for the header set.
2. Teach the `gccw` shim to pass `-isystem /bin` in addition
   to `-B/bin/`, because `-B` does not extend cpp's `<>`
   search path.
3. Stage 24 libc headers from `userspace/libc/` and 16 GCC
   freestanding headers from
   `build/gcc-build-guest/gcc/gcc/include/` onto `/bin/` via
   the existing mkosfs `name=path` pair mechanism.
4. Ship `assets/osfs/stdio_test.c` to `/bin/stdio_test.c`
   so the test never has to round-trip C source through
   the shell's heredoc parser.
5. Add `scripts/test_gcc_stdio.py` (cpp expansion, ELF
   produced, runtime output check) and confirm 7/7 with
   the chapter-187 and chapter-188 tests still green.

---

## Why now

Chapter 188 proved that `/bin/gcc bf.c -o /tmp/bf2` works
end-to-end. But `bf.c` cheated. It has zero `#include`
statements. Every symbol it uses — `open`, `read`, `write`,
`malloc`, `exit`, `memset`, `strlen` — is forward-declared
inline. Every typedef — `size_t`, `ssize_t`, `uint8_t`,
`off_t` — is duplicated inline. Every constant — `O_RDONLY` —
is hard-coded inline.

That's a fine way to write a 210-line interpreter under
tight control. It is not a fine way to write any of the
upstream software queued up next: `make`'s standard rules,
the GNU coreutils, the Doom port, eventually a self-hosted
GCC. All of those start with `#include <stdio.h>` on line 1.

The reason chapter 188 had to cheat is laid out in that
chapter's "header-shipping problem" section:

1. The in-guest `/bin/gcc` had no system include directory
   — the `gccw` shim only added `-B/bin/` to xgcc, which
   handles libraries and programs but **not** cpp's `<>`
   search path.
2. The 24 libc headers worth shipping total about 130 KB
   across 24 files. The OSFS-1 directory was capped at 128
   entries, of which 114 were already used. Five or ten
   headers could squeeze in, not twenty-four — and GCC's
   own freestanding headers (`stdint.h`, `stddef.h`,
   `stdarg.h`) add another sixteen files on top.

So chapter 188 punted to "freestanding-by-design" and left
the header problem for its own chapter. This is that
chapter.

---

## What you'll write

Three things changed.

### 1. OSFS-1 directory cap: 128 → 256

`kernel/core/osfs.h`:

```c
/* Bump chain: 32 (pre-M60) -> 64 (M60) -> 128 (chapter 110)
 * -> 256 (chapter 189, libc + GCC headers on /bin). */
#define OSFS_MAX_FILES         256
#define OSFS_DIR_SECTORS        16
#define OSFS_FIRST_DATA_SECTOR  17
```

`scripts/mkosfs.py` got the matching bumps. The kernel
re-parses the directory at mount-time so this is a
backwards-compatible read-side change — old 128-entry
images would still mount (the trailing 128 slots would
just be zero), but in practice the disk is rebuilt every
test run by `make build/disk.img`, so a mixed state never
occurs.

Disk-image growth: nine new sectors for the directory
itself (4.5 KB) plus header payloads. The total disk image
is still 256 MiB, of which 155 of 256 file slots are now
occupied (60 %). Plenty of room for chapters 133 and
beyond.

### 2. The gccw shim now passes `-isystem /bin`

`userspace/gccw/gccw.c` previously injected two arguments
into xgcc's argv: `-B/bin/`. That was enough for chapter
187 (libraries, programs, startfiles). It was not enough
for cpp.

The reason is a long-standing GCC behavior: `-B<prefix>`
extends the search path for **programs** and **libraries**,
but **not** for `<>`-included headers. cpp's system include
list is built from `--with-sysroot` at configure time, plus
`-isystem`, `-isysroot`, and a small list of compiled-in
defaults. None of those mention `-B`.

Half of this lesson was learned in chapter 187 when `-L`
turned out not to be implied by `-B` either. The full
generalisation is "B does not imply anything except B".
Header lookups need an explicit `-isystem`.

The new shim injects four arguments:

```c
/* Chapter 187: -B/bin/ tells xgcc to look in /bin for
 * crt0.o, libosdevc.a, and the linker script.  Chapter
 * 189: -isystem /bin extends cpp's `<>` search path to
 * /bin so user code can `#include <stdio.h>` etc.  -B
 * alone does NOT imply -isystem -- it covers libs and
 * programs only. */
new_argv[1] = "-B/bin/";
new_argv[2] = "-isystem";
new_argv[3] = "/bin";
```

`MAX_ARGS` was bumped from `argc + 2` to `argc + 4` to
make room.

### 3. Headers shipped on /bin

Two sets, totaling 40 files / ~220 KB.

**24 user libc headers** (from `userspace/libc/`):

```
assert.h   atexit.h   ctype.h    dirent.h   env.h
errno.h    fcntl.h    inttypes.h locale.h   malloc.h
math.h     printf.h   scanf.h    setjmp.h   signal.h
stdio.h    stdlib.h   string.h   strings.h  syscall.h
thread.h   time.h     wchar.h    zlib.h
```

These are the headers chapters 148–164 already maintain
under `userspace/libc/`. The Makefile reads them straight
from there and stages them onto the disk via mkosfs.

**Conspicuously absent: `unistd.h`.** The `unistd.h`
`#include`s `sys/stat.h` and `sys/types.h`, which OSFS-1
doesn't yet support because it has no subdirectories.
Building a `sys/` subdir on OSFS-1 is its own chapter
(190). Until then, in-guest code that wants `unistd.h`
either has to inline what it needs or wait one chapter.

**16 GCC freestanding headers** (from
`build/gcc-build-guest/gcc/gcc/include/`):

```
stdint.h     stdint-gcc.h  stddef.h     stdarg.h
stdbool.h    float.h       limits.h     iso646.h
stdalign.h   stdnoreturn.h syslimits.h  stdatomic.h
stdckdint.h  stdfix.h      tgmath.h     varargs.h
```

These are GCC's own. They come from GCC's source tree,
get installed alongside cc1 during the guest cross-build,
and mkosfs copies them onto /bin as part of `make
build/disk.img`. Skipping the seven `arm_*` headers (NEON,
SVE, SME, BF16, etc.) saved roughly 915 KB — they're
target-specific SIMD intrinsics that nothing on the build
list will use.

---

## Pitfalls

### Pitfall — cpp can't find `<stdint.h>` after shipping libc headers only

**Symptom:** The first cut of `scripts/test_gcc_stdio.py`
tried to exercise the world's simplest stdio program:

```c
#include <stdio.h>
int main(void) {
    printf("hello from %s, answer=%d\n", "in-guest gcc", 42);
    puts("puts works too");
    return 7;
}
```

The first run after shipping the 24 libc headers (but
before the GCC freestanding ones) failed with:

```
/bin/stdio.h:67:10: fatal error: stdint.h: No such file
                    or directory
```

**Cause:** The relevant block of `userspace/libc/stdio.h`:

```c
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "syscall.h"
#include "errno.h"
#include "malloc.h"
#include "printf.h"
```

The four `#include "..."` lines work — they're libosdevc
files, all shipped on /bin, all findable via `-isystem
/bin`. The three `#include <...>` lines blow up because
those headers are not user code. They are compiler-supplied
freestanding headers. On a normal Linux build host, they
live under `/usr/lib/gcc/<triple>/<version>/include/` and
the compiler finds them automatically via a path baked in
at GCC configure time. In this build, the relevant path
points at `…/build/toolchain/lib/gcc/aarch64-osdev/14.2.0/include/`
on the host, which doesn't exist in the guest.

GCC's `stdint.h` itself is a 328-byte forwarder:

```c
#ifndef _GCC_WRAP_STDINT_H
#if __STDC_HOSTED__
# /* ...hosted-mode preamble... */
# include_next <stdint.h>
#else
# include "stdint-gcc.h"
#endif
#define _GCC_WRAP_STDINT_H
#endif
```

Compiling with `-ffreestanding` makes the
`__STDC_HOSTED__` branch false, so the include falls
through to `stdint-gcc.h` — the ~9.6 KB "real" definitions
file. Both files have to ship.

**Fix:** Copy these GCC headers onto /bin alongside the
libc headers, and rely on the same `-isystem /bin` to
find them.

### Pitfall — the shell ate `<stdio.h>` in a heredoc

**Symptom:** The first version of `scripts/test_gcc_stdio.py`
tried to write the test source by typing into the guest
shell:

```python
shell_exec("cat > /tmp/stdio_test.c <<EOF\n"
           "#include <stdio.h>\n"
           "int main(void) { ... }\n"
           "EOF\n")
```

That broke spectacularly. The shell saw `<` and `>` in
`#include <stdio.h>` as redirections, even inside single
quotes. The actual on-disk content came out as something
like:

```
#include
int main(void) { ... }
```

…which is not valid C.

**Cause:** The in-guest shell's quoting story is not yet
POSIX-clean enough for nested heredocs containing `<` /
`>` characters; tokenising still happens inside quoted
heredoc bodies.

**Fix:** Follow the chapter 188 pattern — ship the source
on disk as a regular asset:

```
assets/osfs/stdio_test.c   ->   /bin/stdio_test.c
```

Then the test just does `/bin/gcc /bin/stdio_test.c -o
/tmp/stdio_test`. No shell quoting, no surprises.

**General rule:** never echo-stage C source through the
in-guest shell. Always ship via mkosfs. The chapter 188
"ship the source on disk" pattern, originally introduced
for `bf.c`, is now the standard shape.

---

## The Makefile shape

Two new variables and one extra line in the mkosfs
invocation:

```makefile
LIBC_HEADERS := assert.h atexit.h ctype.h dirent.h env.h \
                errno.h fcntl.h inttypes.h locale.h \
                malloc.h math.h printf.h scanf.h setjmp.h \
                signal.h stdio.h stdlib.h string.h \
                strings.h syscall.h thread.h time.h \
                wchar.h zlib.h

GCC_FREESTANDING_DIR := build/gcc-build-guest/gcc/gcc/include
GCC_FREESTANDING_HEADERS := stdint.h stdint-gcc.h stddef.h \
                            stdarg.h stdbool.h float.h \
                            limits.h iso646.h stdalign.h \
                            stdnoreturn.h syslimits.h \
                            stdatomic.h stdckdint.h stdfix.h \
                            tgmath.h varargs.h

$(DISK): scripts/mkosfs.py $(OSFS_FILES) $(OSFS_BIN_FILES) \
         $(BUILD)/userspace/crt/crt0.o userspace/linker_user.ld \
         $(XGCC_SYS_LIBC) \
         $(addprefix userspace/libc/,$(LIBC_HEADERS)) \
         $(addprefix $(GCC_FREESTANDING_DIR)/,$(GCC_FREESTANDING_HEADERS))
	python3 scripts/mkosfs.py $(DISK) \
	    $(foreach h,$(LIBC_HEADERS),$(h)=userspace/libc/$(h)) \
	    $(foreach h,$(GCC_FREESTANDING_HEADERS),$(h)=$(GCC_FREESTANDING_DIR)/$(h)) \
	    ...
```

The `foreach` pattern matters: mkosfs.py takes
`name=path` pairs, so the same source file path can be
re-pointed to a different on-disk name. Here every header
keeps its native name (`stdio.h`, `stdint.h`, …) — the
flat namespace on OSFS-1 is a feature, not a constraint,
because GCC's `<>` lookup is a single-directory linear
search anyway when there's no subdir prefix involved.

The dependency expression makes sure the disk image is
rebuilt if any source header changes — including upstream
edits to `userspace/libc/*.h`, which has been a source of
stale-image bugs in chapters 160 and 128.

---

## Run it / Test it

`scripts/test_gcc_stdio.py` is a three-step ladder:

```
[chapter 189] /bin/gcc resolves #include <stdio.h>
PASS: step 1: cpp expands <stdio.h> (printf visible)
PASS: step 1: no missing-header errors
PASS: step 2: in-guest gcc produced /tmp/stdio_test ELF
PASS: step 2: cc1 + as + ld all exit 0
PASS: step 3: printf format string + %s + %d resolves correctly
PASS: step 3: puts() works
PASS: step 3: main returned 7 via stdio.h-compiled program

PASS: 7
FAIL: 0
```

Each step exists to fence in a class of regression:

- **Step 1** (`-E`) catches the diagnosed bug class:
  cpp can't find a header. If `<stdint.h>` or
  `<stddef.h>` get dropped from the disk image again,
  step 1 fails first with a precise filename.
- **Step 2** (`-o`) catches the next-tier bugs: any
  header expands cleanly to declarations cc1 doesn't
  like, or pulls in an inline symbol that's not in
  libosdevc.a, or otherwise breaks link.
- **Step 3** (run + check) catches the silent-correctness
  bug: cpp finds the header but it expands to the wrong
  symbol, so `printf` is calling some stub or some
  recursive stack-blower.

Regression: `test_gcc_hello.py` (chapter 187) still PASS
10/10. `test_gcc_bf.py` (chapter 188) still PASS 6/6.
The cap bump + extra headers + extra shim arg didn't
disturb either.

---

## What's deferred

- **`#include <unistd.h>`** — the header itself is on disk
  in `userspace/libc/`, but it `#include`s `<sys/stat.h>`
  and `<sys/types.h>`. OSFS-1 has no subdirectory support
  yet. Chapter 190 adds a single-level subdir to OSFS-1
  and then ships `sys/`.
- **`#include <complex.h>` / `<fenv.h>`** — not shipped.
  Add on demand when a port asks for them.
- **`#include <pthread.h>`** — `thread.h` (a shrunk
  POSIX-ish API) is on disk; the full pthreads header is
  not. Same story: add when a port asks.
- **`#include <stdio.h>` in C++** — cc1plus is not on
  disk; C++ in the guest is a future chapter.
- **`fopen` / `fread` / `printf` from libc, not header-
  inlined** — printf is `static inline` in `printf.h`,
  so today it's per-translation-unit. That's fine for
  hello-world but bad for a program with twelve .c files
  that all use printf. Chapter 133 may need to break
  some of these out into real symbols in libosdevc.a.
- **Header-include path collisions** — what happens if a
  port ships its own `stdint.h`? Today `-isystem /bin`
  is searched after `-I.` so the local one wins. An
  adversarial case has not been tested.

---

## What this unlocks

The thing this chapter actually validates is that a user
can write idiomatic ISO C in the guest. `#include
<stdio.h>`, `printf("…\n")`, return — exactly the C every
intro-to-programming book uses. The toolchain finally
matches its users' expectations.

What it does NOT validate:

- That every header works (only stdio.h is exercised end-
  to-end; the other 23 are present but only their syntactic
  cleanliness is implicitly tested via cpp expansion).
- That a multi-file project links cleanly (chapter 133).
- That `make` finds and respects the headers in /bin
  (chapter 162 ported a make; integration left to chapter
  133).
- That porting an upstream package works (chapter 133).
- That self-hosting GCC works (long way off).

Per the standing "apps must use the OS features the book
builds" rule:

- **Existing app modified:** `/bin/gcc` (gccw.c) now
  passes `-isystem /bin` so cpp finds headers.
- **Kernel change:** `kernel/core/osfs.h` doubled the
  directory cap (128 → 256). `scripts/mkosfs.py`
  mirrored.
- **New on-disk content:**
  - 24 libc headers from `userspace/libc/*.h` shipped as
    `/bin/<name>.h`.
  - 16 GCC freestanding headers from
    `build/gcc-build-guest/gcc/gcc/include/` shipped as
    `/bin/<name>.h`.
  - `assets/osfs/stdio_test.c` shipped as
    `/bin/stdio_test.c`.
- **New test:** `scripts/test_gcc_stdio.py`. PASS 7/7.
- **Existing tests still green:**
  - `test_gcc_hello.py` (chapter 187) — PASS 10/10
  - `test_gcc_bf.py` (chapter 188) — PASS 6/6

The "ship the source alongside the binary" pattern from
chapter 188 now extends to "ship the headers alongside
the compiler." Together they're the on-disk substrate
the rest of part 18 — real upstream ports — will stand
on.

---

## What's next

- **Chapter 190** builds sys/-aware OSFS-1 and ships
  `unistd.h` cleanly, finishing the libc shipping story.
- **Chapter 133** attempts a real upstream port —
  something like `tar`, `gzip`, or a tiny TUI app —
  because the headers are finally in place.
- **Chapter 172** (doom port) gets a clearer path:
  shipping `doomgeneric.c` on disk and rebuilding it
  in-guest will need exactly the header-shipping
  infrastructure this chapter built. With `<stdio.h>`
  resolved, the next missing piece is `<math.h>` working
  end-to-end (probably needs more libgcc soft-float
  routines exposed; future chapter).


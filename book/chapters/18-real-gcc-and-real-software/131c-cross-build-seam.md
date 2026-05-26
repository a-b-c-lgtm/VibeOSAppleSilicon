# Chapter 131c — Cross-build seam: link-mode wrapper, first libc gaps

> **Milestone in this chapter:** extend the chapter-131b wrapper
> into link mode, run upstream binutils' configure under
> `--host=aarch64-osdev`, and catalog the first wave of libc
> gaps that surface.
> **Code referenced:**
> - [scripts/aarch64-osdev-cc.in](../../../scripts/aarch64-osdev-cc.in)
>   (link-mode auto-injection)
> - [vendor/binutils-2.44/libiberty/](../../../vendor/binutils-2.44/libiberty/)
> - [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py)
>
> **At the end of this chapter** you will have a wrapper that
> auto-injects `build/userspace/crt/crt0.o` on link, a clean
> upstream `binutils-2.44` configure under
> `--host=aarch64-osdev`, the first wave of `libiberty/*.c`
> objects built against the OS libc, and a written catalog of
> the missing functions (`vfprintf` collision, `mktemp`,
> `freopen`, `_doprnt`, `sleep`, `st_dev`, open `mode` arg)
> that chapter 131d will close. Prerequisite: chapter 131b
> (wrapper installed). Run `make` first so
> `build/userspace/crt/crt0.o` exists.

---

## What you'll do in this chapter

1. Teach the chapter 131b wrapper a **link mode** that
   auto-injects `crt0.o` and `-T linker_user.ld` when the
   caller (autoconf) doesn't supply them, plus
   `-ffreestanding` unconditionally.
2. Drive `binutils-2.44`'s top-level `configure` at
   `--host=aarch64-osdev` and close the trivial libc gaps
   the first build pass surfaces (`abort` reachable from
   `<stdlib.h>`, `sprintf` / `vsprintf`).
3. Run `make -k all-libiberty` to flush out every remaining
   gap in one shot and classify them into the four
   work-streams chapter 131d will eat.
4. Write [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py)
   — a host smoke test that asserts the configure stage
   completes and five known-good libiberty `.o` files
   build cleanly.

## What this chapter actually does

Three things, in order of significance:

1. Teach the chapter 131b wrapper a **link mode** that
   auto-injects `crt0.o` and `-T linker_user.ld` when the
   caller (autoconf) doesn't supply them, plus
   `-ffreestanding` always so gcc's internal `stdint.h`
   stops trying to `#include_next` to a newlib that isn't
   there.
2. Drive `binutils-2.44`'s top-level `configure` at
   `--host=aarch64-osdev`, fix the trivial libc gaps the
   first build pass surfaces (`abort` reachable from
   `<stdlib.h>`, `sprintf` / `vsprintf`), and verify the
   conftest pipeline reaches the libiberty subdir.
3. Catalogue the remaining libc gaps (collected via
   `make -k all-libiberty` to get all errors in one
   shot) and classify them into work-streams for chapter
   131d to chew through.

The wire-up isn't sexy but it's the chapter that turns
"the host can configure binutils" (131a) and "the target
compiler stand-in exists" (131b) into "the cross-build is
actually running and the remaining gaps are known."

---

## Why a link-mode wrapper at all

Chapter 131b's wrapper was a transparent passthrough that
respected whatever flags the caller passed. That works for
hand-written builds (the project Makefile, which passes
`crt0.o` and `-T linker_user.ld` explicitly), but it dies
the moment autoconf shows up.

Autoconf's first interesting move is a link test:

```
configure:3479: aarch64-osdev-cc -o conftest -ffreestanding \
                                 ... conftest.c
```

with `conftest.c` being a six-line `int main(){return 0;}`.
That fails:

```
.../aarch64-elf-ld: warning: cannot find entry symbol _user_start;
                              defaulting to 0000001000100000
.../aarch64-elf-ld: conftest.c:(.text+0x20): undefined reference to
                                              `__errno_value'
collect2: error: ld returned 1 exit status
```

`__errno_value` is the `extern int` the OSdev libc's
[`errno.h`](../../../userspace/libc/errno.h) declares; it's
defined once in [`crt0.S`](../../../userspace/crt/crt0.S).
`_user_start` is also crt0. Autoconf treats *one* failed link
test as proof the compiler can't produce executables, sets
the internal `GCC_NO_EXECUTABLES` flag, and refuses every
subsequent link test in the build with:

```
configure: error: Link tests are not allowed after
                  GCC_NO_EXECUTABLES.
```

So the **first** thing chapter 131c has to do is give
autoconf a link line that succeeds with no extra flags.

### How the wrapper detects link mode

[scripts/aarch64-osdev-cc.in](../../../scripts/aarch64-osdev-cc.in)
walks `"$@"` twice. Pass one decides whether the
invocation is compiling or linking:

```sh
mode=link
for arg in "$@"; do
    case "$arg" in
        -c|-S|-E|-M|-MM|-MD|-MMD) mode=compile; break ;;
        -r|-Ur)                   mode=compile; break ;;
    esac
done
```

`-c` / `-S` / `-E` are the standard "stop before linking"
flags. `-M*` are dependency-generation modes that produce
no executable. `-r` is a partial / relocatable link — it
*does* link, but the output has no entry point and no crt
involvement, so injecting crt0 would corrupt it.

Pass two (only in link mode) decides whether the caller has
already supplied a `crt0` and a linker script:

```sh
for arg in "$@"; do
    case "$arg" in
        *crt0.o|*crt0.S|*crt0.s|*crt0.c)
            needs_crt0=false ;;
        -T|-T*|-Wl,-T,*|-Wl,-T*)
            needs_script=false ;;
    esac
done
```

The patterns cover both flag forms ld and gcc accept. The
`*crt0.X` match is conservative; the project's Makefile
always names it `crt0.o` and feeds it as a positional
argument, so it gets matched cleanly. If a future caller
hands ld a `my_custom_start.o` instead, the wrapper will
double up — that's a "live with the warning" case until
someone hits it.

When both flags pass, the wrapper appends:

```sh
extra_post="$extra_post $OSDEV_ROOT/build/userspace/crt/crt0.o
            -Wl,-T,$OSDEV_ROOT/userspace/linker_user.ld"
extra_pre="-nostdlib -nostartfiles"
```

`crt0.o` goes *after* the user's objects so the
`__errno_value` and `_user_start` symbols get resolved
last. The `-nostdlib -nostartfiles` go *before* the user's
flags so the Makefile's identical flags (chapter 50+) still
win the duplicate-arg fight cleanly.

### Why `-ffreestanding` unconditionally

Brew's `aarch64-elf-gcc` was built with `--without-headers`,
so its bundled libc is empty. Its internal `stdint.h` is
a wrapper that does:

```c
#ifndef _GCC_WRAP_STDINT_H
#if __STDC_HOSTED__
# include_next <stdint.h>      /* would find newlib's, but there is none */
#else
# include "stdint-gcc.h"        /* self-contained fallback */
#endif
```

`-ffreestanding` defines `__STDC_HOSTED__=0` and switches
the gcc-internal `stdint.h` to its self-contained
[`stdint-gcc.h`](file:///opt/homebrew/Cellar/aarch64-elf-gcc/14.2.0/lib/gcc/aarch64-elf/14.2.0/include/stdint-gcc.h).
Without `-ffreestanding` every conftest that pulls in
`<stdint.h>` dies at the `#include_next`.

The Makefile already passes `-ffreestanding` everywhere,
so it never noticed. Autoconf doesn't. The wrapper passes
it unconditionally now — idempotent for callers that
already do.

### Byte-identity test still passes

The chapter 131b smoke test
[scripts/test_aarch64_osdev_cc.py](../../../scripts/test_aarch64_osdev_cc.py)
still asserts that the wrapper-built `hello.stripped.elf`
is byte-identical to the Makefile-built one. It still
passes (7864 bytes) because the Makefile supplies both
`crt0.o` and `-Wl,-T,...` explicitly, so the wrapper's
auto-inject logic correctly skips both. The extra
`-ffreestanding` is a no-op next to the Makefile's own
`-ffreestanding`.

---

## The conftest probe

With the link mode in place, `binutils-2.44`'s top-level
`configure` now runs end-to-end at
`--host=aarch64-osdev --target=aarch64-osdev`. The full
invocation (matched in
[scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py))
is:

```sh
PATH=build/toolchain/bin:$PATH \
CC=aarch64-osdev-cc CFLAGS="-mcpu=cortex-a72" LDFLAGS="" \
vendor/binutils-2.44/configure \
    --host=aarch64-osdev --target=aarch64-osdev \
    --prefix=build/toolchain-guest \
    --disable-nls --disable-gdb --disable-werror \
    --disable-multilib --with-system-zlib \
    --disable-binutils --disable-ld --disable-gprof \
    --disable-gprofng --disable-libdecnumber \
    --disable-readline --disable-sim \
    --disable-libquadmath --disable-libquadmath-support \
    --disable-shared
```

The `--disable-binutils --disable-ld` pair tells the
top-level driver to only build gas (and its prereqs:
libiberty, libbfd, libopcodes). This chapter isn't trying
to bring up the full toolchain in-guest in one shot; gas
alone is the smallest binary that proves the seam works.

`--with-system-zlib` is required for the same macOS C23
clang reason chapter 131a hit (the bundled zlib has K&R
prototypes that clang now rejects).

Configure succeeds and produces `Makefile`s in each
subdir. `make configure-libiberty` then runs
`libiberty/configure`, which now does ~200 link tests and
all of them succeed — no `GCC_NO_EXECUTABLES`.

---

## First-wave libc fixes

Two trivial gaps surface and get closed in this chapter:

### 1. `abort` reachable from `<stdlib.h>`

C99 §7.20.4.1 says `<stdlib.h>` declares `abort`. `abort`
is defined as `static inline` in
[`userspace/libc/signal.h`](../../../userspace/libc/signal.h)
(chapter 128b), but `<stdlib.h>` doesn't include
`<signal.h>`, so `libiberty/regex.c`'s `abort();` in the
"all cases listed" path gets
`implicit declaration of function 'abort'`.

Fix:
[userspace/libc/stdlib.h](../../../userspace/libc/stdlib.h)
now cascades `#include "signal.h"`. The comment in
`stdlib.h` already documented the cascade pattern for
`env.h` / `malloc.h` / `atexit.h` / `string.h`; this
just extends it.

### 2. `sprintf` / `vsprintf`

C99 §7.19.6.5 / 7.19.6.13. `snprintf` and `vsnprintf` have
been in
[`userspace/libc/printf.h`](../../../userspace/libc/printf.h)
since chapter 19, but the unbounded variants were never
added because no in-tree code uses them.
`libiberty/cplus-dem.c`'s Ada demangler uses `sprintf`
extensively.

Fix: trivial two-liners on top of `vsnprintf` with
`cap = (size_t)-1`. The C99 semantics ("the buffer is
big enough, trust me") are intentionally unsafe; that's
the spec.

After these two: `libiberty/regex.c`, `libiberty/argv.c`,
`libiberty/alloca.c`, `libiberty/bsearch_r.c`, and
`libiberty/cplus-dem.c` all compile cleanly. The smoke
test asserts this; see "Run it / Test it" below.

---

## The libc-gap catalog (what chapter 131d will eat)

`make -k all-libiberty 2>&1 | grep error: | sort -u` after
the first-wave fixes gives a clean per-failure list.
Classify them so 131d can plan rather than improvise:

### Class A — `static inline` vs autoconf REPLACE_FUNCS

```
redefinition of 'vfprintf'        (libiberty/vfprintf.c)
redefinition of 'vprintf'         (libiberty/vprintf.c)
redefinition of 'vsnprintf'       (libiberty/vsnprintf.c)
redefinition of 'getopt'          (libiberty/getopt.c)
redefinition of 'optarg' etc.     (libiberty/getopt.c)
conflicting types for 'strerror'  (libiberty/strerror.c)
conflicting types for 'gettimeofday' (libiberty/gettimeofday.c)
conflicting types for 'getcwd'    (libiberty/getcwd.c)
```

Root cause: the OSdev libc functions are `static inline` in
headers. Autoconf's `AC_CHECK_FUNCS` link test looks for an
*externally linked symbol* — link tests don't `#include`
the header, they just write `char vfprintf();` and try to
link. Static inlines aren't externally linked, so autoconf
concludes "no vfprintf" and tells libiberty's build to
compile its replacement, which then collides with the
header-declared inline at compile time when the
replacement source `#include`s `<stdio.h>`.

The Right Fix™ is to extract the libc into a real
`libc.a` archive of `.o` files built from `.c` translation
units, so the symbols are externally visible and autoconf
finds them. That's chapter 131d's main body. It also
solves the multi-TU state problem noted in chapter 131b
(`_io_open_head` in `stdio.h` becomes a single global
rather than per-TU).

### Class B — missing libc functions

```
implicit declaration of function 'mktemp'    (choose-temp.c)
implicit declaration of function 'freopen'   (fopen_unlocked.c)
implicit declaration of function 'ldexp'     (floatformat.c)
implicit declaration of function 'frexp'     (floatformat.c)
implicit declaration of function '_doprnt'   (vfprintf.c)
implicit declaration of function 'sleep'     (pex-unix.c)
implicit declaration of function 'execvp'    (pex-unix.c)
implicit declaration of function '_exit'     (pex-unix.c)
implicit declaration of function 'link'      (rename.c)
implicit declaration of function 'wait'      (waitpid.c)
```

These are all genuine libc-surface gaps. None of them are
deep:

- `mktemp` is `mkstemp` minus the file open — generate
  a unique name, return it. The libc has `mkstemp` via
  `<stdlib.h>` somewhere? (check during 131d).
- `freopen` is `fclose(f); *f = *fopen(path, mode); return f;`
  except it has to mutate the FILE \* in place. `fopen` /
  `fclose` already exist.
- `ldexp`/`frexp` are `<math.h>` and need FP at EL0
  (chapter 129). 131d papers over them with stubs that
  abort, since the only caller is the Itanium / m68k
  floatformat code which gas-aarch64 doesn't reach.
- `_doprnt` is BSD's pre-`<stdarg.h>` printf engine. The
  config cache can just declare
  `#define HAVE_DOPRNT 0 #define HAVE_VFPRINTF 1`.
- `sleep`, `_exit`, `wait`, `link`, `execvp` are
  thin wrappers over syscalls that already exist.

### Class C — wrong struct shape

```
'struct stat' has no member named 'st_dev'   (fdmatch.c, getpwd.c)
'struct stat' has no member named 'st_ino'   (fdmatch.c, getpwd.c)
```

Our `stat` struct
([userspace/libc/sys/stat.h](../../../userspace/libc/sys/stat.h)
chapter 117) was shaped for the OSdev filesystem layout
which has no device numbers (one disk) and inode numbers
that aren't exposed to userspace yet. POSIX-shape `st_dev`
and `st_ino` need to be added; the kernel side of stat
needs to populate them.

### Class D — wrong signature

```
too many arguments to function 'open'   (mkstemps.c, pex-unix.c, simple-object.c)
```

POSIX `open()` is variadic — `open(path, flags)` or
`open(path, flags, mode)` depending on whether `flags`
includes `O_CREAT`.
[`userspace/libc/fcntl.h`](../../../userspace/libc/fcntl.h)
declares it as `int open(const char *, int)`. 131d moves
it to the variadic form.

### Summary

| Class | Count | Difficulty |
|-------|-------|------------|
| A (static inline vs replace) | 8 | Hard — needs libc.a refactor |
| B (missing functions) | 10 | Easy — straight wrappers, except ldexp/frexp which wait for chapter 129 |
| C (struct stat fields) | 2 fields | Medium — needs kernel-side stat changes too |
| D (open variadic) | 1 signature | Easy — fcntl.h tweak |

Total distinct failures: ~30. Most fall away once Class A
is fixed (the replacement files stop being compiled).

---

## What 131d will need to do

Order matters. Approximate plan:

1. **`open` variadic.** Trivial; unblocks `mkstemps.c`,
   `pex-unix.c`, `simple-object.c`.
2. **`struct stat` fields.** Add `st_dev`, `st_ino` to
   the user-side `struct stat` AND make the kernel
   `getdents` / `fstat` syscall populate them with
   plausible values (device 0, inode = block number or
   similar).
3. **Class B function stubs.** Wrap the underlying
   syscalls; `ldexp`/`frexp` get `abort()` stubs with a
   comment pointing at chapter 129.
4. **Class A libc.a refactor — the big one.** Move every
   `static inline` function out of the headers into
   per-function `.c` files compiled into
   `build/userspace/libc/libc.a`. Headers retain the
   declarations. The wrapper learns to link `-l:libc.a`
   from `build/userspace/libc/` automatically in link
   mode (similar to how the chapter 131c crt0
   auto-inject works). Every existing app in the tree
   gets re-linked against `libc.a` to prove the
   refactor preserves behaviour; the chapter 131b
   byte-identity smoke test gets a new baseline
   recorded.

After 131d, libiberty should fully build. After that,
`libbfd` + `libopcodes` + `gas` (which sit on top of
libiberty) will each surface their own gap catalogues
that future chapters work through the same way.

---

## What this unlocks

- [scripts/aarch64-osdev-cc.in](../../../scripts/aarch64-osdev-cc.in)
  — link-mode detection and auto-inject; `-ffreestanding`
  is unconditional; expanded comment block documenting
  rationale.
- [userspace/libc/stdlib.h](../../../userspace/libc/stdlib.h)
  — added `#include "signal.h"` to the C99-completeness
  cascade so `abort` is reachable from `<stdlib.h>`.
- [userspace/libc/printf.h](../../../userspace/libc/printf.h)
  — added `sprintf` / `vsprintf` as thin wrappers around
  `vsnprintf` with unbounded capacity.
- [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py)
  — new host smoke test (kept per
  `/memories/debug-scripts-policy.md`).
- `build/binutils-build-guest/` is auto-created on demand
  by the smoke test; covered by the existing `build/*`
  `.gitignore` rule.

No existing apps changed — chapter 131c is host-tool
plumbing. The
[apps-must-use-features](/memories/apps-must-use-features.md)
rule applies when chapter 131d's libc.a refactor lands
(every app gets re-linked) and when chapter 131e replaces
`/bin/as` with the real gas.

## Run it / Test it

- New: [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py).
  Host-side smoke test, NOT in `scripts/sweep.sh`. Asserts:
  1. top-level binutils configure runs to completion
     under `--host=aarch64-osdev`;
  2. `make configure-libiberty` succeeds (no
     `GCC_NO_EXECUTABLES`);
  3. five known-good libiberty `.o` files build cleanly:
     `alloca.o`, `argv.o`, `bsearch_r.o`,
     `cplus-dem.o` (exercises chapter 131c's `sprintf`),
     `regex.o` (exercises chapter 131c's
     `abort`-via-`stdlib.h` cascade).
- Unchanged: chapter 131b's
  [scripts/test_aarch64_osdev_cc.py](../../../scripts/test_aarch64_osdev_cc.py)
  still passes (byte-identical `hello.stripped.elf`, 7864
  bytes). The wrapper's link-mode auto-inject correctly
  skips when crt0 and `-T` are supplied explicitly.
- Unchanged: rest of `scripts/sweep.sh`. This chapter
  ships no guest-side code.

## What's next

Chapter 131d eats the gap catalog above: refactor the libc
into a real `libc.a`, add the missing functions, fix the
`struct stat` shape, and make `open` variadic.

# Chapter 176 — `aarch64-osdev-cc`: the target-compiler seam

> **Milestone in this chapter:** install a 30-line shell wrapper
> that presents the chapter-175 binutils as a single
> `aarch64-osdev-cc` driver — the seam later chapters replace
> with a real `aarch64-osdev-gcc`.
> **Code referenced:**
> - [scripts/aarch64-osdev-cc.in](../../../scripts/aarch64-osdev-cc.in)
> - [Makefile](../../../Makefile)
>   (`make aarch64-osdev-cc-install`)
> - [scripts/test_aarch64_osdev_cc.py](../../../scripts/test_aarch64_osdev_cc.py)
>
> **At the end of this chapter** you will have
> `build/toolchain/bin/aarch64-osdev-cc` (plus `as` and `ld`
> symlinks) installed and a host smoke test that builds
> `hello.stripped.elf` byte-identical to the Makefile build,
> with `aarch64-osdev-ld` independently linking the same
> `crt0.o + hello.o` into the same bytes. Prerequisite:
> chapter 175.

---

## What you'll do in this chapter

1. Write [scripts/aarch64-osdev-cc.in](../../../scripts/aarch64-osdev-cc.in)
   — a 30-line shell script template that wraps
   `aarch64-elf-gcc` with `-B` and `-isystem` pointing at
   the chapter 175 toolchain and the OSdev libc.
2. Add `make aarch64-osdev-cc-install` to install the
   wrapper into `build/toolchain/bin/` along with
   `as` / `ld` symlinks.
3. Write [scripts/test_aarch64_osdev_cc.py](../../../scripts/test_aarch64_osdev_cc.py)
   — a two-half host smoke test asserting the wrapper and
   `aarch64-osdev-ld` both produce a byte-identical
   `hello.stripped.elf`.

## The bootstrap problem this chapter solves

Phase 2's punchline is "the OS can compile programs that
run on it". The path there is:

1. Host binutils with the OSdev triple (**175, done**).
2. Build real binutils gas for the guest, using a
   compiler that links against the OSdev libc and produces
   ELF binaries that run on the OSdev kernel (**177**).
3. Run that gas inside the guest, iterate on libc
   gaps surfaced by gas's runtime calls (**177**).
4. Same for ld (**177**).
5. Replace `/bin/as` and `/bin/ld` with the result
   (**178**).
6. Bootstrap a real `aarch64-osdev-gcc` (**181 onwards**).

Step 2 needs a compiler whose triple is `aarch64-osdev`,
whose system headers are the OSdev libc, and whose binutils
are the ones built in 175. That compiler doesn't exist
yet — it's the chapter 181+ job. Re-ordering Part XVIII to land
GCC first is a worse trade (GCC is far harder than gas, and
the whole point of doing gas first is to shake out libc
gaps with a smaller program), so chapter 177 needs a
stand-in. That stand-in is this chapter's wrapper.

The stand-in is a 30-line shell script.

---

## What the wrapper does

[scripts/aarch64-osdev-cc.in](../../../scripts/aarch64-osdev-cc.in)
is a template; `make aarch64-osdev-cc-install` substitutes
`@OSDEV_ROOT@` with the absolute path to the checkout
and drops the result at
[build/toolchain/bin/aarch64-osdev-cc](../../../build/toolchain/bin/aarch64-osdev-cc).

Stripped of comments, the body is:

```sh
exec aarch64-elf-gcc \
    -B "$OSDEV_ROOT/build/toolchain/bin/" \
    -isystem "$OSDEV_ROOT/userspace/libc" \
    "$@"
```

Two overrides on top of the host's
`aarch64-elf-gcc`:

**`-B build/toolchain/bin/`** adds the chapter 175
toolchain dir to gcc's program-and-library search path.
The install step also drops two symlinks in that
directory:

- `build/toolchain/bin/as → aarch64-osdev-as`
- `build/toolchain/bin/ld → aarch64-osdev-ld`

The intent is "gcc front-end, OSdev binutils backend." In
practice that doesn't happen yet — see the caveat below.

**`-isystem $ROOT/userspace/libc`** puts the header-only
libc (chapters 148–164) ahead of newlib in the
system-header search order. The wrapper deliberately does
*not* pass `-nostdinc`:

- gcc's internal headers (`stdint.h`, `stdarg.h`,
  `stddef.h`, `float.h`) need to stay reachable, or
  every C source breaks immediately.
- Bypassing newlib for everything else is the **point**
  of chapter 177 — let gas's autoconf surface
  missing-header gaps so they can be filled in the
  OSdev libc with a concrete test case driving each
  decision. Doing it now, without that pressure, would
  mean inventing libc API in a vacuum.

Everything else (`-c` vs link, `-O`, `-g`, `-Wl,...`,
`-T script`) is passed through unchanged. The wrapper
deliberately does NOT auto-inject `crt0.o` or
`-T linker_user.ld` at link time — the caller still owns
those. Predictability over magic.

---

## The `-B` caveat (and why it's still worth shipping)

`aarch64-elf-gcc` on Homebrew was configured with:

```
--with-as=/opt/homebrew/opt/aarch64-elf-binutils/bin/aarch64-elf-as
--with-ld=/opt/homebrew/opt/aarch64-elf-binutils/bin/aarch64-elf-ld
```

Those absolute paths are hardcoded into the gcc spec and
take precedence over `-B` prefixes at runtime. Verify
directly:

```
$ build/toolchain/bin/aarch64-osdev-cc -print-prog-name=ld
/opt/homebrew/opt/aarch64-elf-binutils/bin/aarch64-elf-ld
$ build/toolchain/bin/aarch64-osdev-cc -print-prog-name=as
/opt/homebrew/opt/aarch64-elf-binutils/bin/aarch64-elf-as
```

So today, when you compile via the wrapper, the actual
assembler and linker that get invoked are Homebrew's
`aarch64-elf-as` and `aarch64-elf-ld`. The wrapper's
`-B` prefix and the `as`/`ld` symlinks are not actually
on the hot path.

This is fine for now, and the smoke test proves it:
both Homebrew's binutils and the chapter 175 binutils
are binutils-2.44 built from the same source (only the
target-triple plumbing was patched, not the
assembler/linker engines). For ELF64-LE aarch64 output,
they produce byte-identical results.

The wrapper still installs the symlinks because chapter
183's real `aarch64-osdev-gcc` will be configured with
`--with-as=$TOOLCHAIN/aarch64-osdev-as` and
`--with-ld=$TOOLCHAIN/aarch64-osdev-ld`, baking the
correct paths into the new spec at build time. When
that compiler replaces this wrapper, the routing
question goes away for free.

Calling the OSdev binutils directly (e.g. `aarch64-osdev-ld`
from a Makefile rule or a test script) works fine today
and exercises the chapter 175 build path. Half B of the
smoke test proves it.

---

## What the smoke test asserts

[scripts/test_aarch64_osdev_cc.py](../../../scripts/test_aarch64_osdev_cc.py)
runs two halves against
[build/userspace/hello/hello.stripped.elf](../../../build/userspace/hello/hello.stripped.elf)
as the baseline:

**Half A — wrapper passthrough.** Compile
[userspace/hello/hello.c](../../../userspace/hello/hello.c)
and
[userspace/crt/crt0.S](../../../userspace/crt/crt0.S)
through the wrapper with the same `USER_CFLAGS` the
Makefile uses, link via the wrapper with the same
`USER_LDFLAGS` (passed as `-Wl,...`), strip with the
chapter 175 `aarch64-osdev-strip`, byte-compare to the
baseline. Passing this proves the wrapper does not
perturb the existing build for code that uses relative
`#include "../libc/..."` paths. Any drift — accidental
newlib inclusion, wrong optimisation, wrong linker
script — diverges the bytes.

**Half B — OSdev linker, directly.** Take the
Makefile-built `crt0.o` and `hello.o`, link via
`build/toolchain/bin/aarch64-osdev-ld` with the
`USER_LDFLAGS`, strip, byte-compare. Passing this
proves the OSdev binutils ld is byte-identical to brew's
for this input class, which is the contract chapter 177
needs.

Both halves currently pass:

```
$ python3 scripts/test_aarch64_osdev_cc.py
aarch64_osdev_cc: PASS — wrapper and aarch64-osdev-ld
  both produce byte-identical hello.stripped.elf (7864 bytes)
```

The test is **not** added to `scripts/sweep.sh` — sweep
runs kernel regression. This test is host-toolchain
sanity, like 175's
[test_binutils_target.py](../../../scripts/test_binutils_target.py).

`hello.c` is the test subject because it's the smallest
fully-linked binary in the tree but still pulls in
[libc/printf.h](../../../userspace/libc/printf.h),
[libc/syscall.h](../../../userspace/libc/syscall.h), and
[libc/malloc.h](../../../userspace/libc/malloc.h)
transitively through the single-TU header pattern. A
divergence has obvious places to look.

---

## What 177 will need from this

Chapter 177 needs to cross-build real binutils gas — a
multi-translation-unit C program that uses `<stdio.h>`,
`<string.h>`, `<stdlib.h>`, etc. through angle-bracket
includes rather than relative paths. That's where the
`-isystem $LIBC` half of this wrapper finally pays off:
gas's `#include <stdio.h>` resolves to
[userspace/libc/stdio.h](../../../userspace/libc/stdio.h),
not newlib's. Every libc gap (missing function, wrong
struct field, missing macro) becomes a compile error
pointing at a specific gas source file.

Two issues will surface in 177 that this wrapper
sidesteps for now and that you'll need to address:

1. **The single-TU header pattern conflicts with
   multi-TU programs.** The OSdev libc headers define
   `static inline` functions and per-TU static state
   (e.g. `_io_open_head` in `stdio.h`). For multi-TU
   gas, every `.c` that includes `stdio.h` gets its
   own `_io_open_head`, so `fflush(NULL)` walks only the
   `FILE *`s opened by THAT `.c`. The fix is to either:
   (a) extract a real `libc.a` archive built from
   per-function `.c` files;
   (b) declare select state `extern` and define once in
   crt0 or a new `libc_state.c`. Probably both.
2. **autoconf's link tests need a working `int main()
   {return 0;}` to succeed.** That needs crt0.o
   auto-appended at link time when not in `-c` mode. The
   wrapper today doesn't do this; the caller (Makefile)
   does. For configure invocations the wrapper will need
   to learn `-c`/`-S`/`-E` detection and conditional
   crt0 injection.

Both are correctly chapter 177 work, not 176.

---

## What this unlocks

- New script template:
  [scripts/aarch64-osdev-cc.in](../../../scripts/aarch64-osdev-cc.in).
- New Makefile target `aarch64-osdev-cc-install` (creates
  the wrapper at
  `build/toolchain/bin/aarch64-osdev-cc` plus the
  `as` / `ld` symlinks).
- The wrapper itself
  (`build/toolchain/bin/aarch64-osdev-cc`) is generated
  and not committed.
- No existing apps changed — the wrapper is
  forward-compatible scaffolding for 177/178/181-onwards.

## Run it / Test it

- New: `scripts/test_aarch64_osdev_cc.py` — host smoke
  test, not in sweep. Run manually after
  `make aarch64-osdev-cc-install` or after editing the
  wrapper template.
- Unchanged: the rest of `scripts/sweep.sh`. This
  chapter ships zero guest-side code.

## What's next

Chapter 177 uses this wrapper to cross-build real
binutils gas, with autoconf surfacing every libc gap as
a compile error.

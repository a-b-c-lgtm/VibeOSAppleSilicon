# Chapter 122 — Cross-building a GCC that targets osdev

**Status:** Stub. Tracking the GCC milestone, part 1.
See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

This chapter does not put GCC on the OS yet. It puts a
*host-resident* GCC into our build directory whose
output runs on the OS. The distinction matters: GCC
takes hours to build, and we only want to take that hit
once with the well-understood macOS host toolchain
before re-doing it with the in-OS toolchain in chapter
123–124.

The deliverable is `build/cross/aarch64-osdev-gcc`. It
runs on macOS. Given `hello.c`, it emits an aarch64
ELF that runs on our OS, links against our libc, and
exits cleanly. Once chapter 121's TCC and chapter 119's
`/bin/ld` are in scope, we have *two* compilers that
can produce binaries for the OS — TCC running inside,
the cross-GCC running outside.

## What this chapter adds

- A pinned GCC source tarball reference (gcc-11.5.0),
  unpacked into `external/gcc/` (gitignored).
- A `patches/gcc/` directory containing the small
  diff that introduces the `aarch64-none-osdev`
  target triple. Most of the patch lives in
  `gcc/config.gcc`, `gcc/config/aarch64/osdev.h`, and
  the equivalent files under `libgcc/config/`.
- A new top-level Makefile rule: `make gcc-cross`.
  Configures gcc 11.5 with:
  ```
  --target=aarch64-none-osdev
  --enable-languages=c
  --disable-shared
  --disable-multilib
  --without-headers
  --with-sysroot=build/sysroot
  --disable-bootstrap
  --disable-libssp
  --disable-libgomp
  --disable-libquadmath
  ```
  And builds into `build/cross/`.
- A `build/sysroot/` skeleton that mirrors the
  `/include` and `/lib` trees from chapter 121's TCC
  setup. Same crt0/crti/crtn/libgcc/libc — they're
  ABI-compatible.
- `scripts/test_gcc_cross_hello.py`: cross-compiles
  `hello.c` on macOS with the new gcc, copies the
  binary into the disk image, boots the OS, runs it,
  asserts stdout.

## Prerequisites

- Chapter 120 — runtime (the cross-gcc needs the same
  crt0 family that runs natively).
- Chapter 121 — TCC, conceptually only (this chapter
  doesn't depend on TCC, but the libc surface they
  both target is what makes the parallel possible).
- A host with ~10 GB free disk and an afternoon. The
  bootstrap step we're disabling would otherwise add
  another 6 hours.

## Plan

1. Pin GCC 11.5. It's the last 11.x; predates the
   shift to `--with-c++17` for the host build; still
   builds with the Apple-clang-shipped libstdc++ on
   macOS. (12.x and 13.x can be done too; the book
   commits to 11.5 to avoid moving target chasing.)
2. Write `gcc/config/aarch64/osdev.h`. ~80 lines.
   Defines `STARTFILE_SPEC` / `ENDFILE_SPEC` /
   `LIB_SPEC` to point at our crt0/libc; defines
   `CPP_SPEC` so `#define __osdev__` is always on;
   sets the dynamic linker to a sentinel (we have no
   dynamic linker — every binary is static).
3. Edit `gcc/config.gcc` to register the triple. ~15
   lines, copying the `aarch64-none-elf` block and
   substituting `osdev`.
4. `make gcc-cross`. Iterate on missing symbols and
   confused configure tests. Expect 5–10 build
   failures the first time; each one is a tiny
   patch.
5. Smoke: compile `hello.c`, link, boot, run.

## What you'll learn

- How GCC's configuration system is structured: a
  cascading `config.gcc` describes the target; a
  per-target header pulls together the specs strings
  the driver expands at compile time; libgcc has its
  own parallel config tree because it builds *with*
  the new compiler, not the host one.
- Why the "Canadian cross" terminology shows up in
  GCC docs (build, host, target — three separate
  axes), and why we are doing the simplest case
  (build == host == macOS, target == osdev).
- The handful of `*_SPEC` macros that control the
  driver's behaviour, and how to read them.

## What this unlocks

- Every binary in `userspace/` can be cross-compiled
  with this gcc as a parallel build pathway, useful
  when our existing host `aarch64-elf-gcc` does
  something we don't like.
- Chapter 123 — we now have a working
  `aarch64-none-osdev` GCC. The next step is to
  rebuild it with `--host=aarch64-none-osdev` so it
  runs on the OS too.

## Applied to

- **Existing apps:** the build system gains an
  alternate path (`make BUILD_CC=cross`) that
  cross-compiles userspace with the new gcc. The
  default path stays on `aarch64-elf-gcc` so the
  book chapters keep building deterministically.
- **New apps:** none yet — the cross-gcc lives on
  macOS, not on the OS.
- **New tests:** `scripts/test_gcc_cross_hello.py`,
  `scripts/test_gcc_cross_browser.py` (cross-build
  the browser with the new gcc and run it; if it
  works, the gcc is good).

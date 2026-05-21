# Chapter 123 — Cross-building a GCC that runs on osdev

**Status:** Stub. Tracking the GCC milestone, part 2.
See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

Chapter 122 produced `aarch64-osdev-gcc` running on the
*host*. This chapter produces an `aarch64-osdev-gcc`
binary that runs on the *OS itself*. It's still
cross-built (we build it on macOS), but the resulting
binary is an aarch64 ELF that we ship inside the disk
image and execute at the shell.

The technical change is one configure flag:
`--host=aarch64-none-osdev`. The practical change is
much bigger: GCC's own source, all 1.5 MLOC of it, is
now being compiled *for our OS*. Every place GCC's
code does `gettimeofday`, `open("/tmp/...", O_RDWR |
O_CREAT)`, `mmap`, `qsort`, `system()`, gets pointed at
our libc. The libc gaps that were tolerable in chapters
116–117 stop being tolerable here.

## What this chapter adds

- A second build pass in `make gcc-cross` (renamed to
  `make gcc-stage1`). Uses the chapter-122 GCC as the
  build compiler; `--host=aarch64-none-osdev` targets
  it at the OS.
- New libc bits the gcc host code exercises that
  chapters 116–117 didn't reach:
  - `mmap` of a regular file (gcc memory-maps source
    files).
  - `realloc` with growth > 1 MiB (gcc's IR storage).
  - `qsort` (gcc sorts symbol tables).
  - `system()` returning the child's exit status.
  - `mkstemp` / `tmpfile` (gcc writes intermediate
    `.s` files).
  - `gettimeofday` / `clock_gettime`
    (`__TIMESTAMP__`, build timings).
  - `signal(SIGPIPE, SIG_IGN)` (gcc's pipe to `as`).
- A bigger user heap default. cc1 compiling
  moderate-size files (e.g. the browser layout
  engine) wants ~40 MiB resident; the existing
  default (chapter 26) is much smaller.
- `scripts/test_gcc_native_smoke.py`: boots, copies
  a stripped tarball of the new gcc into
  `/bin/`, runs `gcc --version`, asserts the version
  string parses and points at our triple.

## Prerequisites

- Chapter 122 — the cross-built gcc that *targets*
  osdev. This chapter reuses it as the build compiler.
- Chapter 116–117 — libc surface, expanded with the
  new bits this chapter enumerates.
- Chapter 90 — mmap (gcc memory-maps source files).
- Chapter 101 — guard pages (cc1's recursive
  walkers blow the stack on adversarial input; a
  guard page is much friendlier than silent
  corruption).

## Plan

1. Reconfigure with `--host=aarch64-none-osdev
   --build=$(host-triple) --target=aarch64-none-osdev`.
   The build/host/target trio is the meaningful one;
   this is what GCC calls a "Canadian cross."
2. First build attempt: expect ~30 link errors for
   missing libc symbols. Triage each into "real bit
   of POSIX I should add" vs "GCC feature I should
   disable" (`--disable-...`).
3. Add the missing libc bits one at a time. Each
   gets a tiny regression script under `scripts/`.
4. Once the link is clean: copy the resulting
   `cc1`, `cpp`, `gcc`, `gcov` into the disk image
   at `/bin/`. The driver expects them at fixed
   `libexec` paths; stage those at `/lib/gcc/` too.
5. Boot and run `gcc --version` from the shell.
   That's the chapter's success criterion. **Real**
   compilation comes in chapter 124.

## What you'll learn

- The build / host / target distinction in toolchain
  configuration, and why getting these three right
  is the difference between "compiler builds" and
  "compiler runs."
- Why GCC's source has so many `HOST_*` /
  `BUILD_*` macros: code that runs at compile-time
  on the build machine needs the build machine's
  word sizes; code that runs in the deployed gcc
  needs the host's. The distinction maps directly
  onto the configure axes.
- The fingerprint of a real-world libc gap: a
  surprising link failure on a symbol like
  `__cxa_demangle` or `getpwuid_r` that *something
  deep inside GCC* calls only when a specific kind
  of source is parsed.

## What this unlocks

- Chapter 124 — first native compile of a C program
  inside the OS using GCC (not just TCC). The
  difference vs chapter 121 is that the binary's
  generated code is now -O2 quality.
- Chapter 125 — self-hosting bootstrap. Once gcc
  runs on the OS, it can be asked to rebuild itself
  from source while running on the OS, which is the
  textbook definition of self-hosting.

## Applied to

- **Existing apps:** still none — gcc is added,
  nothing is rewritten.
- **New apps:** `/bin/gcc`, `/bin/cc1`, `/bin/cpp`,
  `/bin/gcov`. Each one is large (cc1 is ~15 MiB
  stripped) — chapter 124 covers the disk-image
  layout choice this forces.
- **New tests:** `scripts/test_gcc_native_smoke.py`,
  plus libc regressions for each of the new symbols
  this chapter forces us to add.

# Chapter 121 — TinyCC: a one-chapter native C compiler

**Status:** Stub. Tracking the milestone "first native
compiler". See [Chapter 115](115-c-compiler-strategy.md)
for why TCC comes before GCC.

## Why this chapter exists

This is the watershed chapter for Part XVII: by the end
of it the OS contains a C compiler that compiles C
programs on the OS, with no host involvement past
"booted QEMU."

[TinyCC](https://bellard.org/tcc/) is the right choice
for *first* because it is small (~25 KLOC), self-
contained (its own assembler, its own linker, no
preprocessor dependency), and historically targets
aarch64 cleanly. Its generated code is unoptimised —
expect 2–3x slower than GCC `-O2` — but for the
"compile a 50-line C file inside the OS" loop, that
doesn't matter. What matters is that the loop *runs*.

GCC comes in chapters 122–124. This chapter is the
proof-of-concept that the libc work (116–117) and the
runtime work (120) are sufficient to host a compiler at
all.

## What this chapter adds

- A cross-built `/bin/tcc` in the disk image at build
  time. Source patched lightly to use our libc paths
  (`/include/`, `/lib/`) and our crt0.
- A `/include/` tree on disk: the libc headers from
  chapter 116–117 (`stdio.h`, `stdlib.h`, `string.h`,
  `unistd.h`, `fcntl.h`, `sys/stat.h`, `dirent.h`,
  `errno.h`).
- A `/lib/libc.a` archive (assembled by host `ar` at
  build time; `/bin/ar` from chapter 119 makes the
  same file when we run it natively).
- A `/lib/crt0.o`, `/lib/crti.o`, `/lib/crtn.o`,
  `/lib/libgcc.a` — copies of chapter 120's runtime,
  exposed at the well-known paths the compiler driver
  searches.
- `/data/src/hello.c` — the first program written for
  this loop.
- `scripts/test_tcc_hello.py`: boots, runs
  `tcc /data/src/hello.c -o /tmp/hello && /tmp/hello`,
  asserts stdout matches "hello, osdev\n".

## Prerequisites

- Chapter 116 — stdio, errno, env.
- Chapter 117 — stat, fcntl, dirent.
- Chapter 120 — crt0 + libgcc stubs.
- Chapter 119 is *not* a hard prereq for TCC itself
  (TCC links internally), but chapter 121's test
  harness uses `/bin/ar` from chapter 119 to build
  `/lib/libc.a` natively as an end-to-end check.

## Plan

1. Pull TCC source (a specific git SHA, pinned in our
   `Makefile`). Apply a `patches/tcc/` directory of
   small fixups: search paths, removal of `dlopen`
   support (we have no dynamic loading), removal of
   the bcheck runtime (uses `mprotect` we don't
   ship), Plan-9-style assembly directives accepted.
2. Cross-build TCC on the host with our existing
   `aarch64-elf-gcc`. Output binary is statically
   linked and stripped; goes to `build/userspace/tcc`
   and gets copied into the OSFS image alongside
   every other `/bin/*`.
3. Stage the `/include` and `/lib` trees in the
   image's OSFS-2 (`/data/` partition, exposed
   read-only at `/include` and `/lib` via the mount
   table from chapter 113 — until 113 lands, hard-
   coded paths).
4. Write `hello.c`, ship it as `/data/src/hello.c`.
5. Bring up the test. Expect the first run to fail in
   a libc symbol we forgot; iterate until green.

## What you'll learn

- How a real (if small) C compiler is structured: a
  preprocessor pass, a lexer, a recursive-descent
  parser, a code generator that walks AST nodes and
  emits machine code directly (no IR).
- The relationship between `--sysroot`, search paths,
  and the path-prefix trick GCC and TCC both use to
  find headers and libraries.
- Why a working "hello world" is a more meaningful
  milestone than a passing test suite — every
  syscall, libc symbol, and ELF feature your compiler
  uses has to actually work on the target.

## What this unlocks

- The full inner loop: edit, compile, run, inside
  the OS, with no host. **This is the chapter the
  section was written for.**
- Chapter 122 (GCC bring-up) — TCC will compile the
  early bits of GCC's source while we get GCC itself
  building. (GCC can be host-compiled by TCC.)
- A reader path: if you only want a native compiler
  and don't care about optimisation, you can stop
  here.

## Applied to

- **Existing apps:** none changed yet — TCC arrives
  alongside the rest of the system.
- **New apps:** `/bin/tcc`. Add a wrapper
  `/bin/cc` -> `tcc` symlink (or a 5-line shell
  script) so the conventional name works.
- **New demos:** `/data/src/hello.c`,
  `/data/src/fizzbuzz.c`, a tiny
  `/data/src/sokoban.c` (the same toy that motivates
  the chapter title in many books).
- **New tests:** `scripts/test_tcc_hello.py`,
  `scripts/test_tcc_link_libc.py`,
  `scripts/test_tcc_recompiles_self.py` (TCC can
  build TCC, which is a powerful smoke test).

## Foreshadowing

The TCC ceremony of chapter 125 (the GCC bootstrap) is
already possible here: `tcc` can compile `tcc.c` to a
new `tcc`. The book doesn't make a big deal of it
because GCC's bootstrap is the headline. But for the
reader who stops at this chapter, "tcc compiles tcc"
is the same milestone in miniature.

# Chapter 120 — Bootstrap glue: crt0, crti, crtn, libgcc stubs

**Status:** Stub. Tracking the toolchain milestone "native
runtime". See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

A C program does not start at `main`. It starts at
`_start`, supplied by `crt0.o`, which sets up `argc` /
`argv` from the stack, calls any global constructors,
runs `main`, takes its return value, and calls `_exit`.
GCC (and TCC) generate code that *assumes* these support
files exist in known names at known paths; the linker
links them automatically. If they're missing, the most
trivial `int main(){return 0;}` won't link.

We have the equivalent today — `userspace/libc/crt0.S`
or similar — but it's hand-written for our existing
build, where the kernel's ELF loader does the argc/argv
setup and `_start` just dispatches to `main`. Real
compilers expect:

- `crt0.o` — the entry point.
- `crti.o` + `crtn.o` — the prologue and epilogue
  halves that bracket the global-constructor calls
  (`__init_array` machinery).
- `libgcc.a` — the small set of helper symbols GCC's
  code generator can emit calls to: 128-bit
  multiplication, soft-float for `_Float128`, the
  stack-protector epilogue, `__cxa_finalize`. On
  aarch64 the list is short — most arithmetic the
  hardware does natively.

This chapter writes the four files and proves they're
correct by linking and running every existing
`userspace/` program against them.

## What this chapter adds

- `userspace/runtime/crt0.S`: the entry point. Calls
  `__libc_init` (chapter 116's stdio setup),
  `__init_array_start..__init_array_end`, `main`,
  then `_exit(main_return_value)`.
- `userspace/runtime/crti.S` + `crtn.S`: the two
  halves of the `.init` and `.fini` section's
  per-translation-unit functions.
- `userspace/runtime/libgcc.c`: ~30 helper functions
  GCC emits calls to. On aarch64 these are mostly
  `__aarch64_ldadd*_acq_rel` (atomic helpers
  pre-LSE), `__udivti3` (128-bit unsigned divide),
  `__multi3` (128-bit multiply), `__stack_chk_fail`,
  `__cxa_finalize`. Most are 5–20 lines each.
- A small Makefile rule that packages these into
  `build/libgcc.a` (using `/bin/ar` from chapter
  119 at runtime, or the host `ar` at build time —
  both paths work).
- `scripts/test_crt0_chain.py`: links a one-line
  program with global constructors, asserts they run
  in the right order before `main` and finalisers
  run before `_exit`.

## Prerequisites

- Chapter 118 — `/bin/as` to assemble the crt files
  inside the OS (the host build also continues to
  work).
- Chapter 119 — `/bin/ld` to link the runtime into
  binaries built on the OS.
- Chapter 116 — `__libc_init` for stdio.

## Plan

1. Inventory: grep the current `userspace/libc/*.S`
   and the makefile for everything that touches
   `_start` and `__init_array`. Move it into the new
   `userspace/runtime/` directory and split into the
   four files.
2. Sort out symbol names so they match what
   `aarch64-elf-gcc` (host) emits AND what our future
   `/bin/cc` will emit. The two should agree because
   they're the same GCC source line; if they don't,
   chapter 123's bring-up will catch it.
3. Write the libgcc helpers in plain C with `static
   inline` 128-bit operations expanded by hand. Cross-
   check against host `libgcc.a` (`aarch64-elf-gcc
   -print-libgcc-file-name` then `nm`).
4. Add a regression for `__cxa_atexit` ordering —
   atexits run in reverse registration order; getting
   this wrong is invisible until a test fails 50
   chapters from now.

## What you'll learn

- Why `crt*.o` is split across so many files — each
  one contributes a *chunk* of the `.init` and
  `.fini` sections, so concatenating them in the
  right order yields well-formed entry/exit
  functions.
- What `libgcc.a` actually is: a tiny library of
  arithmetic helpers the compiler emits calls to
  when the target ISA can't do the op in a single
  instruction.
- Why every Unix-y compiler has a copy of this code
  even though it's identical across them — toolchain
  independence is more important than not having
  three copies on disk.

## What this unlocks

- Chapter 121's TCC port has the crt0 it needs to
  link the C programs it compiles.
- Chapter 122–124's GCC has the same. With this
  chapter shipped, a binary linked by *any* compiler
  on our system can start, run, and exit cleanly.

## Applied to

- **Existing apps:** every binary in `userspace/`
  relinks against the new runtime. Behaviour is
  identical; the build provenance changes.
- **New apps:** none.
- **New tests:** `scripts/test_crt0_chain.py`,
  `scripts/test_atexit_order.py`.

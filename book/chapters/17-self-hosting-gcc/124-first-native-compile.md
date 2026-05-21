# Chapter 124 — First native compile: hello.c on the desktop

**Status:** Stub. Tracking the GCC milestone, part 3.
See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

By the start of this chapter, `/bin/gcc` exists in the
disk image, `gcc --version` works at the shell, and the
libc surface is wide enough to host the compiler. What
*hasn't* been done is run an actual compile — turn a
`.c` file in `/data/src/` into a runnable binary in
`/bin/`, using only the in-OS toolchain.

This is a debugging-diary chapter as much as a feature
chapter. cc1 compiling even a trivial file exercises
ten times more libc surface than `gcc --version` did,
and it brings up the resource conversation: how big the
user heap needs to be, how much room the disk image
needs for transient `.s` and `.o` files, and where
those files should live.

## What this chapter adds

- A larger user heap default — cc1 compiling a 200-
  line `.c` file wants ~40 MiB resident. Today's
  user heap (last bumped in chapter 71 for the
  browser) is comfortably under that.
- A `/tmp/` ramdisk big enough for transient compile
  artifacts. `gcc` writes `/tmp/ccXXXXXX.s` then
  `/tmp/ccXXXXXX.o` per source file; both can be
  hundreds of KiB to MiB.
- A `/data/src/` directory in the OSFS-2 image with a
  starter set of example programs:
  `hello.c`, `fizzbuzz.c`, `sokoban.c`, `life.c`,
  `compile_browser.c` (a one-file rewrite of a
  smaller utility for the multi-file demo).
- `scripts/test_native_gcc_hello.py`: boots, opens
  `gui_term`, runs
  `gcc -O2 -o /bin/myhello /data/src/hello.c`,
  runs `/bin/myhello`, asserts stdout.
- A book-side debugging diary section: every
  obstacle the implementation hit, and what fixed
  each one. Modeled on
  [chapter 101](../12-system-services/101-guard-pages.md)
  in style.

## Prerequisites

- Chapter 123 — gcc binaries staged in `/bin/` and
  `/lib/gcc/`.
- Chapter 79b — gui_term spawning real processes
  (we want the demo to feel like a real shell
  session, not a one-shot).
- Chapter 116-117 — full libc surface.
- Chapter 120 — runtime crt0 the produced binary
  links against.

## Plan

1. Bump the user heap default and document the new
   ceiling.
2. Size `/tmp/` to at least 64 MiB. Add a chapter-
   level note that anyone rebuilding bigger binaries
   in-OS may need more.
3. Stage the example programs in `/data/src/`.
4. Run `test_native_gcc_hello.py`. Capture the
   resource hits in the diary:
   - `cc1` calls `mmap(NULL, 0x4000000, ...)` early
     for IR memory — heap sizing.
   - First link failure on a libc symbol GCC's host
     code reaches into for `__cxa_thread_atexit`.
   - Output ELF rejected by our loader because
     `p_align` is 0x10000 (the kernel ELF loader
     wants 0x1000).
5. Each of the obstacles becomes a one-paragraph
   subsection. The whole chapter is ~3000 words once
   the diary fills in.

## What you'll learn

- The size economics of a working C compiler — why a
  real compile chews through tens of megabytes of
  IR, and why the original 1970s GCC ancestors were
  built to streaming-IR designs we long since
  abandoned for in-memory ones.
- The fingerprint of an under-sized user heap vs an
  under-sized `/tmp/`: cc1 hangs vs the link step
  fails with EIO, respectively.
- The "everything's working but the binary won't
  load" debugging path — ELF spec compliance has a
  lot of room for surprise even when the bytes
  *look* fine in `objdump`.

## What this unlocks

- The OS now has a working "edit, compile, run"
  loop inside the guest using a *real* compiler.
- Chapter 125 — the self-hosting bootstrap is a
  scaling-up of what this chapter just proved on a
  toy program.
- Chapter 127 — the notepad Build button has
  something real to call.

## Applied to

- **Existing apps:** gui_term gains the role of "the
  REPL for the new compiler" — no code change, but
  the chapter's screenshots all use it.
- **New apps:** none, but the example programs in
  `/data/src/` are themselves a kind of app — they
  exist to be compiled by readers.
- **New tests:** `scripts/test_native_gcc_hello.py`,
  `scripts/test_native_gcc_O2.py` (asserts an
  optimised build still runs),
  `scripts/test_native_gcc_link_libc.py` (asserts a
  multi-translation-unit link succeeds — uses two
  fixture files in `/data/src/multi/`).

## A note on disk pressure

cc1 compiling `gcc/expr.c` (one of GCC's bigger
internal files) writes ~3 MiB of intermediate state
to `/tmp/`. If you want to do that inside the OS —
which we'll do for real in chapter 125 — `/tmp/`
needs to grow. This chapter is the right place to
take the hit, because the alternative is "discover
the limit during the multi-hour bootstrap of chapter
125 and have to restart."

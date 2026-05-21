# Chapter 115 — PLAN: A C compiler that runs on the OS

> **Status: PLAN ONLY.** This chapter is the strategy
> document for the whole of Part XVII. No code lands here;
> the matching implementation work is split across
> chapters 116–127.

## Why this section exists

The book has built every binary in the disk image with a
macOS-hosted aarch64 cross-toolchain (`aarch64-elf-gcc`,
`aarch64-elf-as`, `aarch64-elf-ld`, plus our own libc).
That works, and it is how the book has gotten this far,
but it leaves a gap between two halves of an operating
system that an early-1970s Unix would have closed years
ago: **a program written and run on the OS itself.**

Today the workflow for adding a feature is:

1. Edit a `.c` file on macOS.
2. `make`, which cross-compiles into `build/disk.img`.
3. Boot QEMU, watch.
4. Repeat.

The OS is a passenger in its own development. This
section makes the OS a participant: source files live
in `/data/src/`, a compiler lives in `/bin/cc`, and the
loop is `vi hello.c; cc hello.c -o hello; ./hello` —
inside the running guest, with no host involvement.

## What "native compiler" actually means

There are four moving pieces. The book builds them in
this order:

| Piece | Today              | After Part XVII             |
|-------|--------------------|------------------------------|
| libc  | freestanding subset| POSIX-ish (file I/O, env, stat) |
| as    | host `aarch64-elf-as` | `/bin/as`                 |
| ld    | host `aarch64-elf-ld` | `/bin/ld` + `/bin/ar`     |
| cc    | host `aarch64-elf-gcc`| `/bin/cc` (a real GCC)     |

A C compiler is the deepest of those, and the only one
that lights up the workflow. The other three are
prerequisites: GCC needs an assembler and a linker behind
it, and both need a libc rich enough for compiler
internals (file mapping, qsort, big malloc/realloc, env
lookup, signals during build).

## The two-compiler strategy: TinyCC first, GCC second

GCC is ~1.5 MLOC, builds for hours even cross, and pulls
in C++ for its host build above gcc-4.7. Standing it up
as the *first* native compiler invites months of
yak-shaving on libc gaps we can't see until the link
fails on symbol 800. The book takes a smaller bite first.

**[TinyCC](https://bellard.org/tcc/) (~25 KLOC)** is the
shortest path to "the OS can build a C program." It
includes a built-in assembler and a built-in linker, so
chapters 118 and 119 (`/bin/as`, `/bin/ld`) are not
prerequisites for it — they're prerequisites only for
GCC, which uses the system assembler and linker by
default. TCC's bring-up validates the libc work in
chapters 116–117 against a real compiler before we
commit to the GCC effort.

GCC is then the headline. It produces measurably faster
code, supports the whole language, and — most
importantly — the moment it self-hosts, the OS has
joined the small club of systems that can rebuild
themselves from source without their host.

## Reading flow

```
115 (this chapter)
 │
 ▼
116 ── 117       (POSIX libc)
       │
       ├─────▶ 118 ── 119 ── 120   (as, ld, crt0)
       │                       │
       │                       ▼
       └────────────▶ 121     122 ── 123 ── 124 ── 125
                     (TCC)    (GCC bring-up + native)
                       │                       │
                       ▼                       ▼
                     hello.c                hello.c
                     on the OS,             on the OS,
                     via TCC                via GCC
                                              │
                                              ▼
                                            126 (make)
                                              │
                                              ▼
                                            127 (notepad Build button)
```

If the reader only wants TinyCC as their native compiler,
they can stop at chapter 121. The rest of the section
upgrades to a real GCC and the self-hosting ceremony at
chapter 125.

## What this section explicitly does NOT do

- **C++.** GCC's C++ front-end (`cc1plus`) is ~3x the
  size of `cc1`. We keep `--enable-languages=c` to
  finish in this lifetime.
- **glibc.** Our libc grows toward POSIX, not toward
  glibc. We add what compilers need; we are not chasing
  application compatibility with Linux binaries.
- **Modern GCC.** The book pins GCC 11.5 (the last
  release before `--with-c++17` got mandatory for the
  host build and before configure started assuming
  modern Autoconf). Pinning is documented in chapter 122.
- **Optimisation correctness audit.** GCC's `-O2`
  produces correct code on aarch64. We trust the
  upstream test suite for that and run only a smoke
  subset inside the OS in chapter 125.

## What this section unlocks

- **A real `/bin/cc`**: shell loop `cc foo.c && ./a.out`.
- **`/bin/make`** (chapter 126): multi-file projects
  inside the guest.
- **Notepad → Build → run** (chapter 127): an in-OS
  dev loop that doesn't require restarting QEMU.
- **Self-hosting bootstrap** (chapter 125): the OS can
  rebuild its own GCC. The book stops being the only
  way to grow the OS.

## Prerequisites

- Chapter 73 — fork (so GCC can spawn `cc1`, `as`, `ld`).
- Chapter 74 — exec.
- Chapter 90 — mmap (GCC mmaps source files).
- Chapter 95 — wall-clock time (build timestamps,
  `__DATE__` / `__TIME__` macros).
- Chapter 100 — strace (debugging the GCC bring-up is
  unpleasant without it).

## Applied to

This is a planning chapter — nothing is applied yet.
The follow-up chapters each name the apps and binaries
they add.

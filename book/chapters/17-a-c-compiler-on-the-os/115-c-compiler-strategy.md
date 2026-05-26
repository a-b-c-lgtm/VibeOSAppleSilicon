# Chapter 115 — A C compiler that runs on the OS

> **Milestone in this chapter:** strategy overview for Part XVII.
> **Code referenced (delivered across the section):**
> - [userspace/cc/cc.c](../../../userspace/cc/cc.c)
> - [userspace/as/as.c](../../../userspace/as/as.c)
> - [userspace/ld/ld.c](../../../userspace/ld/ld.c),
>   [userspace/ar/ar.c](../../../userspace/ar/ar.c)
> - [userspace/libc/](../../../userspace/libc/) (POSIX-ish growth)
>
> **At the end of this chapter** you will understand the four moving
> pieces (libc, assembler, linker, compiler), the order in which the
> rest of Part XVII builds them, and why the section ships a tiny
> from-scratch compiler instead of porting GCC. **No code lands in
> this chapter.** The actual implementations land in chapters 116
> through 127, and a real GCC follows in Part XVIII.

## Why this section exists

Every binary on the disk image so far has been built with a
macOS-hosted aarch64 cross toolchain (`aarch64-elf-gcc`,
`aarch64-elf-as`, `aarch64-elf-ld`, plus the in-tree libc). That
works, and it is how the book has gotten this far, but it leaves a
gap between two halves of an operating system that an early-1970s
Unix would have closed years ago: **a program written and run on
the OS itself.**

Today the workflow for adding a feature is:

1. Edit a `.c` file on macOS.
2. `make`, which cross-compiles into `build/disk.img`.
3. Boot QEMU, watch.
4. Repeat.

The OS is a passenger in its own development. This section makes
the OS a participant: source files live in `/data/src/`, a compiler
lives in `/bin/cc`, and the loop is
`vi hello.c; cc hello.c -o /tmp/hello; /tmp/hello` — inside the
running guest, with no host involvement.

## What "native compiler" actually means here

Four moving pieces. The book builds them in this order:

| Piece | Today              | After Part XVII             |
|-------|--------------------|------------------------------|
| libc  | freestanding subset| POSIX-ish (file I/O, env, stat) |
| as    | host `aarch64-elf-as` | `/bin/as` (subset)        |
| ld    | host `aarch64-elf-ld` | `/bin/ld` + `/bin/ar` (subset) |
| cc    | host `aarch64-elf-gcc`| `/bin/cc` (~1000 LOC, tiny subset of C) |

A C compiler is the deepest of those, and the only one that lights
up the workflow. The other three are prerequisites — even a small
compiler has to call out to an assembler and a linker.

## The choice: build a tiny compiler, not port a big one

There are two well-trodden roads into "the OS hosts a compiler":

1. **Port a real compiler.** GCC is ~1.5 MLOC, builds for hours
   even cross, and pulls in C++ for its host build above gcc-4.7.
   TinyCC is ~25 KLOC with its own built-in assembler and linker.
   Either port is months of work mostly spent in libc gap-filling
   and configure archaeology.
2. **Write the smallest compiler that closes the loop.**
   A pedagogical tool: enough C to demonstrate that
   `/bin/as` + `/bin/ld` + a libc actually compose, and
   to host a Build button in notepad.

The book takes the second road. **Part XVII ships a
from-scratch `/bin/cc`** — under 1000 lines, accepting
a tiny subset of C (literals, locals, `int` arithmetic,
calls to a fixed-shape `printf`, `return`/`exit`). It
is not GCC. It is not TinyCC. It will not self-host.
What it does is make every step of the
source-to-executable pipeline visible, debuggable, and
modifiable in a single afternoon.

Chapter 125 spells out the gap between `/bin/cc` and a
self-hosting compiler in detail, and is upfront that
closing the gap is a Part XVIII undertaking we do not
attempt here.

## Reading flow

```
115 (this chapter — strategy)
 │
 ▼
116 ── 117                  (POSIX-ish libc growth)
       │
       ├─────▶ 118 ── 119 ── 120  (/bin/as, /bin/ld, crt0 + libgcc-style stubs)
       │                       │
       │                       ▼
       └────────────▶ 121 ── 122 ── 123 ── 124 ── 125
                     (/bin/cc — literals only)
                              │           │      │       │
                              │           │      │       └─ self-hosting gap
                              │           │      └────────  hello.c on disk
                              │           └───────────────  /bin/cc grows locals + arith
                              └──────────────────────────── cross-toolchain contract
                                                            │
                                                            ▼
                                                          126 ── 127
                                                          (/bin/make, notepad Build)
```

A reader who only wants to see "the OS compiled a C
file end-to-end" can stop after chapter 124. Chapters
126–127 turn that into a developer loop a human would
actually use.

## What this section explicitly does NOT do

- **A real GCC port.** That is left for Part XVIII.
  Chapter 125 catalogues the language work `/bin/cc`
  would need before a GCC bootstrap would even be
  meaningful.
- **A TinyCC port.** Considered and rejected for the
  same reason — TCC is a fine compiler but porting it
  is a months-long project that produces nothing
  user-visible until it links cleanly. Chapter 121
  explains the trade-off in detail.
- **C++.** `/bin/cc` accepts a strict subset of C; C++
  is not on the roadmap.
- **glibc.** Our libc grows toward POSIX (what `/bin/cc`,
  `/bin/make`, and a future GCC port need), not toward
  Linux binary compatibility.
- **Optimisation.** `/bin/cc` emits straight-line code
  at one quality level. No constant folding, no
  register allocation beyond the obvious, no peepholes.

## What this section unlocks

- **`/bin/cc`** (chapters 121, 123): a usable shell
  loop `cc foo.c -o /tmp/foo && /tmp/foo` for tiny
  programs.
- **`/bin/make`** (chapter 126): one-rule Makefiles
  driving the toolchain.
- **Notepad → Build → run** (chapter 127): an in-OS
  dev loop that doesn't require restarting QEMU.
- **An honest baseline for Part XVIII.** When the book
  later attempts a real compiler port, the cross-
  toolchain contract (chapter 122), the libc surface
  (chapters 116–117), and `/bin/as` + `/bin/ld`
  (chapters 118–119) are already standing.

## Prerequisites

- Chapter 73 — fork (so `make` and `cc` can spawn
  helper processes).
- Chapter 74 — exec.
- Chapter 90 — mmap (would be useful for a future
  compiler that wants to mmap source files; `/bin/cc`
  uses `read` instead).
- Chapter 95 — wall-clock time (Makefile timestamp
  rules, `__DATE__` / `__TIME__` macros for a future
  preprocessor).
- Chapter 100 — strace (debugging the toolchain pipeline
  is much easier with it).

## Applied to

This is a planning chapter — nothing is applied yet.
The follow-up chapters each name the apps and binaries
they add.


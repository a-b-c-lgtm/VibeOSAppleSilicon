# Chapter 126 — Porting GNU make

**Status:** Stub. Tracking the developer-loop
milestone. See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

The native gcc works, but driving a multi-file build
by hand at the shell — `gcc -c a.c; gcc -c b.c; gcc
a.o b.o -o app` — is tedious in any project bigger
than three files. The native compiler is only
genuinely useful when paired with a build driver.

GNU make is the obvious choice. It's ~40 KLOC, the
syntax everyone knows, and the source is
freestanding-friendly in the same way TCC was — most
of the host requirements are POSIX features we've
been adding for the compiler anyway.

## What this chapter adds

- A cross-built `/bin/make` in the disk image,
  configured to use `/bin/sh` for recipe execution
  and our `/tmp/` for any temporary files.
- A `/data/src/example/` project: three `.c` files,
  one header, and a Makefile. Builds to
  `/bin/example`. Demonstrates the toolchain
  end-to-end on something more interesting than
  hello world.
- A small `Makefile` snippet in the book that
  readers can copy-paste as a starting template.
- `scripts/test_make_example.py`: boots, runs
  `cd /data/src/example && make && ./example`,
  asserts stdout.

## Prerequisites

- Chapter 124 — native gcc compiles things.
- Chapter 73 — fork (make spawns recipes).
- Chapter 74 — exec.
- Chapter 78 — SIGCHLD / waitpid (make waits on
  every recipe).
- Chapter 79 — job control (make `-j N` parallel
  builds use it).
- Chapter 116–117 — the libc surface, particularly
  `getenv`/`setenv` (Makefiles read env), `stat`
  (`make`'s entire dependency-tracking model is
  built on mtimes), `dup2` (for `>` and `2>` in
  recipes).

## Plan

1. Pull make-4.4 source. Patch out GLOB support
   (we don't have `glob.h`), GNU-extension flags
   we don't ship, the LOAD plugin support.
2. Cross-build with chapter 122's gcc. Output is
   one statically-linked binary; copy to `/bin/`.
3. Verify the example project builds. Iterate on
   any libc gaps. Most likely culprits:
   `WIFEXITED`/`WEXITSTATUS` macros in our
   `sys/wait.h`; the `vfork` symbol (alias to
   `fork` — we don't need real vfork).
4. Document parallel-build behaviour with our SMP
   scheduling (chapter 92). `make -j2` should
   genuinely use both cores; `make -j8` is
   pointless because we only boot with two.

## What you'll learn

- Why the original Unix `make` design — "rebuild a
  target if any of its dependencies have a newer
  mtime" — depends *crucially* on every
  filesystem in the system supporting accurate
  mtimes. Our OSFS-2 has them (chapter 81);
  tmpfs has them; procfs returns boot-time for
  everything, which is fine because nothing in
  procfs is a build dependency.
- The relationship between `make -j` and
  `SIGCHLD` — `make` is a near-perfect demo of
  every primitive in the Unix process model.
- Why "rule-based" build systems were the
  state of the art for 50 years even though
  declaratively they describe a tiny fragment of
  the build graph problem.

## What this unlocks

- Multi-file projects can now be built
  iteratively inside the OS without typing every
  gcc invocation.
- Chapter 127 — the notepad Build button shells
  out to `make` (when a Makefile is in the
  current directory) instead of inventing its
  own build orchestration.

## Applied to

- **Existing apps:** none changed.
- **New apps:** `/bin/make`. Add `/bin/touch`
  (one-liner using `utimensat`) as a make
  convenience.
- **New demos:** `/data/src/example/` — three
  source files plus a Makefile. Big enough to
  motivate make's dependency-tracking; small
  enough to read in one sitting.
- **New tests:** `scripts/test_make_example.py`,
  `scripts/test_make_parallel.py` (asserts
  `make -j2` actually overlaps two recipes
  using `/proc/<pid>/stat` snapshots).

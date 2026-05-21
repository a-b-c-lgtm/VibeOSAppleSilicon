# Chapter 125 — Self-hosting GCC: stage 2 builds stage 3

**Status:** Stub. Tracking the headline milestone of
Part XVII. See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

The classical GCC bootstrap is a three-stage build:

1. **Stage 1**: build GCC using whatever C compiler
   the host provides. (For us: chapter 122's
   cross-build, run on macOS.)
2. **Stage 2**: build GCC using the stage-1 compiler.
3. **Stage 3**: build GCC using the stage-2 compiler.

The success criterion is that the **stage-2 and
stage-3 outputs are byte-for-byte identical**. If they
are, the compiler is *self-hosting*: it can be
rebuilt from source using itself, and the source's
behaviour is invariant under that operation. That's
the textbook definition of a stable compiler.

For us, the additional twist is that stages 2 and 3
both run **inside the OS**. Stage 1 was built on macOS
(chapter 122–123), produces the `/bin/gcc` that runs
on osdev (chapter 124 proved it compiles a hello
world). Stage 2 boots the OS, runs that `/bin/gcc`
against GCC's own source tree on `/data/src/gcc/`, and
produces a new `/bin/gcc`. Stage 3 boots a fresh OS,
runs the stage-2 `/bin/gcc` against the same source
tree, and produces a third `/bin/gcc`. If stage 2 ==
stage 3, we've self-hosted.

The book treats this as a **one-time ceremony**, not
a CI regression. The full bootstrap takes many hours
inside QEMU; on Apple Silicon with HVF, expect 6–12
hours depending on `-smp` count and disk pressure.
Running it every commit would gate development on
"did the bootstrap pass" and slow everything else by
orders of magnitude.

## What this chapter adds

- A shipped GCC source tree on disk at
  `/data/src/gcc/`. Pinned to the same gcc-11.5.0 as
  chapter 122. ~700 MiB unpacked; gzipped tarball in
  the OSFS image to save space, unpacked on first
  use.
- A `/data/src/gcc/bootstrap.sh` script that runs
  the three stages and diff-checks the outputs.
- A bigger disk image and (when running this
  chapter) a bigger `/tmp/`. Documented per-chapter
  rather than baked into the default — we don't want
  every reader paying a 4 GiB disk-image cost for a
  chapter most won't reproduce.
- The book chapter is mostly *narrative*: a
  cookbook for running the ceremony, plus
  screenshots of the diff that should come back
  empty.
- `scripts/run_bootstrap.sh` (host-side): kicks off
  the in-guest bootstrap with extra RAM, extra
  cores, extra disk, and a longer timeout — and
  parks the OS console output to
  `/tmp/bootstrap.log` so the reader can watch
  along.

## Prerequisites

- Chapter 124 — gcc compiles a hello world inside
  the OS.
- Chapter 89 — SMP scheduling (the bootstrap is
  parallelizable; `make -j2` halves the wall time).
- Chapter 90 — mmap of files (every source file is
  mmapped during compile).
- Chapter 99 — procfs (so the reader can watch
  cc1's RSS climb in real time via `ps`).

## Plan

1. Stage the source on disk. Bundle as a tarball in
   the OSFS image; expand to `/data/src/gcc/` on
   first use via a one-line shell helper.
2. Stage 2: from inside the OS, `cd /data/src/gcc &&
   ./configure --target=aarch64-none-osdev
   --host=aarch64-none-osdev
   --build=aarch64-none-osdev --enable-languages=c
   --disable-shared --disable-multilib
   --disable-bootstrap --prefix=/stage2`. Then
   `make -j2`. Wait. Eat lunch.
3. When stage 2 finishes (`/stage2/bin/gcc` exists),
   snapshot the build artifacts into a directory
   that won't get wiped.
4. Stage 3: same configure, same make, but with the
   stage-2 gcc in `$PATH` ahead of the stage-1 gcc.
5. Compare:
   ```
   cmp /stage2/bin/cc1 /stage3/bin/cc1
   cmp /stage2/bin/gcc /stage3/bin/gcc
   ```
   Expect both to be byte-identical. If they aren't:
   - Most common cause: a `__DATE__`/`__TIME__`
     macro leaked into the binary. Add `-DNDEBUG`
     and `-frandom-seed=fixed` to the per-stage
     `CFLAGS_FOR_TARGET`.
   - Second most common: timestamps in the ELF
     headers. Use the `--enable-determinism` flag
     pin we'll add in this chapter.
6. Document the successful bootstrap log
   verbatim. The reader's run will look the same
   modulo timestamps.

## What you'll learn

- What "self-hosting" precisely means as a
  formal property of a toolchain, and why getting
  there is the moment a Unix-y system stops being
  toy.
- Why `__DATE__` and `__TIME__` are the bane of
  reproducible builds (and why
  `SOURCE_DATE_EPOCH` exists in the open-source
  world to fix exactly this).
- The diminishing returns of stage N for N > 3 —
  if 2 == 3, additional stages don't add
  information.

## What this unlocks

- **The OS as a software-development platform.**
  Everything written from chapter 1 to chapter 124
  was prerequisite plumbing for this single
  moment.
- A reader who reaches this chapter can, in
  principle, walk into a forest with a Mac and a
  copy of the book and re-derive the OS from
  source, with the OS itself helping at every
  step past chapter 124.

## Applied to

- **Existing apps:** the bootstrap exercises every
  layer of the OS at scale. Bugs that surface
  during the multi-hour run go in
  `/memories/repo/` as their own files (per the
  existing repo memory discipline).
- **New apps:** the bootstrap itself is the
  "app." No new binaries beyond the gcc family
  that already shipped in chapter 124.
- **New tests:** the ceremony is the test. We do
  add `scripts/test_bootstrap_smoke.py` — a
  fast variant that builds only `gcc/c-family/`
  (a sub-directory, ~100 files) at two stages
  and diffs the results. Runs in ~20 minutes and
  catches the common "non-determinism crept in"
  regression without the full 6–12 hour budget.

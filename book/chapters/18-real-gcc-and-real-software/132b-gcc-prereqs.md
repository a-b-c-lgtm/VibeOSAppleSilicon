# Chapter 132b — GMP, MPFR, MPC as in-tree prerequisites

> **Status:** shipped. `make gcc-osdev-prereqs` (or any
> top-level target — `make`, `make run`, `make run-graphical`)
> downloads three sha256-pinned tarballs and symlinks them
> into the patched gcc source tree as
> `vendor/gcc-14.2.0/{gmp,mpfr,mpc}`. Host smoke test
> `scripts/test_gcc_prereqs.py` passes (verifies sha256s,
> symlink targets, and that each linked dir has a
> `configure` script).
> **Prereq:** chapter 132a (the patched gcc source tree
> must exist before anything can symlink into it).
> **Opens:** Phase 2 step 3 — the in-tree math libraries
> that gcc's configure expects to find when it builds xgcc.
> Chapter 132c will run the actual configure + `make
> all-gcc` and pick these up automatically.

---

## What you'll do in this chapter

1. Pin three tarballs (`gmp-6.2.1`, `mpfr-4.1.0`,
   `mpc-1.2.1`) by sha256 in
   `scripts/fetch_gcc_prereqs.sh`.
2. Add the `gcc-osdev-prereqs` Makefile target that
   fetches them, verifies the hashes, extracts under
   `vendor/`, and symlinks each into
   `vendor/gcc-14.2.0/{gmp,mpfr,mpc}`.
3. Wire `$(GCC_PREREQS_MARKER)` as a prereq of `all` /
   `run` / `run-graphical` so a fresh clone picks the
   tarballs up implicitly.
4. Add the three extracted directories and the tarballs
   to `.gitignore` (the pins in the fetch script are the
   source of truth).
5. Verify with `scripts/test_gcc_prereqs.py`.

---

## Why now

GCC does arbitrary-precision arithmetic in its front-end —
not because it cares about computing huge numbers
end-to-end, but because:

- **Constant folding.** When a source file writes
  `int x = (1.0 / 3.0) * 9.0;` GCC has to evaluate
  `1.0 / 3.0` and the multiply at compile time, with at
  least the precision of whatever the target's `double`
  is. Computing that in host `double` would give
  whatever the host machine's FPU does — and host and
  target machines don't always agree (subnormals, ties-to-
  even, transcendentals). Doing it in software with
  arbitrary precision via MPFR sidesteps all of that.
- **`__builtin_…` math.** `__builtin_sinl(M_PI_4)` at a
  constant gets the same compile-time treatment.
- **Diagnostics on integer overflow.** `int x = 1 << 50;`
  prints a diagnostic that shows the actual two's-
  complement value of the shift. GMP does the bignum work.
- **Complex constant folding.** `_Complex double` literals.
  MPC (built on MPFR built on GMP) does that.

The library stack is layered:

```
   MPC   ← complex arithmetic
    │
   MPFR  ← arbitrary-precision floats
    │
   GMP   ← arbitrary-precision ints and rationals
```

GCC 14's `configure` script refuses to proceed past
sanity-checks if any of the three is missing. There is no
"build a stripped-down gcc without them" option — they're
hard dependencies.

---

## Two ways to satisfy the dependency

**Option A: link against the host's system gmp/mpfr/mpc.**
On macOS that means `brew install gmp mpfr libmpc` and
passing `--with-gmp=/opt/homebrew --with-mpfr=…` to
configure. Pros: one-time setup, no extra source in the
tree. Cons: ABI skew (the host gmp's `mpz_t` struct
layout has to match what gcc was *compiled* against, not
what its source expects — a Homebrew upgrade can silently
break a rebuilt xgcc); also "you need to install these
three things first" is exactly the kind of out-of-band
prereq that breaks book reproducibility.

**Option B: vendor and build them as in-tree subdirs of
the gcc source.** When gcc's top-level configure sees
`gcc-<ver>/gmp`, `gcc-<ver>/mpfr`, and `gcc-<ver>/mpc`,
it treats them as part of the gcc build. `make all-gcc`
builds gmp first, then mpfr against the just-built gmp,
then mpc against both, then xgcc statically linked against
all three. No system search paths involved. Same compiler
builds all four projects, so ABI skew is impossible.

This chapter picks option B. It's also what GCC's own
`contrib/download_prerequisites` script does — that
script's whole job is to fetch the three tarballs and
extract them into the in-tree slots.

---

## Why not just run `contrib/download_prerequisites`

The upstream script works, but:

- It pulls whatever versions are currently in the upstream
  `prerequisites.sha512` file. Those *should* match what's
  in any given gcc release, but a maintainer rev could in
  principle shift them. This build wants exact
  byte-for-byte tarballs pinned.
- It always downloads gettext (~9 MiB) for NLS support,
  which the OSdev build disables with `--disable-nls`.
  Wasted bandwidth.
- It also downloads ISL by default (~3 MiB) for Graphite
  loop optimizations. The OSdev build doesn't use
  `-floop-*` flags, and ISL adds another two minutes to
  the xgcc build for zero benefit at this level of
  optimization.
- Chapter 132c's "does it build" smoke test is the
  ground-truth contract. Tying the prereq fetch to an
  upstream script means a fresh-clone test failure can
  point at upstream connectivity rather than the OSdev
  patch.

So: vendor the three required tarballs directly with
sha256 pins captured in
[scripts/fetch_gcc_prereqs.sh](../../../scripts/fetch_gcc_prereqs.sh).
The pins are cross-checked against
`vendor/gcc-14.2.0/contrib/prerequisites.sha512` (the
upstream sha512s for the same tarball bytes) — different
hash families, same bytes, so a bit-flip in either
column would be caught at sha256-check time.

| Package | Version | Source                                         |
| ------- | ------- | ---------------------------------------------- |
| gmp     | 6.2.1   | `gcc.gnu.org/pub/gcc/infrastructure/gmp-6.2.1.tar.bz2`  |
| mpfr    | 4.1.0   | `gcc.gnu.org/pub/gcc/infrastructure/mpfr-4.1.0.tar.bz2` |
| mpc     | 1.2.1   | `gcc.gnu.org/pub/gcc/infrastructure/mpc-1.2.1.tar.gz`   |

GCC's "infrastructure" mirror serves the exact versions
their build was tested against — different from the
projects' own mirrors, which often have newer point
releases that gcc 14.2 hasn't been tested against and
which sometimes break the build.

---

## How the symlinks work

After extraction the workspace looks like:

```
vendor/
├── gcc-14.2.0/
│   ├── gmp  -> ../gmp-6.2.1     (symlink)
│   ├── mpfr -> ../mpfr-4.1.0    (symlink)
│   ├── mpc  -> ../mpc-1.2.1     (symlink)
│   └── ... rest of gcc source
├── gmp-6.2.1/
├── mpfr-4.1.0/
└── mpc-1.2.1/
```

Use *relative* symlinks (`../gmp-6.2.1`) rather than
absolute ones so the whole `vendor/` tree stays movable —
zip it up, drop it on another machine, the symlinks still
resolve.

The three extracted directories live *outside*
`gcc-14.2.0/` rather than inside it for one practical
reason: `make clean-gcc-src` (chapter 132a) blows away
the gcc source tree but leaves the math libs alone, so a
subsequent re-fetch of the gcc source doesn't have to
re-extract them. `make clean-gcc-prereqs` is the explicit
"nuke the math libs too" target when needed.

---

## Why nothing is built yet

This chapter's contract is "the source bytes are on disk,
in the right place, with the right names." The actual
build happens in chapter 132c via gcc's top-level
`configure && make`, which detects the in-tree subdirs and
arranges its own dep graph. Pre-building the math libs at
this stage would mean:

- Picking host vs cross (you'd want host for xgcc, but
  cross-built copies are also wanted in-guest later —
  split the work or do it twice?).
- Re-implementing whatever flag-tuning gcc's top-level
  configure does on its own (some of it is non-obvious,
  e.g. `--disable-shared` for gmp because xgcc statically
  links it).
- Writing custom `.a` install rules.

132c will get all of that for free.

---

## Run it / Test it

- New: `scripts/test_gcc_prereqs.py` — host smoke test,
  not in `scripts/sweep.sh`. Verifies sha256 pins, symlink
  targets, and that each extracted dir has a `configure`
  script. Skips cleanly if `.prereqs-osdev` marker absent.
- Unchanged: `scripts/sweep.sh`. Still 61/61 from chapter
  131f — no guest-side code changed.

## What this unlocks

- New host build target: `make gcc-osdev-prereqs` (Makefile,
  in the chapter-132b block after the chapter-132a `clean-gcc-src`
  rule).
- New script: [scripts/fetch_gcc_prereqs.sh](../../../scripts/fetch_gcc_prereqs.sh)
  (idempotent fetch + sha256 verify + extract + symlink).
- New gitignore entries: `vendor/{gmp-6.2.1,mpfr-4.1.0,mpc-1.2.1}/`
  and the three tarballs. None of these are committed; the
  fetch script's sha256 pins are the source of truth.
- Wiring: `$(GCC_PREREQS_MARKER)` added as a prereq of
  `all` / `run` / `run-graphical`, alongside the
  chapter-132a `$(GCC_MARKER)`. A fresh clone running
  `make run-graphical` now picks up both gcc source and
  its math libs without any out-of-band scripts.
- No existing apps changed — host-only chapter.

## What's next

- **132c** — the actual cross-gcc build. Sibling build dir
  `build/gcc-build-host/`, configure with the OSdev triple
  and toolchain prefix, `make all-gcc -jN`. First attempt
  will fail (libgcc / target headers); iterate the same
  way chapter 131e cranked binutils.
- **132c follow-up** — extend
  `gcc/config/aarch64/aarch64-osdev.h` to override
  `LIB_SPEC` / `STARTFILE_SPEC` / `LINK_SPEC` /
  `STANDARD_INCLUDE_DIR` so the resulting xgcc defaults to
  the OSdev libc + crt0 + linker script. (132a deliberately
  left that file minimal.)
- **132d** — `/bin/gcc /tmp/hello.c -o /tmp/hello &&
  /tmp/hello`. End-to-end: a chapter-13 ELF loaded by a
  cross-compiled hello-world.


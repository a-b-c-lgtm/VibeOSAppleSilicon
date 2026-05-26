# Chapter 132a — GCC with an `aarch64-osdev` target

> **Status:** shipped. `make gcc-osdev-src` fetches
> gcc-14.2.0 from a sha256-pinned upstream tarball and
> applies the four-hunk additive patch. Host smoke test
> `scripts/test_gcc_target.py` passes (asserts `config.sub`
> canonicalises `aarch64-osdev` → `aarch64-unknown-osdev`
> and that the new arms are wired into `gcc/config.gcc`
> and `libgcc/config.host`).
> **Prereq:** chapter 131a (the binutils analogue of this
> chapter, same triple, same workflow, same patch shape);
> chapter 131f for the in-guest binutils that the eventual
> xgcc will dispatch to via `-B`.
> **Opens:** Phase 2 step 2 — bringing up a real
> aarch64-osdev cross compiler. This chapter does the
> source-tree definition only (one .h, two configure
> tables, one canonicaliser). Chapter 132b builds GMP /
> MPFR / MPC. Chapter 132c actually runs the cross-gcc
> build. Chapter 132d swaps `/bin/cc` to dispatch to it.

---

## What you'll do in this chapter

1. Pin gcc-14.2.0 in `scripts/fetch_gcc.sh` (sha256 +
   tarball URL) and add the `gcc-osdev-src` Makefile
   target that fetches and patches it.
2. Add `osdev*` to `config.sub`'s OS whitelist.
3. Add the `aarch64-*-osdev*` arm to `gcc/config.gcc`
   pointing at the existing `aarch64-elf` headers plus
   one new OS header.
4. Add the same arm to `libgcc/config.host`
   (verbatim copy of the `aarch64*-*-elf` `tmake_file`
   chain).
5. Add `gcc/config/aarch64/aarch64-osdev.h` defining
   `TARGET_OS_CPP_BUILTINS` (and only that — the spec
   overrides land in 132c).
6. Verify with `scripts/test_gcc_target.py` (sub-second
   host smoke).

---

## Why now

Chapter 131a took ~5 hours of human time and 90 lines of
patch to teach binutils-2.44 about `aarch64-osdev`. Most
of it was reading: working out which case statements take
`aarch64-elf` today, which ones key off the OS field, and
which ones don't care. The actual patch was short.

GCC is bigger. Its source tree has 80k+ files; its target
definitions live in `gcc/config/<cpu>/*.h` plus a per-CPU
case in `gcc/config.gcc` plus another case in
`libgcc/config.host`; some sub-libraries
(`libstdc++-v3/configure.host`, `libatomic/configure.tgt`)
each have their own per-OS lists. The temptation is to
patch all of them at once and call it chapter 132. The
problem with that is xgcc won't build without GMP / MPFR /
MPC — those are chapter 132b — so trying to fold
"recognise the triple" and "build the compiler" into a
single chapter means the chapter can't be declared done
until both land. That's a long, expensive integration
loop.

132a's contract is narrower and cheaper to verify:

> *The gcc-14.2.0 source tree, after the OSdev patch is
> applied, agrees that `aarch64-osdev` is a real target
> triple.*

That's it. No `configure`, no `make`, no compiler. The
smoke test runs `bash vendor/gcc-14.2.0/config.sub
aarch64-osdev` and grep-checks two config files. It takes
under a second. When it goes red, the failure is a patch
problem, not an autotools problem or a GMP problem or a
libgcc bootstrap problem.

That separation pays for itself in 132b/c/d, where the
errors get genuinely hard to read.

---

## The triple, again

Same one as chapter 131a: `aarch64-osdev`, canonicalised
by `config.sub` to `aarch64-unknown-osdev`. CPU is
`aarch64`, vendor is `unknown`, OS is `osdev`. Two reasons
worth restating:

1. **Predefined macros.** `aarch64-elf-gcc` defines
   `__ELF__` and that's about it. `aarch64-osdev-gcc`
   will define `__osdev__`, `__unix__`, `__osdev`, and
   assert `system=osdev / system=unix / system=posix` so
   portable third-party code (the kind chapter 134 will
   start porting — coreutils-lite, then DOOM) can
   `#ifdef __unix__` and find what it expects.

2. **Default search paths and specs.** This is what 131a
   *couldn't* fix on its own: binutils never reads a libc
   path or a crt0 name, so its `aarch64-elf` defaults are
   fine. GCC's are not — `aarch64-elf-gcc` ships with
   built-in `LINK_SPEC` / `STARTFILE_SPEC` / `LIB_SPEC`
   strings that name newlib, name a bare-metal crt0, and
   point at a `/usr/local/cross-gcc/...` prefix that
   doesn't exist on the guest. By the time 132c runs the
   cross compiler needs to default to `/bin/osdev.ld`,
   to the chapter-120 crt0, and to the chapter-128 libc.
   *This chapter doesn't override those specs yet* — see
   below.

---

## The four hunks

Whole patch: `vendor/gcc-aarch64-osdev.patch`. Same
discipline as the binutils patch: 100% additive. The new
arms are inserted as the *first* arm of each existing
`case ${target} in` / `case ${host} in` block, so the
surrounding text never moves. A future `gcc-14.3` or
`gcc-15` will almost certainly accept the same patch
without manual rebase.

### 1. `config.sub` — make the triple parse

Identical surgery to chapter 131a's binutils patch.
`config.sub` has a long whitelist of OS names; the hunk
inserts

```
	     | osdev* \
```

between the existing `| rtems* \` and `| midipix* \`
entries. After the hunk:

```
$ bash vendor/gcc-14.2.0/config.sub aarch64-osdev
aarch64-unknown-osdev
```

Without it `configure` exits 1 with `Invalid configuration
'aarch64-osdev'`.

(The binutils and gcc copies of `config.sub` come from the
same upstream `config` repo, so the hunks look textually
identical. They are two separate files on disk and need
two separate hunks.)

### 2. `gcc/config.gcc` — pick the target machine headers

`gcc/config.gcc` is the *front* of GCC's configure. For
every triple it sets `tm_file` (the chain of `.h` files
the compiler `#include`s to know what its target looks
like), `tmake_file` (extra Makefile fragments folded into
the build), and a handful of switches like
`use_gcc_stdint=wrap`.

The hunk inserts the new arm at the top of the existing
`case ${target} in` listing the aarch64 variants:

```
aarch64-*-osdev*)
	tm_file="${tm_file} elfos.h newlib-stdint.h"
	tm_file="${tm_file} aarch64/aarch64-elf.h aarch64/aarch64-errata.h aarch64/aarch64-elf-raw.h aarch64/aarch64-osdev.h"
	tmake_file="${tmake_file} aarch64/t-aarch64"
	use_gcc_stdint=wrap
	;;
```

The `tm_file` chain is intentionally the same as
`aarch64-*-elf` for the first three headers (elfos.h →
ELF-as-a-format conventions; newlib-stdint.h → the
`uint8_t` / `int_least32_t` definitions; aarch64-elf.h +
aarch64-errata.h + aarch64-elf-raw.h → AArch64 assembly,
errata workarounds, and the bare-metal startfile/link
specs). The *only* addition is the new
`aarch64/aarch64-osdev.h` at the end of the chain — and
since `tm_file` is processed in order, this header gets
to override anything the earlier ones defined. That's
the hook the next subsections rest on.

### 3. `gcc/config/aarch64/aarch64-osdev.h` — the OS-level overrides

This is the new file. It's deliberately tiny:

```c
#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()		\
  do {						\
    builtin_define ("__osdev__");		\
    builtin_define_std ("osdev");		\
    builtin_assert ("system=osdev");		\
    builtin_assert ("system=unix");		\
    builtin_assert ("system=posix");		\
  } while (0)
```

`TARGET_OS_CPP_BUILTINS` is the macro GCC uses to inject
target-OS-specific preprocessor symbols. The combination
above gives userspace code three ways to detect the
platform:

- `#ifdef __osdev__` — the strict-conformance spelling,
  always defined.
- `#ifdef osdev` — the relaxed spelling, defined when
  `-ansi` / `-std=c89` aren't in effect. (That's what
  `builtin_define_std` gives you — both the `__foo__` and
  the bare `foo` forms.)
- `#if defined(__unix__)` / `__has_include(<unistd.h>)`
  style guards — the three `system=` asserts are how GCC
  drives the `__unix__`, `__posix__` predefines.

What the file *doesn't* do is override `LIB_SPEC`,
`STARTFILE_SPEC`, `ENDFILE_SPEC`, or `LINK_SPEC`. Those
all currently come from `aarch64-elf-raw.h`, which says
roughly "link with the user-supplied crt0 and nothing
else, no libc". That's correct for chapter 132a's smoke
test (`config.sub`-canonicalises and the configure tables
accept the triple) and it's even correct for chapter 132b
(GMP/MPFR/MPC are host libraries, they don't go through
the cross compiler's specs). It's wrong for chapter 132c+
— but by the time xgcc runs end-to-end the libc layout
(chapter 132c's `STANDARD_INCLUDE_DIR` = `/include`) and
the crt0 path (chapter 132d's `STARTFILE_SPEC` = `crt0.o`
from `/lib/`) need to be known first, which means those
overrides land *after* the source tree agrees the triple
exists. Chapter 132c will add them.

### 4. `libgcc/config.host` — pick libgcc's per-target tmake files

`libgcc` is the runtime support library every GCC-compiled
program links. It's built by the cross compiler against
itself as part of the xgcc build (chapter 132c). Its
`configure` uses a separate per-target case in
`libgcc/config.host`:

```
aarch64*-*-osdev*)
	extra_parts="$extra_parts crtbegin.o crtend.o crti.o crtn.o"
	extra_parts="$extra_parts crtfastmath.o"
	tmake_file="${tmake_file} ${cpu_type}/t-aarch64"
	tmake_file="${tmake_file} ${cpu_type}/t-lse t-slibgcc-libgcc"
	tmake_file="${tmake_file} ${cpu_type}/t-softfp t-softfp t-crtfm"
	tmake_file="${tmake_file} t-dfprules"
	;;
```

The `extra_parts` line lists the startup-helper objects
libgcc itself produces (`crtbegin.o` / `crtend.o` /
`crti.o` / `crtn.o` — the ones that bracket every
GCC-built executable so that global ctors/dtors run and
the stack frame is properly torn down) and the
fast-math-init helper. The `tmake_file` chain pulls in
AArch64-specific atomic-load/store helpers (`t-lse`),
soft-float helpers (`t-softfp` — even though aarch64 has
hardware float, libgcc ships soft-float fallbacks for
half-precision and 128-bit float), and decimal-floating
rules (`t-dfprules` — required by C23, optional but free).

This is the exact list that `aarch64*-*-elf |
aarch64*-*-rtems*` uses. Copying it verbatim is the right
call: when 132c runs and something is missing, the
diagnosis will arrive the same way it does for the
existing aarch64-elf and aarch64-rtems ports.

---

## Run it / Test it

- New: `scripts/test_gcc_target.py` — host smoke test, not
  in `scripts/sweep.sh`. Skips cleanly when the source
  tree isn't fetched yet (no
  `vendor/gcc-14.2.0/.patched-osdev` marker). Otherwise it
  checks four things: `config.sub` canonicalises;
  `gcc/config.gcc` has the new arm and references the
  OSdev header; `libgcc/config.host` has the new arm;
  `aarch64-osdev.h` defines `__osdev__`. Sub-second.
- Unchanged: the rest of `scripts/sweep.sh`. This chapter
  ships zero guest-side code, so no kernel regression
  surface changes. The chapter-131f sweep results stay
  61/61.

## What this unlocks

- New host build target: `make gcc-osdev-src` (Makefile,
  just after the chapter 131a `clean-binutils` rule).
- New script: `scripts/fetch_gcc.sh` (idempotent fetch +
  sha256 verify + patch + sanity check).
- New patch: `vendor/gcc-aarch64-osdev.patch` (4 hunks,
  100% additive — same shape as the binutils patch).
- New header file added by patch:
  `gcc/config/aarch64/aarch64-osdev.h` (defines
  `TARGET_OS_CPP_BUILTINS` only — link / startfile / lib
  specs deferred to 132c).
- New gitignore entries: `vendor/gcc-14.2.0/`,
  `vendor/gcc-14.2.0.tar.xz` (the patch itself is
  committed; the source tree is regeneratable from the
  pinned sha256).
- No existing apps changed — host-only chapter.

## What's next

- **132b** — GMP / MPFR / MPC. GCC 14 needs these three as
  link-time dependencies for floating-point constant
  folding. Cross-build the libraries against the OSdev
  libc (so the eventual xgcc can run in-guest) and ship
  the `.a` files under `/lib/`. Patch shape will be much
  smaller — these projects don't have target-triple
  case tables, they just respect `--host`.
- **132c** — Run the GCC build. `configure
  --target=aarch64-osdev --prefix=…`, then `make
  all-gcc`. The first attempt will fail; iteration loop
  is the same as chapter 131e's binutils-in-guest crank
  (read the configure log, find the missing libc
  function, add it to chapter-128 cstring, retry).
- **132c-followup** — extend `aarch64-osdev.h` to override
  `STANDARD_INCLUDE_DIR` (= `/include`),
  `STARTFILE_SPEC` (= chapter-120 crt0), `LIB_SPEC`
  (= `-lc` against the chapter-128 libc), and `LINK_SPEC`
  (= use `/bin/osdev.ld`).
- **132d** — `/bin/gcc /tmp/hello.c -o /tmp/hello &&
  /tmp/hello`. End-to-end smoke: a real GCC, compiled by
  itself, building a real ELF that the chapter-13 ELF
  loader runs.


# Chapter 128 — PLAN: Real GCC on the OS, and a playable Doom

> **Status: PLAN ONLY.** This chapter is the design contract
> for Part XVIII. Implementation lands in chapters 128a–133f.
> The plan is written first because Part XVIII makes several
> load-bearing version and sequencing choices that are
> expensive to undo, and the reader (and the author) should be
> able to redirect early.

## The goal

The user-facing demo this section is built around is two
terminal sessions on the booted OS:

```
$ httpget https://github.com/ozkl/doomgeneric/...tar.gz > /tmp/doom.tgz
$ cd /tmp && tar -xzf doom.tgz && cd doomgeneric-master
$ make
$ ./doomgeneric -iwad doom1.wad
[title screen, menu, "New Game", playable]
```

Concretely Part XVIII must ship:

- `/bin/gcc` — a real, in-guest C compiler that handles a
  realistic C codebase end-to-end. **Not** the chapter-121
  `/bin/cc` (deliberately tiny) and **not** a re-invention.
- `/bin/as` and `/bin/ld` — real GNU binutils, replacing the
  chapter-118/119 toys for any input larger than the
  hand-written test programs.
- `/bin/tar` — read-mode tar/untar.
- libc surface large enough for real software (signals,
  setjmp, qsort, full printf/scanf, time, ctype, getopt).
- A working Doom binary that runs on the OS framebuffer
  with the chapter-30 input plumbing.

## Strategy: cross-build, do not bootstrap

There are two ways to put real GCC on the OS:

1. **Bootstrap.** Get GCC's source to compile through some
   existing in-guest compiler (the chapter-121 `/bin/cc`,
   after *enormous* growth, or a small bootstrapper like
   TinyCC). Then have that GCC binary re-compile its own
   source on the OS to a fixed point.

2. **Cross-build and ship.** Use the host's
   `aarch64-elf-gcc` to build a `aarch64-osdev-gcc`
   targeting this specific OS (target triple, syscall ABI,
   libc layout). Copy the resulting binary onto the OSFS at
   `/bin/gcc`. The binary then runs *on* the OS, opens C
   source files from the OS filesystem, invokes the on-OS
   `/bin/as` and `/bin/ld`, and emits ELFs the OS executes.

Part XVIII picks (2). Reasons:

- **The user-facing goal is to *use* GCC, not to bootstrap
  it.** Path (2) satisfies "download source, build with
  gcc, run the binary" with no asterisks.
- **The cross-toolchain machinery is already in place.**
  Chapter 122 wrote down the cross-toolchain contract every
  userspace binary already respects. Adding one more
  cross-built binary (`aarch64-osdev-gcc`) is the same
  pattern Part XV used to ship BearSSL (chapter 112a) and
  GCC-emitted `userspace/browser`.
- **(1) is multi-year work in disguise.** GCC 4.8 onward is
  in C++; pre-4.8 GCC (3.4.6, 4.7.4) has no aarch64
  back-end. There is no easy "C-only GCC for aarch64."
  Bringing up either path is a years-long project, not a
  Part-XVIII project.
- **Real self-host (stage-2 == stage-3) is committed as
  Part XIX**, the section that follows this one. Once
  `/bin/gcc` works, rebuilding GCC's own source on the OS
  becomes a meaningful exercise. Part XVIII deliberately
  stops at "GCC binary that compiles real programs on the
  OS" so the self-host work has a stable substrate to
  measure itself against.

This is exactly the [BearSSL](../15-browser-maturation/112a-bearssl-build.md)
playbook applied to a much larger codebase, with the
expected long tail of "the cross-built binary expects libc
things the on-disk libc doesn't provide yet."

## Version selection

The choices that fall out of "cross-build aarch64-osdev-gcc
from the host":

| Component | Version | Why |
| --- | --- | --- |
| GCC | **same as host toolchain** (currently `aarch64-elf-gcc 14.2.0`) | Matching versions means the cross-build is well-supported and the runtime ABI matches what the rest of the userspace already uses. |
| binutils | **same as host toolchain** (`aarch64-elf-binutils 2.42`-ish) | Same reasoning. |
| Doom port | **DoomGeneric** ([github.com/ozkl/doomgeneric](https://github.com/ozkl/doomgeneric)) | Single platform shim (`doomgeneric.c` + a per-platform glue file). ~200 lines of OS-side glue. |
| Doom WAD | **DOOM1.WAD** (id Software shareware release, freely redistributable) | Stage it in `/data/wads/` via `mkosfs2.py`. |

The "use the host toolchain version" rule is deliberate. If
the host `aarch64-elf-gcc` ever upgrades, Part XVIII's
work goes with it. Pinning to a different GCC version
(e.g. 11.4.0) would create a second cross-toolchain to
maintain.

## The blocker: FP/SIMD at EL0

Today every userspace binary is built `-mgeneral-regs-only`
and the kernel's context switch never touches `q0..q31`,
`fpsr`, `fpcr`, or `CPACR_EL1.FPEN`. That works because
nothing in the existing userspace uses floating-point or SIMD.

Real GCC's emitted code is different. The aarch64 PCS
treats `q0..q7` as argument-passing registers, AAPCS64
expects `q8..q15` as callee-saved, and most non-trivial C
programs (Doom included) use `float`/`double`. FP has to
be turned on before *any* of the cross-built tooling can
run without taking an undefined-instruction trap.

This is **chapter 129** and it must land before chapter
131 (cross-built GCC binary). Until 129 ships, Phase 1
(below) is the only thing the section can demonstrate.

## Sequencing: Phase 1 (Doom plays) before Phase 2 (GCC builds it)

The cheap risk-reduction move is to validate the platform
integration *before* sinking chapters into the toolchain
port:

- **Phase 1** — cross-build Doom on the host, ship it as
  `/bin/doom`, prove it runs. This isolates "does the
  framebuffer / input / file I/O / FP-at-EL0 work?" from
  "does the toolchain port work?". If Doom won't run when
  the *host* GCC produced the binary, Doom certainly
  won't run when the in-guest GCC produces it.

- **Phase 2** — port real GCC + binutils + tar, then
  rebuild Doom in-guest and confirm bit-for-bit (or at
  least behaviorally) the same result.

If Phase 1 surfaces a platform problem the plan didn't
predict (missing libc fn Doom calls, framebuffer pitch
confusion, input-event lossiness), Phase 2 doesn't go any
easier — fix it once in Phase 1 and Phase 2 inherits the
fix.

## Chapter sequence

Each chapter ships green code with a regression test, in
keeping with Part XVII's discipline.

### Sub-part A: Phase 1 — Doom plays (cross-built binary)

**128a. `<setjmp.h>` and aarch64 setjmp/longjmp** — 50-ish
lines of asm spilling callee-saved `x19..x30`, `sp`, `d8..d15`
(once FP is on — for now, integer subset). Required by
Doom's `I_Error` longjmp out of `D_DoomMain`. Pure libc;
no kernel change.

**128b. `<signal.h>` libc wrappers** — `signal()`,
`raise()`, default handlers for `SIGSEGV`/`SIGFPE`/`SIGINT`
that exit with a marker line. Wires onto the chapter-77
`sigaction` kernel surface. Doom installs a handler for
`SIGINT` to call `I_Quit`.

**128c. `<ctype.h>`, `<assert.h>`, missing string fns** —
`isalpha`/`isdigit`/`isalnum`/`isspace`/`tolower`/`toupper`,
`assert()` macro (wired to abort+serial), `strtok_r`,
`strdup`, `strndup`, `strcasecmp`, `strncasecmp`,
`memmem`. Pure libc, mechanical, ~200 lines.

**128d. `<time.h>` and `<sys/time.h>`** — `time()`,
`gettimeofday()`, `clock()`, `localtime()`. Uses chapter-95
RTC. Doom needs `gettimeofday` for its tick clock.

**128e. `qsort`, `bsearch`, `strtol`, `strtoul`, `strtod`,
`getopt`** — mechanical libc additions. `getopt` is what
Doom uses to parse `-iwad`, `-warp`, etc.

**128f. Real printf/scanf** — `%d`/`%u`/`%x`/`%o`/`%s`/`%c`/`%p`
+ width/precision/flags/`%lld`/`%f` for printf; `%d`/`%s`
for scanf. Replaces chapter-19 minimal printf. Touches every
binary in `userspace/` (recompile only, no source changes).

**129. FP/SIMD at EL0** — kernel change. Set
`CPACR_EL1.FPEN=0b11` at boot. Extend
`struct trap_frame` (or a parallel `struct fp_frame`) with
`q0..q31` (32×16=512 bytes) + `fpsr` + `fpcr`. Lazy-save
strategy: trap on first FP use per thread, allocate the FP
slot, set FPEN; from then on save/restore on every context
switch. Drop `-mgeneral-regs-only` from `USER_CFLAGS`.
**This is a Part-XVIII landmark.** Regression: a userspace
program that returns `(int)(3.14f * 2.0f)` and the integer
sweep stays green.

**130a. Doom-on-host cross-build** — vendor DoomGeneric
under `vendor/doomgeneric/`, write the `aarch64-osdev`
platform shim (`doomgeneric_osdev.c`, ~200 lines:
`DG_Init` opens the framebuffer via libgui, `DG_DrawFrame`
blits the 320×200 indexed surface up to native depth,
`DG_GetKey` reads from the chapter-30 input plumbing,
`DG_SleepMs` calls `sleep`, `DG_GetTicksMs` reads
gettimeofday). Cross-build into `build/userspace/doom`.

**130b. DOOM1.WAD on disk** — extend `mkosfs2.py` to stage
the shareware WAD under `/data/wads/doom1.wad` at image-build
time. (Download via httpget on first boot is a Phase-2-ish
upgrade — for now the WAD ships baked into the image.)

**130c. Doom plays** — boot to desktop, terminal,
`doom -iwad /data/wads/doom1.wad`. Title screen renders.
New game starts. Player moves with WASD/arrows. Pistol
fires. Smoke test ([`scripts/test_doom_plays.py`](../../../scripts/test_doom_plays.py))
boots the OS, drives input, screenshots the title screen,
asserts a known-pixel signature on the title-screen demon.

> **Shipped**: chapter 130c landed with the WAD-load + render
> half automated (the `nonblack_pct` title-region check) and
> the menu/input/motion half kept as a documented manual
> smoke procedure. QMP-injected key events don't satisfy the
> wsd/kernel-WM focus-shadow handshake on first window
> creation; the right fix is a focus-bus extension scheduled
> for Part XIX. Until then, `scripts/test_doom_plays.py`
> catches every regression a *kernel-touching* chapter is
> likely to introduce; the manual procedure covers the
> input shim's gameplay-tier behaviour.

**End of Phase 1**: Doom plays. Platform integration is
proven. The OS is now demonstrably a real machine for at
least one piece of real software.

### Sub-part B: Phase 2 — Real GCC builds it

**131a. binutils target setup** — define the
`aarch64-osdev` target triple in binutils source (a new
`bfd/config.bfd` entry + `gas/configure.tgt` line + a
`ld/emulparams/aarch64osdev.sh`). Cross-build
`aarch64-osdev-as` and `aarch64-osdev-ld` on the host.

> **Shipped**: see
> [131a — Binutils with an `aarch64-osdev` target](131a-binutils-target.md).
> Implementation note: no `ld/emulparams/aarch64osdev.sh`
> was needed — reused the existing `aarch64elf` emulation
> because entry symbol, page size, and text base all agree.
> The patch turned out to be four additive hunks
> (`config.sub`, `bfd/config.bfd`, `gas/configure.tgt`,
> `ld/configure.tgt`) at
> `vendor/binutils-aarch64-osdev.patch`. `make
> binutils-osdev` builds the toolchain into
> `build/toolchain/bin/`; host smoke test
> `scripts/test_binutils_target.py` verifies.

**131b. binutils as in-guest libc shim** — the cross-built
`aarch64-osdev-as` will hit libc functions the on-disk
libc doesn't provide (the long tail). Iterate: run, observe
missing symbol, add to libc, repeat. This chapter (and the
two that follow) is the bulk of the real work in Phase 2.

> **Shipped (re-scoped).** The original 131b framing —
> "cross-build binutils as for the guest, iterate on
> libc gaps" — depends on having a target compiler that
> can be handed to binutils's autoconf with
> `--host=aarch64-osdev`. None exists until
> chapter 132. So 131b was split:
>
> - **131b (delivered):** build the seam itself —
>   `aarch64-osdev-cc`, a 30-line wrapper around
>   `aarch64-elf-gcc` that adds
>   `-B build/toolchain/bin/` (for the chapter 131a
>   binutils) and `-isystem userspace/libc` (for the
>   in-tree libc). Smoke test: byte-identical
>   `hello.stripped.elf` via the wrapper, and
>   byte-identical link via `aarch64-osdev-ld`
>   directly. See `131b-osdev-cc-wrapper.md`.
> - **131c (delivered):** taught the wrapper a link mode
>   that auto-injects `crt0.o` and
>   `-T linker_user.ld` so autoconf's link tests stop
>   hitting `GCC_NO_EXECUTABLES`; default `-ffreestanding`
>   so gcc-internal `stdint.h` stops looking for newlib.
>   Top-level `binutils-2.44 configure` now runs cleanly
>   under `--host=aarch64-osdev`; first batch of
>   libiberty files compiles (`alloca.o`, `argv.o`,
>   `bsearch_r.o`, `cplus-dem.o`, `regex.o`). Trivial
>   libc gaps closed: `abort` reachable from
>   `<stdlib.h>`, `sprintf` / `vsprintf` added. Full
>   libiberty gap catalogue (Class A static-inline-vs-
>   replace, Class B missing functions, Class C struct
>   stat fields, Class D `open` variadic) collected and
>   handed off to 131d. See
>   `131c-cross-build-seam.md`.
> - **131d (delivered):** *not* the libc.a extraction
>   originally planned. The investigation surfaced a
>   smaller, more surgical path that closes the same
>   gap and defers the libc.a refactor for the chapter
>   that earns it. Three pieces:
>
>   1. **CONFIG_SITE cache**
>      ([`scripts/aarch64-osdev-configure.cache`](../../../scripts/aarch64-osdev-configure.cache),
>      ~95 lines, pure `ac_cv_func_*` /
>      `ac_cv_have_decl_*` entries only). Pre-populates
>      cache so autoconf's link probes never run and
>      libiberty's replacement files
>      (`vfprintf.c`, `strerror.c`, `vsnprintf.c`,
>      `gettimeofday.c`, …) never get compiled. Traps
>      caught: never include `ac_cv_env_*` /
>      `ac_cv_prog_*` (env-handshake aborts on any
>      whitespace drift); never pass via
>      `--cache-file=` (writes back at end of run,
>      destroying the curated file).
>   2. **One-hunk libiberty patch** for `getopt.c`:
>      adds `# define ELIDE_CODE` under
>      `#if defined(__OSDEV_LIBC__)`, reusing the
>      existing GLIBC-elision wrapper. `getopt.c` is in
>      `REQUIRED_OFILES` (Makefile.in unconditional)
>      and cache can't suppress it.  `getopt1.c` is
>      *intentionally* not patched — `getopt_long`
>      survives for binutils-the-tools in 131e.
>      `__OSDEV_LIBC__` is defined unconditionally by
>      the wrapper.
>   3. **Six targeted libc fixes** (no archive
>      extraction): `open()` becomes variadic;
>      `gettimeofday()` becomes 2-arg (6 in-tree call
>      sites updated); `strerror()` returns `char *`;
>      `struct stat` (and `struct kstat`,
>      `struct __kstat_raw`) gain `st_dev` / `st_ino`
>      (kernel-side fill helper plumbed into
>      `vfs_stat_path` x4); and additions of `sleep`,
>      `_exit`, `freopen`, `mktemp`, `link`, `execvp`,
>      `ldexp`, `frexp`.
>
>   Result: `build/binutils-build-guest/libiberty/libiberty.a`
>   = 917,890 bytes. Host smoke
>   `scripts/test_guest_configure.py` passes; chapter
>   131b byte-identity baseline still passes (wrapper's
>   new `-D__OSDEV_LIBC__` is invisible to ordinary
>   compiles). The libc.a extraction is deferred —
>   nothing in the rest of Phase 2 needs it yet. See
>   `131d-libc-gaps.md`.
>
> Net effect on the plan: chapter 131 is now a 5-chapter
> sub-series (a, b, c, d, e) instead of 4. Old "131c
> binutils ld in-guest" → 131e; old "131d replace
> /bin/as and /bin/ld" → 131f. No change in milestones.

**131e (delivered).** Cross-build binutils' `ld` for
> `aarch64-osdev`. Five companion archives configure +
> build under the chapter-131c wrapper (`libiberty` was
> 131d; `libsframe`, `bfd`, `opcodes`, `libctf` join here);
> `ld/ld-new` comes out as a 3,206,056-byte AArch64 ELF.
> Three trap doors on the way:
>   1. **Vendor `extern malloc` / `extern free` /
>      `extern strcmp` unresolved at link.** The in-tree
>      libc keeps these as `static inline` (no external
>      symbols, by design). Fix: extend
>      `userspace/libc/cstring.c` (which already shipped
>      `strdup` for the chapter 130a Doom port) with a
>      self-contained K&R first-fit allocator + 8
>      string functions + abort/exit, all published
>      under POSIX names via `__asm__("name")` per-
>      function renames. 13 new strong-extern symbols.
>      strdup migrates from malloc.h's per-TU heap to
>      cstring.o's own heap (via forward-declared
>      `extern malloc`) so vendor archives and Doom
>      end up on the same heap.
>   2. **libtool's `libdep.la` rejects non-libtool
>      objects.** binutils 2.44's `ld/Makefile.am` has
>      `bfdplugin_LTLIBRARIES = libdep.la`
>      unconditionally; no `--disable-libdep` exists.
>      Adding cstring.o via `make LIBS=...` leaks into
>      libtool's `--mode=link` and gets refused. Fix:
>      surgical Makefile post-process inject of
>      `cstring.o` into the `ld_new_LDADD =` line in
>      `ld/Makefile` only — never touches libdep's link.
>   3. **The 131d `ELIDE_CODE` patch on
>      `libiberty/getopt.c` left `_getopt_internal`
>      undefined for ld-new.** `getopt1.c` (intentionally
>      unpatched in 131d for `getopt_long`'s sake) calls
>      `_getopt_internal`. Fix: replace ELIDE_CODE with
>      a single `#define OSDEV_LIBC_NO_GETOPT` at the
>      top of `getopt.c` and wrap the in-tree stdlib.h
>      getopt block in `#ifndef OSDEV_LIBC_NO_GETOPT`.
>      Every other TU still sees the static getopt; this
>      one TU sees libiberty's externs.
> Eight smaller libc additions came along for the ride:
> `locale.h` (NEW, `setlocale` stub), `sys/param.h`
> (NEW, `MAXPATHLEN`), `getuid` / `getgid` / `geteuid` /
> `getegid` in `unistd.h`, `umask` / `chmod` in
> `sys/stat.h`, `tmpfile` in `stdio.h`, `gzwrite` stub +
> `gzFile` typedef in `zlib.h`, the
> `OSDEV_LIBC_NO_GLOBAL_DEFS` guard from chapter 130a
> applied to `atexit.h`'s `__cxa_finalize` and `env.h`'s
> `environ` (suppresses per-TU emission across vendor
> archives; cstring.o provides a weak `environ` slot for
> lexsup.c's extern; crt0.S's pre-existing weak no-op
> satisfies vendor `__cxa_finalize` calls), and `string.h`
> picks up `#include "errno.h"` so vendor TUs that
> `#include <string.h>` alone see `strerror`'s prototype.
> The test script also picks up `--without-zstd` (no
> libzstd for `aarch64-osdev`), `-DNDEBUG` (silences
> `__assert_fail` references in bfd),
> `-DOSDEV_LIBC_NO_GLOBAL_DEFS` (activates the guard),
> and the `ZLIB = -lz` Makefile post-process (no
> `libz.a` for aarch64-osdev either).
>
> Host smoke
> [scripts/test_guest_ld.py](131e-binutils-ld-in-guest.md)
> passes; chapter 131d's
> `scripts/test_guest_configure.py` baseline still
> passes (libiberty.a now 949,790 bytes — modest growth
> from struct stat's chapter-131d `st_dev`/`st_ino`
> fields rippling into a couple of libiberty TU sizes).
> See `131e-binutils-ld-in-guest.md`.

**131f. Replace `/bin/as` and `/bin/ld`** *(shipped, alongside 131e)* —
the OSFS entries `/bin/as` and `/bin/ld` now point at
`build/userspace/binutils/{as,ld}.stripped.elf` (the
binutils 2.44 `as-new`/`ld-new` built by 131e, stripped to
~3 MB each).  The toy chapter-118/119 sources under
`userspace/as/` and `userspace/ld/` are no longer
packaged. `/bin/ar` is still the chapter-119 toy because
nothing in the rest of Phase 2 needs the real `ar` yet
(the GCC build doesn't produce static libraries in-guest;
the libosdevc.a archive ships pre-built). Regression
tests: `scripts/test_bin_as.py` PASS 8/8;
`scripts/test_bin_ld_ar.py` PASS 12/12. The toolchain
contract from chapter 122 is honoured.

**132a. GCC target setup** — define `aarch64-osdev` as a
target in GCC's source tree
(`gcc/config.gcc`, `gcc/config/aarch64/osdev.h`,
`gcc/config/aarch64/osdev.c`). The OS-support header names
the crt0 path, syscall numbers (well, just `_start` and let
libc do syscalls), default linker script, and default
include path (`/include/`).

**132b. GMP/MPFR/MPC** — GCC 14 (= the host version) needs
these as link-time deps for floating-point constant
folding. Cross-build them too. Ship the `.a` files
alongside the GCC binary in `/lib/`.

**132c. Cross-build `aarch64-osdev-gcc`** — run the GCC
build on the host targeting `aarch64-osdev`. The resulting
`xgcc` is what becomes `/bin/gcc` on the OSFS.

**132d. `/bin/gcc` runs hello world** — `gcc /tmp/hello.c -o
/tmp/hello && /tmp/hello`. End-to-end smoke. Likely fails
on first attempt; iterate on the libc gaps the same way
binutils did.

> **Plan retarget — May 2026.** The original 132d shipped
> as *Real cross-compiler specs: retiring the wrapper* (the
> end-to-end smoke that was supposed to be 132d ran first
> because the spec investigation had to happen anyway).
> The originally-planned 132e (*medium real program*) is
> reslotted as **132h**; chapter **132e now ships
> as Phase 1 of guest-side gcc** — cross-building
> gmp/mpfr/mpc under `--host=aarch64-osdev` so the
> subsequent guest gcc has its link-time prereqs.
> Chapter **132f shipped** as the end-to-end hello-world
> smoke (`/bin/gcc` running inside the guest, synthetic
> `-nostdlib -e _start` link).  Chapter **132g shipped**
> the default-specs link (`gcc hello.c -o hello` with no
> escape-hatch flags).  See chapters 132e, 132f, and 132g
> for full details.

**132e. GMP/MPFR/MPC for the guest sysroot** *(shipped)* —
cross-build the three math libraries under
`--host=aarch64-osdev` so guest-side gcc has its
arithmetic-precision link inputs.  Three vendor patches
(one-line config.sub additions per package) + idempotent
patch-apply loop in `scripts/fetch_gcc_prereqs.sh`.  Phase
2 (xgcc/cc1/cpp guest cross-build) follows.

**132f. `/bin/gcc` works on the OS** *(shipped)* —
the first end-to-end use of guest-side `/bin/gcc`: a
trivial `hello.c` compiled, assembled, linked, and run
inside the OS, with `exit(42)` coming back through the
kernel reaper.  Driven by `scripts/test_gcc_hello.py`
(7-step ladder).  Uncovered five bugs (lrealpath stub
that frees argv[0]; two too-small `MAX_*_ARGV` caps;
a 16 KiB stack frame in `_env_init`; a stale-cc1 rebuild
gotcha; and the sysroot-include red herring) — see the
chapter for the inline-svc bisection technique.

**132g. `gcc hello.c -o hello` with default specs** *(shipped)*
— `int main(void) { return 7; }` compiled, linked, and run
inside the OS using the bare command `/bin/gcc /tmp/hello2.c
-o /tmp/hello2` (no `-nostdlib -nostdinc -e _start` escape
hatch).  The fix was one line in LINK_SPEC: GCC's `-B/bin/`
adds `/bin` to startfile-prefix and exec-prefix lists but
NOT to ld's `-L` library path, so `--start-group -losdevc`
needed an explicit `-L /bin` baked into the spec.  Test
ladder is now PASS 10 / FAIL 0; see chapter 132g for the
diagnostic probe (`scripts/_dbg_gcc_libc_probe.py`) and
the `gencheck.o` rebuild gotcha that nearly ate the
chapter.

**132h. `/bin/gcc` builds a medium-sized real program**
*(shipped)* — `userspace/bf/bf.c` (~210 LOC, brainfuck
interpreter) is shipped on the OSFS image alongside the
host-built `/bin/bf` binary.  The in-guest GCC compiles
`bf.c` with the bare command `/bin/gcc /bin/bf.c -o
/tmp/bf2` (no flags), and `/tmp/bf2 /bin/hello.bf`
produces byte-identical "Hello World!" output to the
host build.  bf is freestanding-by-design (no `#include`,
forward-declares its libosdevc.a symbols inline) because
the in-guest gcc has no system include directory — the
gccw shim only prepends `-B/bin/` for library lookup.
Shipping the 43-file `userspace/libc/*.h` set would
exceed OSFS-1's 128-file cap and is deferred to a
dedicated kernel-ABI chapter.  Test:
`scripts/test_gcc_bf.py` (PASS 6/6).  See chapter 132h
for the header-shipping discussion and the host/guest
link-line symmetry technique.

**132i. `#include <stdio.h>` works in the guest**
*(shipped)* — OSFS-1's directory cap doubled (128 -> 256;
9 new directory sectors).  24 user-facing libc headers
from `userspace/libc/*.h` are shipped onto `/bin`, plus
16 of GCC's own freestanding headers (`stdint.h`,
`stddef.h`, `stdarg.h`, and friends) from
`build/gcc-build-guest/gcc/gcc/include/`.  The gccw shim
now injects `-isystem /bin` in addition to `-B/bin/`,
since `-B` does not extend cpp's `<>` search path.
End-to-end test: `assets/osfs/stdio_test.c` (a real
`#include <stdio.h>` program with `printf("hello from
%s, answer=%d\n", ...)`, `puts`, and an exit code) is
compiled in-guest with the bare command
`/bin/gcc /bin/stdio_test.c -o /tmp/stdio_test`, runs,
and prints the expected lines.  `unistd.h` is the only
common header NOT shipped, because it `#include`s
`<sys/stat.h>` and OSFS-1 has no subdirectory support
yet (see 132j below).  Test:
`scripts/test_gcc_stdio.py` (PASS 7/7); chapters 132g
and 132h regressions both still green.  See chapter
132i for the directory-cap math, the "echo-staging
breaks on `<>`" trap, and the GCC-freestanding-headers
discovery.

**132j. `sys/` headers without a hierarchical FS**
*(shipped)* — kept OSFS-1's flat directory and shipped
seven new dirents literally named `sys/stat.h`,
`sys/types.h`, `sys/time.h`, `sys/times.h`, `sys/wait.h`,
`sys/param.h`, and `unistd.h`.  The kernel's path
resolver byte-matches them exactly without ever
interpreting `/` as a path separator.  Relative
`#include "../foo.h"` directives are rewritten to
`#include <foo.h>` by `scripts/stage_libc_headers.py`
during the disk recipe, so the host build is untouched
and `-isystem /bin` is enough at runtime.  Test:
`scripts/test_gcc_sys_stat.py` (PASS 6/6); all four
earlier gcc tests + the two binutils tests stay green.
The full subdirectory implementation is deferred to a
future `OSFS-2`.

### Sub-part C: the rest of the build chain

**133a. `/bin/tar`** *(shipped)* — read-mode ustar reader
(`tar tf` + `tar xf [-C dir]`), ~380 LoC in
`userspace/tar/tar.c`. Gzip was skipped entirely: the
tarball is produced at build time by `scripts/mktar.py`
(deterministic ustar via Python `tarfile`), so
compression buys nothing and avoids vendoring inflate.
The host script also bundles `vendor/doomgeneric/src/`
into `/bin/doomgeneric.tar` (1.9 MB, 203 entries) so the
in-guest extraction has something to chew on.
`scripts/test_tar.py` is **PASS 8/8** and four earlier
chapters' regressions stay green
(`test_gcc_sys_stat.py` 6/0, `test_gcc_hello.py` 10/0,
`test_gcc_bf.py` 6/0, `test_gcc_stdio.py` 7/0). Full
write-up in
[133a — `/bin/tar` (ustar reader)](133a-tar.md).

**133b. `/bin/make` audit against Doom's real Makefile**
*(shipped)* — chapter 126's 351-LoC `/bin/make` handled
only `target: deps + recipe`. 133b expands it to ~720 LoC,
adding `VAR = value`, `$(VAR)` / `${VAR}` / `$$`
expansion, the automatic vars `$@` / `$<` / `$^`,
`%.o: %.c` pattern rules, `.PHONY:`, `@` (silent) and `-`
(ignore-error) recipe prefixes, and line continuation.
Deferred: `:=` / `?=` / `+=`, `$(wildcard …)`,
`ifeq`, `include`, `-j` — none needed when a tailored
Makefile for chapter 133c is hand-written with an explicit
`OBJS = ...` list. `scripts/test_make_v2.py` is
**PASS 9/9**; chapter-126 `test_make_port.py` remains
**14/0**; six earlier regressions stay green. Full
write-up in
[133b — expanding `/bin/make`](133b-make-expansion.md).

### Sub-part D: end-to-end

**133c. Rebuild Doom in-guest, pilot** *(shipped)* —
proves the full toolchain chain
(`/bin/tar` → `/bin/make` → `/bin/gcc` → `/bin/cc1` /
`/bin/as` / `/bin/xgcc`) compiles real upstream
DoomGeneric sources from a tarball-on-disk down to ELF
AArch64 objects on `/data/src/`. Pilot scope: three small
files (`m_random.c`, `m_bbox.c`, `m_fixed.c`) chosen to
exercise zero-include, same-dir-include, and full-libc
include search respectively.
`scripts/test_doom_pilot.py` is **PASS 8/8**; seven
earlier-chapter regressions stay green (60/0 total).
Surfaced one kernel gap (`sys_spawn` does not propagate
parent's cwd to the child — worked around with absolute
paths in the pilot Makefile; queued for a separate
chapter) and one tooling gap (`/bin/ls` only honours
`argv[1]` — worked around in the test). Full write-up in
[133c — in-guest Doom rebuild pilot](133c-doom-pilot.md).

**133d. Rebuild Doom in-guest, full** *(shipped)* —
scaled OBJS from 3 to the canonical **77 files** (all of
`vendor/doomgeneric/src/*.c` minus `doomgeneric_xlib.c`
and the other host-backend variants). `/bin/make
-f /bin/doom_full.mk` runs end-to-end, producing 77 ELF
AArch64 objects on `/data/src/`. Zero compile or link
errors across 77 cc1 invocations (only one
`-Wpointer-to-int-cast` warning, an upstream quirk in
`p_maputl.c`). `scripts/test_doom_full.py` is **PASS 8/8**;
the eight-script regression sweep (68/0 total) stays green.
Surfaced two traps: (1) the chapter-133b `/bin/make`
buffer sizes (MK_MAX_VAL=512) truncated DoomGeneric's
~1500-char OBJS list — bumped to 4096; (2) the kernel's
`[sys_exit] thread '...' exited with code …` log line
booby-traps any test that greps for the bare substring
`exited with code` — fixed by anchoring on the `make:`
prefix in failure assertions. Full write-up in
[133d — in-guest Doom rebuild, full vendor compile](133d-full-doom-compile.md).
Still .o-only — link comes in 133e.

**133e. Link `doomgeneric.elf` in-guest** *(shipped)* —
bundled the osdev runtime (`crt0.o`, `doomgeneric_osdev.o`,
`setjmp.o`, `cstring.o`, `wmclient.o`) into
`/bin/libdoomrt.a` (198 KB) instead of shipping
`/bin/libgui.a` as originally planned — five members vs a
much wider library, with ld's normal symbol-driven
extraction picking exactly what Doom references. The 82
vendor-object paths would have overrun the chapter-91
`THREAD_ARGS_MAX=128` kernel cap, so the recipe uses
binutils' `@file` response-file feature (libiberty
`expandargv`, free from chapter-131f's real ld) rather
than bumping kernel limits: `/bin/ld -T /bin/osdev.ld
-o /data/doomgeneric.elf @/bin/doom_link.args
/bin/libdoomrt.a`. Pre-built `.o` files cross-built on
the host and shipped as `/bin/doomobjs.tar` (6.4 MB,
extracted to `/data/src/` by `/bin/tar`) so the link
regression test runs in ~12 s instead of redoing the
25-minute 133d compile. `scripts/test_doom_link.py` is
**PASS 11/0**, producing a 2,502,904-byte
`/data/doomgeneric.elf`. Surfaced five traps: (A) the
long-standing `[wmclient] DAMAGE failed status=-5`
chatter from background GUI apps under heavy disk I/O
DoS'd the serial channel — fixed via one-shot printf in
`wm_window_dirty()` benefiting every wmclient binary;
(B) OSFS-1's 19-byte filename cap forced
`doomgeneric_objs.tar` → `doomobjs.tar`; (C) `doom_full.mk`
ships 80 objects but the link needs 82 (`gusconf.o`,
`icon.o` were dropped during 133d) — fix to 133d's
fixture queued for 133f; (D) chapter-119's RWX-segment
linker script triggers a benign ld warning that matched
the test's `b"ld: "` failure check — narrowed to skip
`ld: warning:` lines; (E) the on-disk `/bin/wc` predates
option parsing so `-c` doesn't work — test parses the
bare four-column output instead. The nine-script in-guest
toolchain sweep is now 79/0. Full write-up in
[133e — in-guest Doom link](133e-link-doomgeneric-in-guest.md).

**133f. Rebuilt Doom plays** *(shipped — closes Part XVIII)* —
the in-guest-linked `/data/doomgeneric.elf` (chapter 133e)
runs and renders Doom's title screen on the wm framebuffer
on first attempt; the test passes without needing any
further OS or shim changes. Acceptance: `[doom] window
created` + `V_Init:` + 100% non-black title region (sample
matches chapter-130c's cascade math).
`scripts/test_doom_rebuilt_plays.py` is green; screenshot
saved to `/tmp/doom_rebuilt_title.ppm`. The test uses a
novel **chained** design: it `subprocess.run`s
`test_doom_link.py` as a precondition (Phase 1; produces
`/data/doomgeneric.elf` on data.img), then boots a fresh
QEMU and runs the rebuilt binary (Phase 2). Surfaced one
test-harness trap: inline tar+make+run in a single QEMU
boot tripped QEMU's `unix:server,nowait` serial transport
into closing the connection during a 3-second idle drain
after `/bin/tar` exit — root cause unclear but the
chained design sidesteps it entirely by giving each QEMU
instance exactly one logical task. Byte-equivalence to
the cross-built reference (chapter 130a) is NOT asserted
— the in-guest ld emits different DWARF / build-id /
section ordering than xgcc's link does; behavioural
equivalence is the spec. Full write-up in
[133f — the rebuilt Doom plays](133f-rebuilt-doom-plays.md).

The chapter-128 end-state — `tar` + `make` + `./doom`
end-to-end on the booted OS — is now reachable from a
fresh boot in three commands:

```
$ /bin/tar xf /bin/doomobjs.tar -C /data
$ /bin/make -f /bin/doom_link.mk
$ /data/doomgeneric.elf
```

Part XVIII closes here.

## Scope estimate

This is the longest section in the book so far. Roughly:

- Sub-part A (libc + FP + Phase 1 Doom): 8 chapters
- Sub-part B (binutils + GCC): 8 chapters
- Sub-part C (tar + make audit): 2 chapters
- Sub-part D (Phase 2 Doom): 2 chapters

Total: **~20 chapters**. Per-chapter scope is comparable to
the heavy Part XII TLS chapters: a focused chunk of
implementation + debugging + writeup each.

The two biggest unknowns:

1. **FP-at-EL0 (chapter 129)** has subtle interactions
   with the context switch (the SP_EL0 handling note in
   `/memories/repo/aarch64-sp-el0-context-switch.md`, the
   IRQ-window discipline, SMP). Could blow up into 3
   sub-chapters.
2. **The libc-gap long tail (chapters 131b, 132d)** is
   the kind of work that goes until it doesn't. The
   estimate assumes binutils and GCC mostly want POSIX
   shapes the in-tree libc already has. Each surprise
   (e.g. they want `mmap` semantics chapter-90 mmap
   doesn't quite honour) is a sub-chapter.

## What this section does NOT cover

- **Self-host of `/bin/gcc`** (stage-2 == stage-3 fixed
  point). Committed as **Part XIX** — the section that
  follows. Part XVIII deliberately stops at "GCC compiles
  real programs" so Part XIX has a stable substrate.
- **C++ in-guest.** Building `/bin/g++` and shipping
  libstdc++ is its own section. Doom is straight C.
- **Other large software ports** (Vim, Lua, SQLite). Each
  earns a chapter (or section) once it's wanted.
- **Networking-heavy software** (Git, SSH). The libc
  surface they need (OpenSSL, zlib, pthreads, …) is
  bigger than Doom's. Defer.
- **Floating-point-heavy compute** (numerical software,
  ML, audio synthesis). Chapter 129 turns FP *on*; it
  does not optimise it. Real perf work is a separate
  question.

## Decisions

These were the three load-bearing decisions in the plan.
Recorded here so future chapters can cite them rather
than relitigate them:

1. **Phase-1-first.** Cross-built Doom plays on the OS
   before sinking chapters into porting binutils + GCC.
   The cross-built binary is transient (replaced in
   chapter 133) but isolates platform-integration risk
   from toolchain-port risk.
2. **Track host GCC.** `aarch64-osdev-gcc` is built from
   the same GCC source tree the host's `aarch64-elf-gcc`
   uses (currently 14.2.0). Upgrading the host upgrades
   the in-guest one; the cost is that Part XVIII gets
   re-tested whenever the host toolchain moves.
3. **Self-host is Part XIX.** Part XVIII deliberately
   stops at "GCC compiles real programs on the OS."
   Rebuilding GCC's own source in-guest (stage-2 ==
   stage-3 fixed point) is the explicit goal of Part XIX
   and gets a stable substrate to measure itself against.

## What this unlocks for later parts

Once `/bin/gcc` ships:

- Part-XIX-class projects become *trivial* to scope:
  "pick a program, port it." The same chapter shape used
  for BearSSL.
- Notepad's chapter-127 Build button could shell out to
  `/bin/gcc` instead of `/bin/cc`, removing the language
  restriction on the in-OS dev loop.
- The book's reader can plausibly *use* the OS for a
  small programming task end-to-end.

## Status of this plan

```
status:        PLAN
prerequisites: none net-new; rides on
               chapter 90 (mmap), chapter 77 (sigaction),
               chapter 95 (RTC), chapter 122 (cross-toolchain
               contract).
next chapter:  128a (setjmp/longjmp)
```

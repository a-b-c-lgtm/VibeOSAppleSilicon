# Chapter 132c — Cross-building `aarch64-osdev-gcc`

> **Status:** shipped. `make gcc-osdev` builds the patched
> gcc-14.2.0 source against the in-tree gmp/mpfr/mpc and
> installs `build/toolchain/bin/aarch64-osdev-gcc`
> (alongside `aarch64-osdev-cpp` and friends) in roughly
> four minutes on a 12-core M-series Mac. Host smoke test
> `scripts/test_xgcc_build.py` passes: the binary exists,
> reports `-dumpmachine=aarch64-osdev` and
> `-dumpversion=14.2.0`, predefines `__osdev__`, and
> resolves `as` through the chapter-131a binutils prefix.
> Regression sweep still 61/61.
> **Prereq:** chapters 131a (binutils-osdev), 132a (patched
> gcc source), 132b (in-tree math libs).
> **Opens:** chapter 132d — turning the bare cross compiler
> into one that can compile and link real userspace
> programs against the OSdev libc + crt0, ending in
> `/bin/gcc /tmp/hello.c -o /tmp/hello && /tmp/hello`
> end-to-end.

---

## What you'll do in this chapter

1. Add the `gcc-osdev` Makefile target that runs the
   top-level `configure` against
   `vendor/gcc-14.2.0/configure` in a sibling build dir
   at `build/gcc-build-host/`.
2. Pick configure flags carefully — `--target`,
   `--prefix`, `--program-prefix`,
   `--enable-languages=c`, `--disable-bootstrap`,
   `--disable-multilib`, `--without-headers
   --with-newlib`, and the rest spelled out below.
3. Run `make all-gcc -jN` (stop short of libgcc and any
   target library).
4. `make install-gcc` into `$(TOOLCHAIN_PREFIX)`.
5. Verify with `scripts/test_xgcc_build.py` that the
   resulting binary reports the right triple, the right
   version, predefines `__osdev__`, and routes `as`
   through the chapter-131a binutils prefix.

---

## Why now

Chapters 132a and 132b put the patched source and the
in-tree math libraries in place. Everything is now
staged for the actual `configure && make` invocation
that produces a working cross compiler binary. This
chapter focuses on getting **just the compiler proper**
(cc1 + xgcc + cpp + driver shims) built and installed,
deliberately stopping short of libgcc and the target
libraries that would require target system headers —
those chickens-and-eggs belong in chapter 132d's
`STARTFILE_SPEC` / `LIB_SPEC` work.

---

## The shape of the build

GCC is a federation of build subdirectories. The top-level
`configure` script, handed the OSdev triple, walks the
source tree and figures out which subdirs apply to a
cross compiler build:

- `gmp/`, `mpfr/`, `mpc/` — host libraries, built with
  the system C compiler. These get static archives that the
  compiler proper links against. Chapter 132b put them in
  place via symlinks.
- `libiberty/`, `libcpp/`, `libbacktrace/`,
  `libdecnumber/`, `zlib/`, `intl/`, `libcody/`,
  `libsframe/`, `libctf/` — more host-side support
  libraries.
- `gcc/` — the compiler proper. Produces `cc1` (the C
  front-end and code generator), `xgcc` (the driver),
  `cpp` (the preprocessor driver), and a pile of helper
  binaries.
- `libgcc/`, `libatomic/`, `libssp/`, `libstdc++-v3/`,
  `libquadmath/` — *target* libraries. These need the
  cross compiler about to be built, plus target system
  headers. That's why chapter 132c stops at
  `make all-gcc`.

The path from "source on disk" to "compiler installed":

1. `configure` (top-level): produces a top-level Makefile
   that knows how to dispatch into each subdir's own
   sub-configure + sub-make.
2. `make all-gcc`: builds, in dependency order, gmp →
   mpfr → mpc → libiberty/libcpp/etc → `cc1` → `xgcc` →
   `cpp` + driver shims. Stops short of any target lib.
3. `make install-gcc`: copies the binaries into
   `--prefix=build/toolchain/`. Names get the configured
   `--program-prefix=aarch64-osdev-`.

The whole thing lives in a sibling build dir at
`build/gcc-build-host/` — *not* inside the gcc source tree.
GCC's build system has hard rules against in-tree builds
("error: source directory already configured; run "make
distclean" there first" if you try). The split also lets
`make clean-xgcc` rip out the entire build dir without
disturbing the patched source.

---

## Configure flags, one at a time

```make
../../$(GCC_SRC)/configure \
    --target=aarch64-osdev \
    --prefix=$(TOOLCHAIN_PREFIX) \
    --program-prefix=aarch64-osdev- \
    --enable-languages=c \
    --disable-bootstrap \
    --disable-multilib \
    --disable-nls --disable-shared \
    --without-headers --with-newlib \
    --disable-libssp --disable-libquadmath \
    --enable-checking=release \
    --with-system-zlib
```

**`--target=aarch64-osdev`** — the triple chapter 132a
taught `config.sub` and `config.gcc` about. Configure
canonicalises it to `aarch64-unknown-osdev` internally
but `gcc -dumpmachine` reports back the short form
passed in.

**`--prefix=$(TOOLCHAIN_PREFIX)`** — same install prefix
as the chapter-131a binutils. This is what wires gcc to
binutils: when gcc looks for `as` and `ld` at runtime it
searches `$prefix/$target/bin/` and `$prefix/bin/`
*before* `$PATH`, and that's exactly where chapter 131a
put `aarch64-osdev-as` and `aarch64-osdev-ld`. No
explicit `--with-as` flag needed.

**`--program-prefix=aarch64-osdev-`** — names binaries
`aarch64-osdev-gcc` / `aarch64-osdev-cpp` / etc. rather
than the bare `gcc` / `cpp`, so they don't shadow the host
compiler if `$prefix/bin` ends up on a developer's PATH.

**`--enable-languages=c`** — only C matters right now.
Building C++ doubles the compile time and pulls in
`libstdc++-v3` requirements; both are deferred.

**`--disable-bootstrap`** — gcc's default is a 3-stage
build: stage1 (host compiler), stage2 (stage1 compiler
recompiles itself), stage3 (stage2 recompiles itself,
verifies stage2 = stage3 byte-for-byte). That's a quality
gate for *native* compilers — a stage3 cross compiler is
identical to a stage1 cross compiler because both are
built with the host compiler. Skipping bootstrap on a
cross saves ~3x compile time for zero benefit.

**`--disable-multilib`** — only build aarch64 little-endian
LP64. No 32-bit ARM, no big-endian, no ILP32. Saves time
and disk; nothing in OSdev needs the alternatives.

**`--disable-nls --disable-shared`** — standard cross-tool
hygiene. No localised diagnostics (would require gettext);
static-link everything (no `.dylib`s in the toolchain
prefix to worry about loader paths for).

**`--without-headers --with-newlib`** — the magic
incantation that lets `configure` finish without target
libc headers. `--without-headers` says "the target libc
headers aren't present"; `--with-newlib` says "but
pretend they are for the purposes of generating libgcc's
Makefile" (newlib's headers come bundled with the libgcc
build, so configure doesn't complain). This chapter
doesn't actually *build* libgcc, but `make all-gcc` still
walks the configure step for it, and that step gets
unhappy without this pair.

**`--disable-libssp --disable-libquadmath`** — two target
libs that no OSdev workload needs. libssp is GCC's
stack-protector runtime (no `-fstack-protector` in the
OS); libquadmath is software 128-bit float, which
aarch64 doesn't even define a calling convention for.

**`--enable-checking=release`** — turn off all the
internal-consistency assertions inside cc1. ~20% faster
compile time at the cost of a few internal sanity checks
that aren't debuggable from outside anyway.

**`--with-system-zlib`** — link against the host's
libz rather than building gcc's bundled zlib. Saves a
minute of build time; the host always has libz on macOS
and Linux.

---

## Why the build stops at `make all-gcc`

There's a chicken-and-egg in the gcc build system that
fires on `make all` at this stage:

1. `make all` builds everything, including `libgcc.a`.
2. Building `libgcc.a` requires invoking the half-built
   `xgcc` to compile target code (`crtbeginS.o`,
   `crtendS.o`, division helpers, soft-float routines).
3. Invoking `xgcc` to compile target code makes it look
   for target headers — `stddef.h`, `stdint.h`, `limits.h`.
   GCC ships some of these itself (the "fixincluded"
   versions get generated during `all-gcc`), but for others
   it expects to find them under `$prefix/$target/include/`
   or via `STANDARD_INCLUDE_DIR` (which `aarch64-osdev.h`
   doesn't override yet).
4. With `--without-headers`, configure tells libgcc "the
   target has no headers; use gcc's own bundled `stddef.h`
   / `stdint.h`." That works for the *few* libgcc TUs
   that only need those. It does *not* work for the TUs
   that `#include <unistd.h>` or `<sys/types.h>`.
5. Build fails partway through libgcc with `fatal error:
   sys/types.h: No such file or directory`.

The clean way around it: don't try to build libgcc this
chapter. `make all-gcc` is gcc's blessed name for the
sub-target that builds *just* the compiler binaries and
stops. That's enough to satisfy chapter 132c's contract:
`aarch64-osdev-gcc -E foo.c` and `aarch64-osdev-gcc -S
foo.c` (preprocess and emit-assembly) work because they
need neither libgcc nor target headers. Compile-and-link
(`-o foo`) doesn't, but that's chapter 132d's contract,
not 132c's.

---

## How long it took

On a 12-core Apple Silicon Mac with `-j12`:

- Total: ~4 minutes wall time (18:33:01 → 18:37:06 in the
  build that wrote this chapter).
- Configure: ~1m20s. The bulk of this is the recursive
  sub-configures (each of the ~30 subprojects runs its own
  autoconf-generated `configure` to detect host
  capabilities).
- gmp + mpfr + mpc: ~1m, parallel with the start of the
  compiler proper.
- gcc proper (cc1/xgcc/cpp): ~2m.
- install-gcc: ~10s.

Disk footprint after the build:

```
build/gcc-build-host/   ≈ 2.5 GiB  (intermediate .o, .d, libtool)
build/toolchain/        ≈ 380 MiB  (binutils + xgcc installed)
```

A full xgcc rebuild after editing one source file under
`gcc/config/aarch64/` is ~30s of incremental work, not a
full 4 minutes. That'll matter in chapter 132c-follow-up
when iterating on the link specs.

---

## What the smoke test actually checks

[scripts/test_xgcc_build.py](../../../scripts/test_xgcc_build.py)
verifies the contract:

| Property                            | Source                              |
| ----------------------------------- | ----------------------------------- |
| Binary exists                       | `build/toolchain/bin/aarch64-osdev-gcc` |
| `-dumpmachine` = `aarch64-osdev`    | configure's `--target` setting      |
| `-dumpversion` = `14.2.0`           | the source tree pinned in 132a      |
| `__osdev__` / `osdev` predefined    | proves `TARGET_OS_CPP_BUILTINS` from `aarch64-osdev.h` fired during the build (chapter 132a's hook is now active) |
| `-print-prog-name=as` points into `build/toolchain/` | proves gcc found the OSdev binutils via the shared `--prefix` trick |

If any of those fail, chapter 132c's contract is broken
and the smoke test prints a specific diagnostic naming
which property went wrong. It skips cleanly (exit 0,
"SKIP" message) when xgcc hasn't been built yet — keeps
the test runnable on a fresh clone.

The smoke test is deliberately *not* in `scripts/sweep.sh`.
The sweep is the "boot the kernel and run the guest" gate;
xgcc is host-only this chapter. Adding it would mean every
sweep run waits for a non-existent xgcc check on machines
where the dev hasn't run `make gcc-osdev` yet.

---

## Why `gcc-osdev` is not a default `all` prereq

Chapter 132b wired the `$(GCC_PREREQS_MARKER)` into
`all` / `run` / `run-graphical` because fetching three
tarballs (~5 MiB total) is cheap and idempotent. The xgcc
build is *not* cheap (4 minutes, 3 GiB of intermediate
artefacts). Forcing every `make run` to first verify
xgcc would punish anyone who just wants to boot the
existing system into the desktop.

So `gcc-osdev` stays manual:

```bash
make gcc-osdev          # 4 minutes, leaves you with build/toolchain/bin/aarch64-osdev-gcc
make clean-xgcc         # tear down the build dir + installed binaries
```

Chapter 132d will introduce a second manual target,
`make gcc-osdev-full`, that depends on this one *and* the
target-libgcc build, so the "build everything" path is
also explicit. The default `all` flow remains "kernel +
disks + fetch-only-if-missing for gcc source/prereqs."

---

## Run it / Test it

- New: [scripts/test_xgcc_build.py](../../../scripts/test_xgcc_build.py)
  — host smoke. Not in `scripts/sweep.sh`. Skips when xgcc
  not built; PASS once `make gcc-osdev` has been run.
- Unchanged: `scripts/sweep.sh` (61/61). xgcc lives on the
  host; no guest-side code changed.
- Unchanged: chapters 131a (binutils-osdev), 132a
  (gcc_target), 132b (gcc_prereqs) smokes all still PASS
  after this chapter's build.

## What this unlocks

- New host build target: `make gcc-osdev` (Makefile, in
  the new chapter-132c block after `clean-gcc-prereqs`).
- New script: [scripts/test_xgcc_build.py](../../../scripts/test_xgcc_build.py).
- New Makefile vars: `GCC_BUILD_HOST`, `GCC_XGCC`. New
  `.PHONY` targets: `gcc-osdev`, `clean-xgcc`.
- No `.gitignore` change required —
  `build/` is already ignored.
- No existing apps changed — host-only chapter. The next
  chapter starts changing things.

## What's next

- **132c follow-up** — extend
  `gcc/config/aarch64/aarch64-osdev.h` with `LIB_SPEC`,
  `STARTFILE_SPEC`, `LINK_SPEC`, `STANDARD_INCLUDE_DIR`
  overrides so plain `aarch64-osdev-gcc hello.c -o hello`
  links against the OSdev libc archive, prepends the
  chapter-120 crt0, drives the linker with /bin/osdev.ld,
  and finds `<stdio.h>` / `<string.h>` / etc. via
  `-isystem`. This is where chapter 131b's
  `aarch64-osdev-cc` wrapper becomes obsolete: the real
  compiler now knows everything the wrapper was teaching
  the host gcc.
- **132d** — ship `aarch64-osdev-gcc` into the guest as
  `/bin/gcc` (via `assets/osfs/`), so the in-guest shell
  can compile `hello.c`. This is the chapter that closes
  Phase 2 of the chapter 128 plan.
- After 132d — Phase 3: native compile of doomgeneric
  inside the guest, ending in `/bin/doom` running from
  in-guest-compiled binaries.


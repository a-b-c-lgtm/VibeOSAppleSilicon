# Chapter 185 — Cross-building GMP / MPFR / MPC for the guest sysroot

> **Milestone in this chapter:** cross-build the three GNU
> multi-precision libraries gcc links against, this time under
> `--host=aarch64-osdev` so they live in the guest sysroot.
> **Code referenced:**
> - [scripts/fetch_gcc_prereqs.sh](../../../scripts/fetch_gcc_prereqs.sh)
>   (the `osdev` config.sub patches per library)
> - [Makefile](../../../Makefile) (guest sysroot install paths)
> - [scripts/test_guest_gcc.py](../../../scripts/test_guest_gcc.py)
>
> **At the end of this chapter** you will have `libgmp.a`,
> `libmpfr.a`, and `libmpc.a` (totalling ~3.4 MiB of AArch64
> ELF) installed under `$prefix/aarch64-osdev/lib/`, ready
> for the guest xgcc cross-build that follows. Prerequisites:
> chapter 179 (guest-side binutils ld pattern), chapters
> 181–184 (gcc host cross-compiler + sysroot).

---

## What you'll do in this chapter

1. Add a vendor patch to each of GMP, MPFR, and MPC that
   teaches their `config.sub` to accept the `osdev` OS
   suffix.
2. Extend `scripts/fetch_gcc_prereqs.sh` with an
   idempotent patch-apply loop that detects
   already-applied patches via dry-run reverse and writes
   per-package `.patched-osdev` markers.
3. Build the three libraries with
   `scripts/test_guest_gcc.py`, looping configure +
   `make -j` + `make install` per package so MPFR sees
   GMP installed and MPC sees both.
4. Add the canonical glibc-style `_STDIO_H` guard to
   `userspace/libc/stdio.h` so GMP's `inp_str.c` detects
   stdio inclusion and exposes its `FILE *` prototypes.
5. Refresh the sysroot with `make xgcc-sysroot` and
   re-run the smoke until it ends in **PASS (phase 1)**.

---

## Why now

Chapter 183 built `aarch64-osdev-gcc` on the **host**
(macOS arm64), using Homebrew's gmp/mpfr/mpc as link-time
inputs.  That gcc runs on the host and *emits* aarch64
ELF for the guest.  Chapter 185 is about the next layer
down: building a gcc that **runs on the guest**, so the OS
can compile its own programs without leaving the VM.

`--host=aarch64-osdev` means "configure for a compiler that
runs on the target."  Before configure can even start to
look at GCC itself, the three GMP-family archives have to
exist as AArch64 ELF under their own per-package install
trees so that the eventual GCC link can pull them in via
`--with-gmp=`, `--with-mpfr=`, `--with-mpc=`.

This is the same shape as chapter 179 (cross-build `ld`):
take a stock GNU tarball, wedge the OSdev cross-toolchain
into its autoconf-2.69 plumbing, work through every
"undefined reference" the link surface throws back.  Three
libraries means three configure runs, three Makefile
post-processes, and three "what did this older config.sub
ship without?" puzzles in a row.

---

## What this chapter adds, by the byte

```
build/gcc-build-guest/gmp/install/lib/libgmp.a   1,631,566 B
build/gcc-build-guest/mpfr/install/lib/libmpfr.a 1,485,748 B
build/gcc-build-guest/mpc/install/lib/libmpc.a     300,964 B
```

```
$ aarch64-elf-readelf -h build/gcc-build-guest/gmp/install/lib/libgmp.a \
     2>&1 | head -3
File: build/gcc-build-guest/gmp/install/lib/libgmp.a(assert.o)
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00
```

```
$ aarch64-elf-readelf -h ...assert.o | grep Machine
  Machine:    AArch64
```

All three are static archives, AArch64 little-endian
EABI, built with the chapter-176 wrapper and the
chapter-179 env (`-mcpu=cortex-a72 -DNDEBUG
-DOSDEV_LIBC_NO_GLOBAL_DEFS -DOSDEV_LIBC_NO_GETOPT`).

---

## The three vendor patches

```
vendor/gmp-aarch64-osdev.patch    52 lines (incl. header)
vendor/mpfr-aarch64-osdev.patch   44 lines
vendor/mpc-aarch64-osdev.patch    43 lines
```

Each one adds a single line to a `config.sub` so the OS
suffix `osdev` is no longer rejected with

```
Invalid configuration `aarch64-osdev':
    OS `osdev' not recognized
```

before any compiler flag is consulted.  The mirror of
this hunk in `vendor/gcc-aarch64-osdev.patch` (chapter
181) and `vendor/binutils-aarch64-osdev.patch` (chapter
175) is the original; these three are mechanical copies
into the three sibling tarballs.

### GMP — two config.subs, only one is the real one

GMP is the only one of the three that ships **two**
`config.sub` files:

```
vendor/gmp-6.2.1/config.sub        <- GMP's own wrapper
vendor/gmp-6.2.1/configfsf.sub     <- the stock FSF copy
```

The wrapper layer canonicalises GMP-only aliases (so that
e.g. `pentium4` rewrites to `i786`) and *then* delegates
to `configfsf.sub` for the actual OS recognition.  The
recognised-OS list lives in the FSF copy; the wrapper
never grew an `osdev` case.

`vendor/gmp-aarch64-osdev.patch` therefore lands in
`configfsf.sub`, not `config.sub`.  Forget that distinction
and the patch applies cleanly to the wrong file with no
hunk failure — and configure still rejects the host triple
with no obvious clue why.

Inserted line:

```diff
@@ -1709,6 +1709,7 @@
             | udi* | lites* | ieee* | go32* | aux* | hcos* \
             | chorusrdb* | cegcc* | glidix* \
             | cygwin* | msys* | pe* | moss* | proelf* | rtems* \
+            | osdev* \
             | midipix* | mingw32* | mingw64* | mint* \
             ...
```

Bare form, no leading `-`.  This is the
autoconf-2.69-style config.sub where the `-` prefix is
already stripped earlier in the case analysis.

### MPFR — older autotools, leading `-` required

MPFR 4.1.0 ships a noticeably older `config.sub` (no
sub-second version mentioned, dates to the same generation
that needs `-osdev*` with the leading dash):

```diff
@@ -1379,6 +1379,7 @@
              | -udi* | -eabi* | -lites* | -ieee* | -go32* | -aux* \
              | -chorusos* | -chorusrdb* | -cegcc* | -glidix* \
              | -cygwin* | -msys* | -pe* | -psos* | -moss* | -proelf* | -rtems* \
+             | -osdev* \
              | -midipix* | -mingw32* | -mingw64* | -linux-gnu* | -linux-android* \
              ...
```

Same canonical output (`aarch64-unknown-osdev`); just
different lexical form because the case analysis is one
stage earlier in this older copy.

### MPC — bare form, lives under `build-aux/`

MPC keeps its config.sub under `build-aux/`:

```diff
@@ -1354,6 +1354,7 @@
             | udi* | eabi* | lites* | ieee* | go32* | aux* | hcos* \
             | chorusrdb* | cegcc* | glidix* \
             | cygwin* | msys* | pe* | moss* | proelf* | rtems* \
+            | osdev* \
             | midipix* | mingw32* | mingw64* | linux-gnu* | linux-android* \
             ...
```

`patch -p1 -d vendor/mpc-1.2.1/ < vendor/mpc-aarch64-osdev.patch`
finds `build-aux/config.sub` because the diff body uses
`--- a/build-aux/config.sub` paths.

### The bookkeeping convention

[scripts/fetch_gcc_prereqs.sh](../../../scripts/fetch_gcc_prereqs.sh)
now has a step-3 patch-apply loop:

```bash
patch_path="vendor/${name}-aarch64-osdev.patch"
pkg_marker="$ext_path/.patched-osdev"
if [ -f "$patch_path" ] && [ ! -f "$pkg_marker" ]; then
    if patch --dry-run -R -s -f -p1 -d "$ext_path" < "$patch_path" \
            >/dev/null 2>&1; then
        echo "fetch_gcc_prereqs: $patch_path already applied"
        touch "$pkg_marker"
    else
        echo "fetch_gcc_prereqs: applying $patch_path"
        patch -p1 -d "$ext_path" < "$patch_path"
        touch "$pkg_marker"
    fi
fi
```

Two-step idempotency:

1. If the marker file `.patched-osdev` exists, skip.
2. Otherwise dry-run the patch *in reverse*.  If reverse
   succeeds, the patch is already in the tree (hand-edited
   during chapter bring-up); write the marker, move on.
3. Only if reverse-dry-run fails does the script apply
   the patch forward.

This matters because chapter 185 was developed by
editing the live tarballs by hand first, *then* extracting
patch files from the diffs.  A first-run-after-clone of
the repo, with empty tarballs, gets the forward path.
A re-run on the bring-up tree, with the live edits already
in, gets the "already applied" path.  Both end with the
marker file written and the rest of the script proceeding
unchanged.

---

## Pitfalls

### Pitfall — `mpfr: gmp install dir is not a valid directory`

**Symptom:** first run of the smoke (after fixing the
config.subs) fails inside MPFR's configure:

```
configure: error: gmp.h can't be found, or is unusable.
configure: error: gmp install dir is not a valid directory.
```

**Cause:** chapter 179's pattern for the binutils
sub-builds ran `configure` for every subdir in the loop,
but never ran `make` / `make install` between them.  Each
`configure --with-prereq=...` expects the prereq's install
tree to already contain `lib/lib<x>.a` and
`include/<x>.h`.  For binutils that worked because
libsframe / libiberty / bfd are all built *under one
top-level make tree* and configure cross-references via
in-tree paths (`../bfd`, `../libiberty`).

GMP / MPFR / MPC don't share a top-level tree.  They're
three independent autoconf packages.  MPFR's
`--with-gmp=$BUILD/gmp/install` checks at configure-time
that `install/lib/libgmp.la` (or `.a` fallback) exists,
and aborts if not.

**Fix.**  Extend the PHASE1 loop in
[scripts/test_guest_gcc.py](../../../scripts/test_guest_gcc.py)
to do `make -j` + `make install` *inside* the per-subdir
iteration, *before* moving on to the next subdir:

```python
for name, src, extra_args in PHASE1_SUBDIRS:
    # ... configure ...
    rb = run(["make", "-j" + str(os.cpu_count() or 4)],
             cwd=sd, env=env_sub)
    if rb.returncode != 0: fail(...)
    ri = run(["make", "install"], cwd=sd, env=env_sub)
    if ri.returncode != 0: fail(...)
```

Now `mpfr`'s configure sees a populated
`build/gcc-build-guest/gmp/install/{lib,include}/`, and
`mpc`'s configure sees both gmp and mpfr installed.

This collapses Phase 1 and the originally-planned Phase 2
(configure first, build after) into one pass.  Phase 1's
end message changes accordingly:

```
guest_gcc: PASS (phase 1) - gmp/mpfr/mpc configured,
           cross-built, and installed under
           --host=aarch64-osdev
```

### Pitfall — `gmp: __gmpz_inp_str_nowhite implicit declaration`

**Symptom:** after the install-loop fix, the gmp build
itself runs… and then:

```
mpz/inp_str.c:38:1: warning: implicit declaration of
        function '__gmpz_inp_str_nowhite' [-Wimplicit-function-declaration]
mpz/inp_str.c:38:1: error: type defaults to 'int' in
        declaration of '__gmpz_inp_str_nowhite'
        [-Werror=implicit-int]
```

**Cause:** `mpz/inp_str.c` is the `FILE *`-taking variant
of `mpz_inp_str`.  Its declaration sits inside an
`#if _GMP_H_HAVE_FILE` block in `gmp-impl.h`:

```c
#if _GMP_H_HAVE_FILE
size_t mpz_inp_str_nowhite (mpz_ptr, FILE *, int, ...);
#endif
```

That macro is set by `gmp.h` (generated from `gmp-h.in`)
according to this heuristic at line ~252:

```c
#if defined (FILE)              \
  || defined (H_STDIO)          \
  || defined (_H_STDIO)         \
  || defined (_STDIO_H)         \
  || defined (_STDIO_H_)        \
  || defined (__STDIO_H)        \
  || defined (__STDIO_H__)      \
  || defined (_STDIO_INCLUDED)  \
  || defined (__dj_include_stdio_h_) \
  || defined (_FILE_DEFINED)    \
  || defined (__STDIO__)        \
  || defined (_MSL_STDIO_H)     \
  || defined (_STDIO_H_INCLUDED) \
  || defined (_ISO_STDIO_ISO_H) \
  || defined (__STDIO_LOADED)   \
  || defined (_STDIO)
#define _GMP_H_HAVE_FILE 1
#endif
```

Translation: GMP probes for ~16 possible "stdio.h was
included" indicators.  If *any* of them is set, the
`FILE *` prototypes become visible.

The chapter-150 `userspace/libc/stdio.h` defined exactly
one guard:

```c
#ifndef USER_STDIO_H
#define USER_STDIO_H
```

`USER_STDIO_H` matches none of GMP's 16 probes.  Result:
GMP never sees a FILE * prototype declaration, the
`inp_str.c` TU includes `gmp-impl.h` without the
declaration, and the call site falls through to implicit
int — which `-Werror=implicit-int` (default since GCC 14)
upgrades to a hard error.

**Fix.**  One-line addition to `userspace/libc/stdio.h`,
right after the existing `USER_STDIO_H` guard:

```c
/* Chapter 185: also expose the canonical glibc-style guard so
 * third-party headers that probe `#ifdef _STDIO_H` to test
 * "stdio.h was included" detect this header correctly.
 */
#ifndef _STDIO_H
#define _STDIO_H 1
#endif
```

`_STDIO_H` is the glibc-canonical form; defining it is
the established way for third-party headers to know which
header is the system `stdio.h`.  No other code in the
OSdev tree checks for `_STDIO_H` (verified via grep), so
the extra macro is purely additive.  After the sysroot
refresh (`make xgcc-sysroot`), GMP's `inp_str.c` compiles
cleanly.

---

## What the script does, end to end

```python
PHASE1_SUBDIRS = [
    ("gmp",  GMP_SRC,  []),
    ("mpfr", MPFR_SRC, ["--with-gmp=" + install("gmp")]),
    ("mpc",  MPC_SRC,  ["--with-gmp=" + install("gmp"),
                        "--with-mpfr=" + install("mpfr")]),
]

env["PATH"]    = "$prefix/bin:" + PATH
env["CC"]      = "aarch64-osdev-cc"
env["CFLAGS"]  = "-mcpu=cortex-a72 -DNDEBUG "
                 "-DOSDEV_LIBC_NO_GLOBAL_DEFS "
                 "-DOSDEV_LIBC_NO_GETOPT"
env["CONFIG_SITE"] = ".../aarch64-osdev-configure.cache"

for name, src, extra_args in PHASE1_SUBDIRS:
    cwd = "build/gcc-build-guest/<name>/"
    configure --host=aarch64-osdev --disable-shared
              --disable-nls --prefix=cwd/install/  ...extra
    make -j$(nproc)
    make install
```

Per-subdir specialisations:

- **gmp:** `--disable-assembly ABI=64` — gmp ships
  hand-written `.asm` files per platform; the
  `aarch64-osdev` platform has none, and the C fallback
  is fine for what gcc actually exercises.
- **mpfr:** pre-set `ac_cv_func_tsearch=no` — the OSdev
  libc has `tsearch()` as a header-only `static inline`,
  but configure's link-probe runs the cross compiler and
  doesn't see it.  Set the cache value to bypass the
  probe.
- **mpc:** nothing extra; defaults are fine.

The shared env settings come from chapter 179:

- `-DNDEBUG` — `assert(...)` becomes a no-op, no
  `__assert_fail` extern.
- `-DOSDEV_LIBC_NO_GLOBAL_DEFS` — env.h's `environ`,
  atexit.h's `__cxa_finalize`, and other "one strong
  symbol per TU would multiply-define" globals are
  suppressed.  A single weak `environ` in `cstring.o`
  satisfies vendor externs.
- `-DOSDEV_LIBC_NO_GETOPT` — chapter 178 guard so each
  TU sees libiberty's `getopt` instead of the OSdev
  static-inline copy (avoids `_getopt_internal`
  collision).

---

## CONFIG_SITE: still the same cache file

[scripts/aarch64-osdev-configure.cache](../../../scripts/aarch64-osdev-configure.cache)
is the file `env CONFIG_SITE=` points configure at — a
shell script sourced before any probe runs.  Chapter 179
populated it with ~40 `${ac_cv_*=value}` entries; chapter
185 added zero new ones.  All three of gmp / mpfr / mpc
ride the existing cache.  No new probe was hit because
they don't try anything binutils didn't.

If a probe ever does need adding (e.g. mpfr's tsearch
above):

- Per-package overrides go in the test script's env, NOT
  the cache file.  The cache is for cross-cutting
  defaults; per-package quirks are noisy and would
  cross-contaminate the binutils tests.

The mpfr tsearch override illustrates the pattern:

```python
if name == "mpfr":
    env_sub = dict(env)
    env_sub["ac_cv_func_tsearch"] = "no"
else:
    env_sub = env
```

Scoped, one line, easy to delete if mpfr ever stops
running the probe.

---

## Phase 1 output tree

```
build/gcc-build-guest/
├── gmp/
│   ├── (configure scripts + object tree)
│   └── install/
│       ├── lib/libgmp.a       (1,631,566 B)
│       └── include/gmp.h
├── mpfr/
│   ├── ...
│   └── install/
│       ├── lib/libmpfr.a      (1,485,748 B)
│       └── include/mpfr.h
└── mpc/
    ├── ...
    └── install/
        ├── lib/libmpc.a       (300,964 B)
        └── include/mpc.h
```

Phase 2 (next chapter) will:

1. Configure the gcc top-level under
   `build/gcc-build-guest/gcc/` with
   `--with-gmp=`, `--with-mpfr=`, `--with-mpc=`
   pointing at the three `install/` trees above.
2. Run `make all-gcc` (NOT `make all` — that pulls in
   libgcc which needs target libc, libstdc++, etc.).
3. Verify `xgcc` / `cc1` / `cpp` emerge as AArch64 ELF.

The three archives are produced exactly once; the gcc
build re-runs many times during 185+ debugging without
touching them.  This is why the per-subdir install tree
lives under `gcc-build-guest/<pkg>/install/` and not
inside a shared `$prefix` — the archives are
build-tree-local, never installed into the toolchain prefix.

---

## What didn't have to change

Worth recording because it's evidence the chapter 178/e
groundwork carries forward cleanly:

- **The aarch64-osdev-cc wrapper.**  No edits.  Whatever
  the math libs throw at it (`-c`, `-shared` rejected,
  `-Wstrict-prototypes`, etc.) it already handles or
  ignores.
- **The libosdevc.a archive form** (chapter 179).
  GMP's configure runs link probes; the wrapper's
  `--start-group / --end-group` plus the chapter-179
  `cstring.o` extern bridge cover everything.
- **crt0.o** (chapter 156).  GMP's configure runs trivial
  `int main(){return 0;}` link probes; crt0.o's
  `_start -> main` flow makes them all return success.
- **The 8 unprefixed binutils symlinks** in the sysroot
  (chapter 184).  gcc's driver finds `as` / `ld` /
  `ar` / `ranlib` automatically; gmp's `Makefile`
  invokes `$(CC) -shared` (which the wrapper rejects),
  but the static archive flow uses `$(AR)` and `$(RANLIB)`
  which find their unprefixed forms.
- **The chapter-179 env triple**
  (`NDEBUG / OSDEV_LIBC_NO_GLOBAL_DEFS / OSDEV_LIBC_NO_GETOPT`).
  Applied verbatim; no per-library tweaks were needed.

The lesson — chapter 179's libc-gap and config-cache
investments pay off here at near-zero marginal cost.

---

## What you'll write

- `vendor/gmp-aarch64-osdev.patch` (NEW, 52 lines incl. header)
- `vendor/mpfr-aarch64-osdev.patch` (NEW, 44 lines)
- `vendor/mpc-aarch64-osdev.patch` (NEW, 43 lines)
- `vendor/gmp-6.2.1/configfsf.sub` (1 line; covered by patch)
- `vendor/mpfr-4.1.0/config.sub` (1 line; covered by patch)
- `vendor/mpc-1.2.1/build-aux/config.sub` (1 line; covered by patch)
- `scripts/fetch_gcc_prereqs.sh` (+ patch-apply loop, idempotent)
- `scripts/test_guest_gcc.py` (NEW, Phase 1)
- `userspace/libc/stdio.h` (+ `_STDIO_H` canonical guard)
- `book/chapters/18-real-gcc-and-real-software/185-gcc-prereqs-for-guest.md` (this file)
- `book/INDEX.md` (+ 185 link)

---

## Run it / Test it

| Check | Result |
|---|---|
| `python3 scripts/test_guest_gcc.py` | PASS (phase 1) |
| `bash scripts/fetch_gcc_prereqs.sh` (clone-fresh path) | applies all three patches, writes markers |
| `bash scripts/fetch_gcc_prereqs.sh` (live-edits path) | detects already-applied via dry-run -R, writes markers |
| `bash scripts/fetch_gcc_prereqs.sh` (re-run after marker) | no-op |
| `aarch64-elf-readelf -h libgmp.a(assert.o)` | AArch64 |
| `aarch64-elf-readelf -h libmpfr.a(set.o)` | AArch64 |
| `aarch64-elf-readelf -h libmpc.a(set.o)` | AArch64 |
| Top-level `make` | clean |
| `python3 scripts/test_xgcc_compile.py` (184 regression) | PASS |

---

## What this unlocks

- **Test that exercises this**:
  [scripts/test_guest_gcc.py](../../../scripts/test_guest_gcc.py)
  — Phase 1 end-to-end, run manually (not in
  `scripts/sweep.sh`; it's a multi-minute host-side
  build).
- **Existing app touched**: none yet.  The end-state
  consumer is the chapter-163 notepad's "Build" button,
  which currently shells out to the chapter-176 wrapper
  on the host's compiler; in a future chapter the build
  button will instead launch `/bin/gcc` inside the guest.
- **Future app:** Phase 4+ will install
  `/bin/gcc` into the OS image, with the sysroot
  mirrored at `/aarch64-osdev/{lib,include,bin}/`.  At
  that point any guest-side program can `gcc foo.c -o
  foo` exactly the way a developer would on a UNIX host.

Until then, chapter 185's payoff is the three archives
sitting at `build/gcc-build-guest/<pkg>/install/lib/` —
unused by Phase 1 itself, ready to be linked into the
guest-side `xgcc` of Phase 2.

## What's next

Looking ahead so the next chapter has a head-start:

1. **gcc 14's top-level configure** will need yet more
   CONFIG_SITE cache entries.  It runs ~150 probes
   beyond what binutils did; expect a fresh round of
   "no cached value" errors and a corresponding round of
   `ac_cv_*=yes` cache additions.
2. **libstdc++ is the elephant.**  Phase 2 will use
   `--disable-libstdc++-v3` initially, just to get
   `xgcc` / `cc1` / `cpp` building.  A separate later
   chapter will do libstdc++ properly (it's a substantial
   port in its own right).
3. **The gcc tarball ships its own libtool plugin
   build** (`liblto_plugin.la`).  Chapter 179's
   "surgical Makefile inject into $LDADD" pattern will
   recur — same shape, just into `xgcc_LDADD` and
   friends.
4. **A second cstring.o batch.**  gcc's host code calls
   `getenv`, `setenv`, `qsort`, `bsearch`, and friends
   that 179 didn't cover.  Add asm-renamed wrappers as
   undefined-references surface.


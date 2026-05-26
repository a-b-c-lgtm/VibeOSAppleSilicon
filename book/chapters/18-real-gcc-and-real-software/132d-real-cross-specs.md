# Chapter 132d — Real cross-compiler specs: retiring the wrapper

> **Milestone in this chapter:** bake everything the link needs
> into gcc's own target header and sysroot so the chapter-131b
> shell wrapper is no longer required.
> **Code referenced:**
> - [vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h](../../../vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h)
> - [Makefile](../../../Makefile) (`xgcc-sysroot`)
> - [scripts/test_xgcc_compile.py](../../../scripts/test_xgcc_compile.py)
>
> **At the end of this chapter** you will have
> `aarch64-osdev-gcc hello.c -o hello` producing a static
> aarch64 ELF with no `-B`, no `-isystem`, no `-T`, no
> `crt0.o` on the command line — everything resolved through
> gcc's standard sysroot. The chapter-131b shell wrapper is
> kept as a reference artefact but is no longer invoked.
> Prerequisites: chapters 131a (binutils-osdev), 131b
> (wrapper), 132a–c (gcc target + prereqs + cross-build).

---

## What you'll do in this chapter

1. Add `--with-sysroot=$(TOOLCHAIN_PREFIX)/aarch64-osdev`
   to the gcc top-level configure (cold rebuild required).
2. Overwrite the inherited specs in
   `gcc/config/aarch64/aarch64-osdev.h`: `CPP_SPEC`,
   `CC1_SPEC`, `STARTFILE_SPEC`, `ENDFILE_SPEC`,
   `LIB_SPEC`, `LIBGCC_SPEC`, `REAL_LIBGCC_SPEC`,
   `LINK_SPEC`.
3. Add the `xgcc-sysroot` Makefile target that copies
   `crt0.o`, `linker_user.ld`, every `userspace/libc/*.h`,
   and the eight binutils tool symlinks into
   `$prefix/aarch64-osdev/{lib,include,bin}/`.
4. Run `make clean-xgcc && make gcc-osdev` and verify
   with `scripts/test_xgcc_compile.py` that a bare
   `aarch64-osdev-gcc hello.c -o hello` works.
5. Stop using the chapter-131b `aarch64-osdev-cc`
   wrapper anywhere on the host — keep it on disk as a
   reference per the debug-scripts policy.

---

## Why now

Chapter 132c installed `aarch64-osdev-gcc` with the
inherited bare-metal specs from `aarch64-elf-raw.h`. That
compiler can preprocess and emit assembly but cannot
link. Every gap was already known and already worked
around by chapter 131b's `aarch64-osdev-cc` shell wrapper.
The wrapper carried the OSdev secrets the compiler should
have shipped with. That works for hand-written scripts
that happen to spell `aarch64-osdev-cc` but breaks the
moment any third-party port calls `$(CC)` directly. This
chapter folds every wrapper trick into
`aarch64-osdev.h` so `aarch64-osdev-gcc` becomes a
self-sufficient cross compiler.

---

## What "real specs" means

A GCC *spec* is a small string that the driver expands when
constructing argv for the subprocesses it spawns: `cpp`,
`cc1`, `as`, `collect2/ld`.  Every spec is named, and every
named spec is overridable per-target.  Chapter 132c built
the compiler with the default (aarch64-elf-raw inherited)
specs, which is why a bare `aarch64-osdev-gcc hello.c -o
hello` failed:

1. `cpp` had no idea about `__OSDEV_LIBC__`, so libiberty
   headers picked the wrong code paths.
2. `cc1` ran with `__STDC_HOSTED__=1` and tried to
   `#include_next <stdint.h>`, which doesn't exist for
   OSdev.
3. `as` was a plain `as` (no path), so gcc fell back to
   `/usr/bin/as` — Mach-O.  Errors looked like
   `unknown directive .type`.
4. The link line carried `-T linker_user.ld` with no path,
   no `-L`, no `crt0.o` — so ld didn't even start.
5. `-lgcc` got auto-appended (OSdev has no libgcc.a),
   so even a hand-driven link failed.

Chapter 131b papered over all five with a 200-line
`aarch64-osdev-cc` shell wrapper that prepended `-B`,
`-isystem`, `-T`, `crt0.o`, and `-nostdlib`.  That worked
for the wrapper-aware callers but no port that doesn't
know about it (autoconf, make, anything calling `$(CC)`)
ever benefited. Chapter 132d teaches the *real* compiler
each lesson, once, in `aarch64-osdev.h`.

---

## The spec overrides, one at a time

All five live in
[vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h](../../../vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h)
and replace the inherited definitions from
`aarch64-elf-raw.h`.

### `CPP_SPEC` — auto-define `__OSDEV_LIBC__`

```c
#undef CPP_SPEC
#define CPP_SPEC "-D__OSDEV_LIBC__"
```

The chapter-131d libc-gap work added `#ifdef __OSDEV_LIBC__`
guards inside vendored gnulib / libiberty sources
(`getopt.c`, `obstack.c`, etc.) to swap in the OSdev
header-only shims.  Chapter 131b passed `-D__OSDEV_LIBC__`
from the wrapper; now every invocation gets it
automatically.

`-dM` confirms it post-rebuild:

```
$ aarch64-osdev-gcc -E -dM -xc - </dev/null | grep OSDEV
#define __osdev__ 1
#define __OSDEV_LIBC__ 1
```

### `CC1_SPEC` — force `-ffreestanding`

```c
#undef CC1_SPEC
#define CC1_SPEC "%{!ffreestanding:%{!fno-freestanding:-ffreestanding}}"
```

`%{!foo:X}` reads "if `-foo` was not given on the command
line, expand to `X`."  This nested form means "unless the
user has explicitly said `-ffreestanding` or
`-fno-freestanding`, force `-ffreestanding`."

Why it has to be the default: gcc's bundled `stdint.h`
under `lib/gcc/aarch64-osdev/14.2.0/include/` is
`stdint.h`, a *wrapper* that does
`#include_next <stdint.h>` to chain into the system libc's
`stdint.h`.  The OSdev libc has no `stdint.h` (it uses
`inttypes.h` directly), so the include_next blows up.

`-ffreestanding` flips `__STDC_HOSTED__` to 0; gcc then
picks `stdint-gcc.h` (a complete freestanding header that
defines `int32_t` and friends from compiler builtins) and
never tries the wrapper.

This is also exactly the right *meaning*: the OSdev world
is freestanding — no `argc`/`argv` from POSIX `main`, no
`exit()` returning to a parent shell from the perspective
of the kernel, no signal disposition inherited from a
process group.  Userspace runs on top of a thin libc shim
that's much closer to "freestanding + a few syscalls" than
to "hosted POSIX."

### `STARTFILE_SPEC` — only `crt0.o`

```c
#undef STARTFILE_SPEC
#define STARTFILE_SPEC "%{!nostartfiles:crt0%O%s}"
```

`%O` is "the object-file suffix" (`.o`).  `%s` means "find
this file by searching the startfile prefix list" — that's
gcc's library search path with the same priority as `-L`,
plus the sysroot.  The chapter-120 `crt0.o` is installed
at `$prefix/aarch64-osdev/lib/crt0.o`, so `%s` finds it
without any path or `-L` from the user.

No `crti.o` / `crtn.o` (OSdev has no `_init` / `_fini`),
no `crtbegin.o` / `crtend.o` (no C++ static constructors
via gcc's mechanism — crt0 walks `.init_array` itself).

`%{!nostartfiles:...}` honours `-nostartfiles` so kernel-
side standalone tests can opt out.  The smoke checks this
opt-out is honoured by inspecting the verbose `collect2`
command and asserting `crt0.o` does *not* appear when
`-nostartfiles` is passed.

### `ENDFILE_SPEC` / `LIB_SPEC` / `LIBGCC_SPEC` — empty

```c
#undef ENDFILE_SPEC
#define ENDFILE_SPEC ""

#undef LIB_SPEC
#define LIB_SPEC ""

#undef REAL_LIBGCC_SPEC
#define REAL_LIBGCC_SPEC ""
#undef LIBGCC_SPEC
#define LIBGCC_SPEC ""
```

`LIB_SPEC` is "stuff appended to the link line after user
objects" — for hosted targets it's `-lc -lm`.  OSdev's
libc is header-only; nothing to `-l`.

`LIBGCC_SPEC` (and its `REAL_LIBGCC_SPEC` cousin, used when
LTO is in the mix) defaults to `-lgcc -lgcc_s` or similar
depending on configuration.  There is no `libgcc.a` — gcc's
soft-float helpers, division routines, and 128-bit ops are
all served by the chapter-120 header-only libgcc shim
(`userspace/libc/libgcc.h`).  An empty `LIBGCC_SPEC` keeps
the driver from auto-appending the missing archive.

`ENDFILE_SPEC` is the trailing crt files (`crtend.o`,
`crtn.o`); empty, same reason as `STARTFILE_SPEC` skipping
them.

### `LINK_SPEC` — feed ld the linker script

```c
#undef LINK_SPEC
#define LINK_SPEC "                              \
   %{static:-Bstatic}                            \
   %{mbig-endian:-EB} %{mlittle-endian:-EL} -X   \
   %{!T*:-T %R/lib/linker_user.ld}               \
   -maarch64elf%{mbig-endian:b}"                 \
  AARCH64_ERRATA_LINK_SPEC
```

This one carries the most subtlety per character.  Going
left to right:

- `%{static:-Bstatic}` — pass `-Bstatic` to ld if the user
  asked for `-static`.  All OSdev binaries are static;
  this is mostly defensive.
- `%{mbig-endian:-EB} %{mlittle-endian:-EL}` — propagate
  endianness from the cc1 invocation through to ld.  The
  default is little-endian (aarch64 ABI is bi-endian) so
  `-EL` is the usual outcome.
- `-X` — strip local symbols starting with `L` (.L0, .Ltmp,
  etc.) from the output.  Inherited from `aarch64-elf-raw`;
  shaves the symbol table.
- `%{!T*:-T %R/lib/linker_user.ld}` — *the* key clause.
  "Unless the user passed any `-T*` option" (so `-T`,
  `-Tdata`, `-Ttext`, etc.) "feed ld `-T %R/lib/linker_user.ld`."
  `%R` is gcc's sysroot expansion: it becomes the path that
  `--with-sysroot` was configured with.
- `-maarch64elf%{mbig-endian:b}` — emulation: pick
  `aarch64elfb` (big-endian) if the user asked for it, else
  `aarch64elf`.
- `AARCH64_ERRATA_LINK_SPEC` — macro from `aarch64-elf.h`
  that adds the `-mfix-cortex-a53-*` erratum workarounds
  when the matching `-mfix-*` flags were used.  Appending
  it verbatim keeps the inherited behaviour.

The drama is `%R/lib/linker_user.ld`.  Three things were
tried first and don't work:

1. **`-T linker_user.ld` (bare name).**  Doesn't search any
   path.  ld looks in cwd only.  Works if you `cd` into
   `$prefix/aarch64-osdev/lib/` first, otherwise no.
2. **`-T linker_user.ld` plus a `-L $prefix/aarch64-osdev/lib/`.**
   The man page suggests `-T` searches `-L` paths.  In
   binutils 2.44 it does *not* — `-T` is processed at
   parse-time before the search-path table is populated.
3. **Hoping ld's default `SEARCH_DIR` covers it.**  ld 2.44
   indeed prints `SEARCH_DIR("$prefix/aarch64-osdev/lib")`
   under `--verbose`, but the search-dir list is for `-l`
   library lookup, not for `-T` script lookup.  Empirically
   confirmed: putting the script there does nothing for
   `-T linker_user.ld`.

What does work: pass a path ld can `open()` directly.
The cleanest portable source for that path is `%R`, gcc's
sysroot prefix.  That requires the compiler to be configured
with `--with-sysroot=…` — done in chapter 132d's Makefile
change (see next section).

---

## Sysroot: the layout

```
$prefix/aarch64-osdev/
├── bin/
│   ├── as      -> ../../bin/aarch64-osdev-as
│   ├── ld      -> ../../bin/aarch64-osdev-ld
│   ├── ar      -> ../../bin/aarch64-osdev-ar
│   ├── nm      -> ../../bin/aarch64-osdev-nm
│   ├── objcopy -> ../../bin/aarch64-osdev-objcopy
│   ├── objdump -> ../../bin/aarch64-osdev-objdump
│   ├── ranlib  -> ../../bin/aarch64-osdev-ranlib
│   └── strip   -> ../../bin/aarch64-osdev-strip
├── include/
│   ├── stdio.h, stdlib.h, string.h, ...
│   └── (everything from userspace/libc/*.h)
└── lib/
    ├── crt0.o            (chapter-120 startup)
    └── linker_user.ld    (chapter-15 user link script)
```

Three populations:

1. **Library inputs** (`lib/`): `crt0.o` and
   `linker_user.ld`.  `STARTFILE_SPEC` finds the former via
   `%s`; `LINK_SPEC` finds the latter via `%R/lib/`.
2. **Headers** (`include/`): gcc adds
   `$prefix/$target/include/` to its `<...>` search list
   for every cross-compiler invocation, so a bare
   `#include <stdio.h>` resolves to the OSdev header.
3. **Tools** (`bin/`): gcc's cross-tool lookup expects the
   *binutils FHS layout* — `$prefix/$target/bin/{as,ld,…}`
   with *unprefixed* names.  The OSdev binutils install
   only creates the flat `$prefix/bin/aarch64-osdev-as`
   form.  The Makefile symlinks the eight tools used in
   practice into the target bin/ so gcc finds them
   without `-B`.

The whole sysroot is wiped by `make clean-xgcc` (it lives
under `$prefix/aarch64-osdev/`, which `clean-xgcc` removes
wholesale) and re-installed by `make xgcc-sysroot` (which
`gcc-osdev` depends on).

---

## Configure flag: `--with-sysroot`

```diff
 ../../$(GCC_SRC)/configure \
     --target=aarch64-osdev \
     --prefix=$(TOOLCHAIN_PREFIX) \
     --program-prefix=aarch64-osdev- \
+    --with-sysroot=$(TOOLCHAIN_PREFIX)/aarch64-osdev \
     --enable-languages=c \
     ...
```

`--with-sysroot` does two things:

1. Sets `TARGET_SYSTEM_ROOT` in the compiler, which is
   what `%R` expands to in spec strings.
2. Causes gcc to prefer paths under the sysroot when
   resolving `--with-headers` / `--with-libs` directives.

Point it at `$prefix/aarch64-osdev/` (not `$prefix/`)
so `%R/lib/` resolves to the same `aarch64-osdev/lib/`
that `xgcc-sysroot` populates.  Same layout will be
mirrored inside the guest in chapter 132e — when xgcc is
shipped as `/bin/gcc`, sysroot at `/aarch64-osdev/`, the
spec strings will resolve correctly on the target with
zero changes.

This flag flip *requires* a cold rebuild (`make clean-xgcc
&& make gcc-osdev`).  Incremental rebuilds after editing
just `aarch64-osdev.h` are still ~1m20s; the cold rebuild
to pick up `--with-sysroot` is ~3m40s on a 12-core Mac.

---

## The Makefile pipeline

The chapter-132d block (after `clean-xgcc`):

```make
XGCC_SYSROOT     := $(TOOLCHAIN_PREFIX)/aarch64-osdev
XGCC_SYSROOT_LIB := $(XGCC_SYSROOT)/lib
XGCC_SYSROOT_INC := $(XGCC_SYSROOT)/include
XGCC_SYSROOT_BIN := $(XGCC_SYSROOT)/bin

# Four atomic install rules, all depend on the xgcc binary
# being built first so the sysroot never gets a stale install.
$(XGCC_SYSROOT_LIB)/crt0.o:    $(BUILD)/userspace/crt/crt0.o | $(GCC_XGCC)
$(XGCC_SYSROOT_LIB)/linker_user.ld: userspace/linker_user.ld | $(GCC_XGCC)
$(XGCC_SYSROOT_INC)/.osdev-libc-stamp: $(wildcard userspace/libc/*.h) | $(GCC_XGCC)
$(XGCC_SYSROOT_BIN)/.osdev-tools-stamp: $(BINUTILS_AS) $(BINUTILS_LD) | $(GCC_XGCC)

.PHONY: xgcc-sysroot
xgcc-sysroot: $(XGCC_SYS_CRT0) $(XGCC_SYS_LDS) $(XGCC_SYS_INC_MARKER) \
              $(XGCC_SYS_BIN_MARKER)

# Make gcc-osdev install the sysroot too.
gcc-osdev: xgcc-sysroot
```

Two stamp files (`.osdev-libc-stamp`, `.osdev-tools-stamp`)
let make decide whether to re-copy without depending on
N+1 file targets for each header / tool.  Touching any
`userspace/libc/*.h` invalidates the libc stamp; rebuilding
binutils invalidates the tools stamp.

---

## Pitfalls

Three failure modes surfaced in order during bring-up.
Each diagnosis left a reusable signpost — recorded here so
the next edit of `aarch64-osdev.h` recognises them on sight.

### Pitfall — `stdint.h: No such file or directory`

**Symptom:**

```
In file included from /Users/seusher/Desktop/osdev/build/toolchain/lib/gcc/aarch64-osdev/14.2.0/include/stdint.h:9:
.../include/stdint.h: fatal error: stdint.h: No such file or directory
```

Looks recursive because it *is* recursive.

**Cause:** gcc's `stdint.h` is a thin
`#include_next <stdint.h>` wrapper that expects to chain
into the system libc. No system libc, no chain.

**Fix:** `CC1_SPEC` forces `-ffreestanding`; gcc switches
to `stdint-gcc.h` (a complete header) and never tries the
wrapper.

### Pitfall — GAS-syntax errors (`unknown directive .type`)

**Symptom:**

```
$ aarch64-osdev-gcc hello.c -o hello
...
/var/folders/.../ccXXXXX.s:42:2: error: unknown directive
        .type   main, %function
        ^
```

**Cause:** The `.type X, %function` directive is GNU `as`
syntax. macOS `/usr/bin/as` is Mach-O LLVM-based and
rejects it. The diagnostic means gcc fell back to
`/usr/bin/as` because it couldn't find the OSdev one.

Smoking gun:

```
$ aarch64-osdev-gcc -print-prog-name=as
as            # plain name, no path = gcc didn't find it
```

**Fix:** bridge a layout mismatch.  gcc's cross-compiler
search adds `$prefix/$target/bin/` and looks for
*unprefixed* tool names there.  The chapter-131a binutils
install put tools at `$prefix/bin/$target-as` only.
Eight symlinks in the Makefile bridge the two:

```
$prefix/aarch64-osdev/bin/as -> ../../bin/aarch64-osdev-as
```

After the fix, `-print-prog-name=as` returns the full
symlink path, and gcc uses the OSdev `as`.

### Pitfall — `cannot open linker script file linker_user.ld`

**Symptom:**

```
ld: cannot open linker script file linker_user.ld:
   No such file or directory
collect2: error: ld returned 1 exit status
```

**Cause:** The `-T linker_user.ld` was getting through to
ld, but with no path; ld's `-T` doesn't search `-L` paths
or its default `SEARCH_DIR`, so the file is invisible
unless cwd contains it.

**Fix:** configure xgcc with
`--with-sysroot=$prefix/aarch64-osdev` and change
`LINK_SPEC` to `-T %R/lib/linker_user.ld`.  `%R` becomes
the sysroot prefix, gcc hands ld an absolute path, ld
opens the file.

---

## What this unlocks

- `aarch64-osdev-gcc hello.c -o hello` — the *bare* command
  now compiles and links a static aarch64 ELF.  This is
  what every third-party port's `configure && make` will
  call.  Chapter 131b's wrapper exists, but nothing in the
  build is required to know about it.
- The same compiler image will work in-guest unchanged
  (chapter 132e), because the sysroot path is *relative*
  to the install prefix — neither `$(TOOLCHAIN_PREFIX)`
  nor `$prefix` is baked into the binary as an absolute
  host path.
- Chapter 131b's `aarch64-osdev-cc` is kept as a reference
  artefact (per debug-scripts-policy) but no longer
  required by any pipeline.

## What you'll write

- New configure flag: `--with-sysroot=$(TOOLCHAIN_PREFIX)/aarch64-osdev`
  in [Makefile](../../../Makefile) (chapter-132c block).
- Re-built [vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h](../../../vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h)
  with `CPP_SPEC`, `CC1_SPEC`, `STARTFILE_SPEC`,
  `ENDFILE_SPEC`, `LIB_SPEC`, `LIBGCC_SPEC`,
  `REAL_LIBGCC_SPEC`, and `LINK_SPEC` overrides.
- New Makefile targets in the chapter-132d block
  (`xgcc-sysroot`, four file/stamp rules, plus `gcc-osdev`
  now depends on `xgcc-sysroot`).
- New script:
  [scripts/test_xgcc_compile.py](../../../scripts/test_xgcc_compile.py)
  — host smoke that proves CPP_SPEC injects
  `__OSDEV_LIBC__`; bare `gcc hello.c -o hello` produces a
  valid aarch64 LE ELF with `_user_start` in the
  USER_LOAD_ADDR window; `-nostartfiles` removes `crt0.o`
  from the link line.
- Patch file [vendor/gcc-aarch64-osdev.patch](../../../vendor/gcc-aarch64-osdev.patch)
  regenerated to include all four spec overrides (so
  re-extracting a fresh gcc-14.2.0 source tree and running
  `patch -p1` reproduces the chapter-132d compiler).
- No existing apps changed — host-only chapter.  Chapter
  132e ships this into the guest and lets the in-guest
  shell drive it.

## Run it / Test it

- New: [scripts/test_xgcc_compile.py](../../../scripts/test_xgcc_compile.py)
  — host smoke.  Skips when xgcc/sysroot not present,
  PASSes once `make gcc-osdev` has been run.  Not in
  `scripts/sweep.sh` (host-only).
- Existing: `scripts/test_xgcc_build.py` (chapter 132c) —
  still PASS.
- Existing: `scripts/sweep.sh` (61/61) — unchanged.
  Nothing in the guest depends on xgcc yet.

## What's next

- **132e** — ship `aarch64-osdev-gcc` (plus binutils, plus
  the mirrored sysroot) into the guest disk image.
  In-guest test:
  ```
  /bin/gcc /tmp/hello.c -o /tmp/hello && /tmp/hello
  ```
  closes Phase 2 of the chapter 128 plan.
- **133a+** — Phase 3: native-compile `doomgeneric` inside
  the guest, ending in `/bin/doom` from in-guest-built
  binaries.


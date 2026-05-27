# Chapter 175 — Binutils with an `aarch64-osdev` target

> **Milestone in this chapter:** teach upstream binutils about a
> new target triple whose OS field is `osdev`, and host-build
> the resulting `aarch64-osdev-as` / `aarch64-osdev-ld`.
> **Code referenced:**
> - [vendor/binutils-2.44/](../../../vendor/binutils-2.44/)
>   (`config.sub`, libbfd, `gas`, `ld` — the four-hunk additive
>   patch)
> - [Makefile](../../../Makefile) (`make binutils-osdev`)
> - [scripts/test_binutils_target.py](../../../scripts/test_binutils_target.py)
>
> **At the end of this chapter** you will have
> `build/toolchain/bin/aarch64-osdev-as` and
> `aarch64-osdev-ld` cross-built from a sha256-pinned tarball
> plus a small additive patch, and a host smoke test that
> assembles and links a four-instruction program and asserts
> the output is ELF64-LE aarch64. Prerequisite: chapter 164
> for motivation, chapter 174 for the Phase 1 close.

---

## What you'll do in this chapter

1. Write a four-hunk additive patch against
   `binutils-2.44` that teaches its `config.sub`, libbfd,
   `gas`, and `ld` about a new `aarch64-osdev` target
   triple.
2. Write `scripts/fetch_binutils.sh` — idempotent
   sha256-pinned download, extract, patch, and sanity-check.
3. Add a `binutils-osdev` Makefile target that runs an
   out-of-tree configure + build + install into
   `build/toolchain/`.
4. Write the host smoke test
   `scripts/test_binutils_target.py` that assembles and
   links a 4-instruction program and asserts the result is
   ELF64-LE aarch64.

## Why a new triple

The cross-binutils Homebrew installs as
`aarch64-elf-binutils` has driven every chapter of this book
so far. It works because the kernel and userspace ABIs
happen to match what `aarch64-elf` assumes: ELF64-LE, no
syscall convention baked into the linker, no
platform-specific dynamic-loader path.

That's fine for the assembler and linker, which don't make
many platform-specific choices. It will *not* be fine for
GCC in chapter 177. GCC's target-triple controls:

- The set of pre-defined preprocessor macros. `aarch64-elf`
  defines `__ELF__` and that's about it. This chapter wants
  `__osdev__`, and eventually feature macros
  (`__has_pthreads__`, `__has_mmap__`, etc.) keyed off it
  so userspace code can `#ifdef __osdev__` to opt into
  things that aren't portable.
- The default library search paths and crt0 names. On a
  hosted target, GCC links `crt1.o`, `crti.o`, `crtn.o`
  from a known prefix. `aarch64-elf` is "bare metal" — it
  doesn't. The OSdev prefix points at
  `build/toolchain/aarch64-osdev/lib/` (eventually
  `/usr/lib` inside the guest) and the crt0 is the one
  already shipped at chapter 156.
- The libc and the default `--with-newlib` / `--with-glibc`
  posture. `aarch64-elf-gcc` doesn't link a libc at all
  because there isn't one on a bare-metal target. There
  *is* one here (chapters 148–164), and the cross compiler
  needs to know that.

Doing all of the above on top of `aarch64-elf` would mean
either shipping a wrapper script that injects all the right
flags on every invocation, or maintaining a patched-fork
of binutils-and-gcc that pretends the triple is
`aarch64-elf` but behaves differently. Both are worse than
declaring a new triple. So this chapter does that.

The triple is `aarch64-osdev`. `config.sub` will
canonicalise it to `aarch64-unknown-osdev` (cpu=aarch64,
vendor=unknown, os=osdev), and that's the form everything
downstream uses.

---

## The four hunks

The whole patch is at
`vendor/binutils-aarch64-osdev.patch`. It's 100% additive
— no existing line is modified, only new lines inserted
between existing ones. That property matters: it means a
future `binutils-2.45` will almost certainly accept the
same patch without manual rebase, because the surrounding
context (the cases for `aarch64-*-elf*`, `aarch64-*-linux*`,
etc.) doesn't get renamed across releases.

### 1. `config.sub` — make the triple parse

`config.sub` is the shell script that turns a user's
fragmentary triple (`aarch64-osdev`) into the canonical
form (`aarch64-unknown-osdev`). It has a long `case`
listing every OS name it knows. The hunk inserts
`| osdev* \` between the existing `| ose* \` and `| osf* \`
lines. After the hunk:

```
$ bash vendor/binutils-2.44/config.sub aarch64-osdev
aarch64-unknown-osdev
```

Without it the script exits 1 with `Invalid configuration
'aarch64-osdev': OS 'osdev' not recognized`, which kills
configure before it starts.

### 2. `bfd/config.bfd` — tell libbfd what an `aarch64-osdev` ELF is

libbfd is the binary-format library every binutils tool
links against. For each target triple it needs to know:

- The default vector (the BFD "target name" — for us,
  `aarch64_elf64_le_vec`, same as `aarch64-elf`).
- The set of other vectors to compile in as alternates
  (so `objdump -b` can read big-endian, 32-bit, etc.
  variants when asked).

The hunk inserts a new case `aarch64-*-osdev*)` after the
existing `aarch64-*-elf | aarch64-*-rtems* | aarch64-*-genode*)`
case, with `targ_defvec=aarch64_elf64_le_vec` and the same
`targ_selvecs` list as the elf case **minus the PE vectors**
(`aarch64_pei_le_vec`, `aarch64_pe_le_vec`). PE/COFF is
Windows; the OSdev toolchain will never emit it.

### 3. `gas/configure.tgt` — tell gas to use ELF syntax

`gas` (the GNU assembler) needs to know what object-file
format to emit for each target. For `aarch64-osdev` we want
ELF, same as everything else aarch64. The hunk is a single
line:

```
  aarch64*-*-osdev*)                    fmt=elf;;
```

inserted between the genode and linux cases. Without it,
configure picks a default that varies by host, which has
bitten people in the past on macOS where `fmt=mach-o`
sometimes gets selected.

### 4. `ld/configure.tgt` — tell ld which emulation to default to

`ld` supports multiple "emulations" (sets of default
linker-script + page-size + entry-symbol). For
`aarch64-osdev` set:

- `targ_emul=aarch64elf` — reuse the existing aarch64 ELF
  emulation. The OSdev kernel and userspace ABIs agree with
  it on entry symbol (`_start`), page size (4 KiB), and
  text base (0x400000 for static binaries — chapter 12).
- `targ_extra_emuls="aarch64elf32 aarch64elf32b aarch64elfb
  armelf armelfb"` — same as `aarch64-elf` minus the PE
  emulations.
- `targ_extra_libpath="aarch64elf"` — lets `ld -m
  aarch64elf` find its scripts.

Deliberately skip a new `ld/emulparams/aarch64osdev.sh`.
The only reason to ship one would be to override entry
symbol / page size / text base, and none of those differ
from `aarch64elf` for this OS. A new emulparams file would
also obligate adding scripttempl entries and rebuilding
every time upstream tweaks the aarch64elf template.
Reusing the existing emulation is strictly less code.

---

## The build flow

`scripts/fetch_binutils.sh` is idempotent:

1. If `vendor/binutils-2.44.tar.xz` is absent or its sha256
   doesn't match `ce2017e0…`, download it from
   `ftp.gnu.org/gnu/binutils/`. The pin is so a corrupted
   mirror can't silently change our compiler.
2. If `vendor/binutils-2.44/` doesn't exist, extract the
   tarball into it.
3. If `vendor/binutils-2.44/.patched-osdev` doesn't exist,
   apply `vendor/binutils-aarch64-osdev.patch` and create
   the marker.
4. Sanity-check by running the patched `config.sub` against
   `aarch64-osdev` and asserting it canonicalises to
   `aarch64-unknown-osdev`. If it doesn't, the patch is
   broken — bail before the Makefile spends 50 s building
   the wrong thing.

The Makefile target `binutils-osdev` does an out-of-tree
build in `build/binutils-build/` with these configure flags:

| Flag | Why |
|---|---|
| `--target=aarch64-osdev` | the whole point. |
| `--prefix=$(abspath build/toolchain)` | install into a workspace-local prefix so no `sudo` is needed. |
| `--program-prefix=aarch64-osdev-` | binaries land as `aarch64-osdev-as`, not the default `aarch64-unknown-osdev-as`. |
| `--disable-nls` | no localisation; translated error messages aren't worth the dependency. |
| `--disable-gdb` | gdb has heavy dependencies (Python, libreadline) and chapter 131 isn't shipping a guest gdb. |
| `--disable-werror` | binutils-2.44 has a handful of macOS-only warnings (the `pointer comparison always evaluates to false` in `readelf.c` shows up cleanly without it). |
| `--disable-multilib` | one ABI is enough; no aarch32 support is shipped. |
| `--with-system-zlib` | the bundled zlib in 2.44 doesn't compile under macOS's C23 clang (`zutil.c` has K&R prototypes). System zlib is fine. |

Then `make MAKEINFO=true` (skip texinfo: the host's
`makeinfo` is too old to format binutils' info docs and
those docs aren't part of the deliverable), `make
install-binutils install-gas install-ld`. Total wall time
on Apple Silicon: roughly 50 seconds for the build, 5
seconds for the install.

The host smoke test `scripts/test_binutils_target.py`
writes a 4-instruction asm file, assembles it with
`aarch64-osdev-as`, links it with `aarch64-osdev-ld`, and
reads the first 20 bytes of the result to assert ELF magic
+ EI_CLASS=64 + EI_DATA=little + e_machine=183 (EM_AARCH64).
It's intentionally not added to `scripts/sweep.sh` — sweep
runs kernel regression, this is a host-toolchain sanity
check.

---

## What this unlocks

Two binaries in `build/toolchain/bin/`:

```
aarch64-osdev-as       — assembler
aarch64-osdev-ld       — linker
aarch64-osdev-ar       — archiver
aarch64-osdev-objcopy  — section copier (needed in 177 for crt files)
aarch64-osdev-objdump  — disassembler
aarch64-osdev-readelf  — header inspector
aarch64-osdev-nm       — symbol lister
aarch64-osdev-ranlib   — index regenerator
aarch64-osdev-strip    — symbol stripper
aarch64-osdev-strings  — string scanner
aarch64-osdev-addr2line / c++filt / elfedit / size
```

None of these run *inside* the guest yet. They're host
tools, used right now only by the smoke test. Their real
job starts in:

- **176** — port `aarch64-osdev-as` and
  `aarch64-osdev-ld` to run inside the guest. That means
  building them with `aarch64-osdev-gcc` (not yet built)
  against the OSdev libc, then dropping the resulting ELF
  binaries into `/bin/`. The chapter bootstraps the loop:
  the host build of binutils builds a guest build of
  binutils, then the guest binaries are verified to
  produce byte-identical output for the same inputs as
  the host ones.
- **177** — GCC. The big one. Re-uses every flag this
  chapter wrote down (`--target=aarch64-osdev`,
  `--program-prefix`, the toolchain prefix path), plus
  GCC-specific decisions about libgcc, multilib, and
  whether to bootstrap.
- **178** — swap `/bin/as` and `/bin/ld` (currently the
  chapter-154 and chapter-155 stubs) for the real binutils
  ports from 176. The chapter-157 `/bin/cc` driver will
  start invoking the real ones, and at that point the
  guest can self-compile small C programs without the host.

Files touched in this chapter:

- New host build target: `make binutils-osdev` (Makefile
  ~line 2350+).
- New script: `scripts/fetch_binutils.sh` (idempotent
  fetch + patch + sanity check).
- New patch: `vendor/binutils-aarch64-osdev.patch` (4
  hunks, 100% additive).
- New gitignore entries: `vendor/binutils-2.44/`,
  `vendor/binutils-2.44.tar.xz` (the patch itself is
  committed; the source tree is regeneratable).
- No existing apps changed — host-only.

## Run it / Test it

- New: `scripts/test_binutils_target.py` — host smoke
  test, not in sweep. Run manually after `make
  binutils-osdev` or after editing the patch.
- Unchanged: the rest of `scripts/sweep.sh`. This chapter
  ships zero guest-side code, so no kernel regression
  surface changes.

## What's next

Chapter 176 lifts the host-only `aarch64-osdev-gcc`
wrapper into something the rest of Phase 2 can lean on.

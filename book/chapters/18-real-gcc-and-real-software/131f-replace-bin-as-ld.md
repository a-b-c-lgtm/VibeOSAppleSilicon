# Chapter 131f — Replacing `/bin/as` and `/bin/ld` with the real binutils

> **Status:** shipped. `/bin/as` and `/bin/ld` on the
> guest disk are now the GNU `as-new` / `ld-new` binaries
> cross-built in chapter 131e. The toy chapter-118
> assembler and chapter-119 linker stay buildable for
> readers of those chapters but are no longer wired into
> `build/disk.img`.
>
> **Tests.** [scripts/test_bin_as.py](../../../scripts/test_bin_as.py)
> 8/8 PASS, [scripts/test_bin_ld_ar.py](../../../scripts/test_bin_ld_ar.py)
> 12/12 PASS, [scripts/test_cc_hello.py](../../../scripts/test_cc_hello.py)
> 15/15 PASS, [scripts/test_cc_vars.py](../../../scripts/test_cc_vars.py)
> 16/16 PASS, [scripts/test_notepad_build.py](../../../scripts/test_notepad_build.py)
> 10/10 PASS. The toolchain contract from chapter 122
> (the cc → as → ld pipeline) is the spec — every test
> that drove the toy tools still passes against the GNU
> drop-in.
>
> **Prereqs:** chapter 131e (cross-built `ld-new` + `as-new`).
>
> **Opens:** chapter 132a — give GCC an `aarch64-osdev`
> target so the real `xgcc` can be cross-built next.

---

## What you'll do in this chapter

1. Grow `build/disk.img` from 16 MiB to 32 MiB so the
   stripped GNU binaries fit alongside everything else
   on OSFS-1.
2. Ship `assets/osfs/osdev.ld` as `/bin/osdev.ld` so the
   real `ld` has the OSdev VMA script.
3. Add `-T /bin/osdev.ld` to the `/bin/cc` driver's
   `/bin/ld` invocation.
4. Initialise `tpidr_el0` to a zeroed TLS block in
   `crt0.S` so libiberty's `__thread` statics stop
   faulting.
5. Gate `getopt` in `userspace/libc/stdlib.h` behind
   `OSDEV_LIBC_NO_GETOPT` so libiberty's copy wins for
   vendor TUs.
6. Make `tmpfs_write` positional (honor the fd offset,
   sparse-fill any gap) and have `tmpfs_op_open`
   seed the fd offset to EOF when `O_APPEND` is set.
7. Add hunk 6 to `vendor/binutils-aarch64-osdev.patch`
   fixing `libiberty/lrealpath.c`'s fall-off-the-end UB.
8. Verify with the five test scripts named in the header
   block above.

---

## Why now

The chapter-118/119 toy `as` and `ld` were
single-mnemonic-table, single-section, no-relocation tools.
They carried the book through chapters 121 (`/bin/cc`), 126
(`/bin/make`), 127 (notepad Build button) and the
chapter-130a Doom port — but they cannot link binutils,
gcc, or doomgeneric's full feature set. To advance to
chapter 132 onwards ("real gcc in-guest") a real
linker is the prerequisite.

Chapter 131e built `ld-new` and `as-new` from
binutils-2.44 and verified them under a host smoke test
(parse-and-exit). This chapter wires them into the guest
filesystem in place of the toy versions, and surfaces
every trap that "drop a real binutils binary onto a
freestanding hobby OS and have it actually link a hello
world" exposes.

There are six traps, in the order they bit — documented
in the [Pitfalls](#pitfalls) section below:

1. The disk image was too small to hold a 3 MB `/bin/ld`.
2. GNU `ld` defaults to a Linux ELF load address; OSdev's
   is `0x1000100000`.
3. TLS access from inside `ld-new` crashed because the
   crt0 path never initialised `tpidr_el0`.
4. `getopt`'s global `optind` was shadowed by libiberty's
   own copy, eating the first argument.
5. `libiberty/lrealpath.c` fell off the end of a
   non-void function in the freestanding build — silently
   corrupting `argv[1]`.
6. `tmpfs_write` ignored the fd offset and always
   appended — scrambling every ELF file `as-new` wrote.

The first five are localised. The sixth is the kind of
bug a clean unit test of the kernel would never have
caught: a real program's seek-then-overwrite pattern
broke an interface that until now had only ever been
driven by the shell's `>` and `>>`.

---

## What you'll write (file ledger)

- [Makefile](../../../Makefile) — six new targets:
  - `BINUTILS_AS_NEW` / `BINUTILS_LD_NEW` (built by
    `python3 scripts/test_guest_ld.py`)
  - `BINUTILS_AS_STRIPPED` / `BINUTILS_LD_STRIPPED`
    (passed through `aarch64-elf-strip --strip-all`)
  - `disk.img` recipe lines `as=$(BINUTILS_AS_STRIPPED)`
    and `ld=$(BINUTILS_LD_STRIPPED)` replace the old
    `as=$(AS_STRIPPED)` / `ld=$(LD_STRIPPED)` mappings
  - `OSFS_FILES` gains `assets/osfs/osdev.ld` (the
    linker script the real `ld` needs but the toy `ld`
    hard-coded)
- `assets/osfs/osdev.ld` — copied from
  `userspace/linker_user.ld`; ships as `/bin/osdev.ld`
  on the guest.
- [userspace/cc/cc.c](../../../userspace/cc/cc.c) — adds
  `-T /bin/osdev.ld` to its `/bin/ld` invocation.
- `kernel/core/tmpfs.c` — `tmpfs_write` is now positional
  (honors the fd offset) with sparse zero-fill; `tmpfs_op_open`
  initialises the fd offset to `f->size` when `O_APPEND`
  is requested.
- `kernel/core/tmpfs.h` — `tmpfs_write` prototype updated.
- `kernel/core/syscall.c` — `FD_TMPFS_RW` write branch
  passes `e->offset` to `tmpfs_write` and bumps `e->offset`
  by the bytes written.
- `vendor/binutils-aarch64-osdev.patch` — appended a
  `libiberty/lrealpath.c` hunk (the lrealpath UB fix
  documented in [Pitfalls](#pitfalls) below).
- Tests: [scripts/test_bin_as.py](../../../scripts/test_bin_as.py)
  and [scripts/test_bin_ld_ar.py](../../../scripts/test_bin_ld_ar.py)
  (header rewritten — they now smoke the real GNU drop-ins).

---

## Pitfalls

Six traps, in the order they bit during bring-up.

### Pitfall — disk too small for `/bin/ld`

**Symptom:** `mkosfs` aborts with "image full" once the
stripped `ld-new` + `as-new` land alongside everything
else on the 16 MiB image.

**Cause:** `build/disk.img` was 16 MiB (chapter 122
sized). Stripped `ld-new` is 1.6 MiB and stripped
`as-new` is 1.8 MiB.

**Fix.** Bump `DISK_SIZE` in the Makefile to 32 MiB.
The mkosfs script then reports `33,554,432 bytes, 105
files, 33,852 sectors used` for the current image —
comfortable headroom for chapter 132 onwards.

### Pitfall — GNU `ld`'s default VMA

**Symptom:** `/bin/ld /tmp/hello.o -o /tmp/hello`
produces a binary the kernel ELF loader refuses to
map.

**Cause:** the chapter-119 toy `ld` hard-coded the
user load address `0x1000100000` (USER_TEXT_BASE in
[kernel/core/userspace.h](../../../kernel/core/userspace.h)).
GNU `ld` defaults to its built-in `elf_aarch64` script,
which places `.text` at `0x00400000` — an address the
kernel ELF loader rejects (below USER_HEAP_BASE,
inside the kernel's own carve-out).

**Fix.** Ship the existing
`userspace/linker_user.ld` as `/bin/osdev.ld` on the
OSFS-1 disk, and have every link site pass
`-T /bin/osdev.ld`. That includes:

- [userspace/cc/cc.c](../../../userspace/cc/cc.c) — the
  `/bin/cc` driver. Single line addition to the
  `argv` it spawns `/bin/ld` with.
- [scripts/test_bin_ld_ar.py](../../../scripts/test_bin_ld_ar.py)
  and any future hand-rolled ld invocation in scripts.

The script file is shipped as a regular OSFS-1 entry
named `osdev.ld` (so the makefile's existing
`OSFS_FILES` machinery picks it up), and the mkosfs
recipe maps it to the leaf name `/bin/osdev.ld` via
the `osdev.ld=assets/osfs/osdev.ld` argument.

### Pitfall — TLS bootstrap

**Symptom:** `ld-new` crashes on the first call into
any libiberty TU that references a `__thread`-declared
static.

**Cause:** on AArch64 those statics compile to
`mrs xN, tpidr_el0` instructions. The chapter-120 crt0
never initialised `tpidr_el0` — nothing in OSdev had
TLS users until now. Loads through `tpidr_el0`
returned the kernel-left value (usually 0), and the
first dereference faulted.

**Fix.** `crt0.S` sets `tpidr_el0` to a 64-byte
zero-initialised TLS block reserved in `.bss` before
calling `__libc_init_array` / `main`. That covers the
"thread-local statics zero-initialised, never written
from another thread" case binutils actually exercises.
The full POSIX `pthread_setspecific` story is left for
chapter 91 followups.

### Pitfall — `getopt` shadow

**Symptom:** `ld` swallows `/tmp/hello.o` as if it were
a long-option argument.

**Cause:** binutils' `libiberty/getopt.c` carries its own
`int optind;` tentative definition. The OSdev libc's
`userspace/libc/stdlib.h` also defined `static int
optind = 1;` in every TU that included it (per chapter
128e). When `ld-new`'s `lexsup.c` called `getopt_long`,
the version inside libiberty used libiberty's own
`optind` — but `ld_new_LDADD`'s other TUs saw the
`static` copy. The two copies disagreed about which
argv slot to look at next.

**Fix.** Extend chapter 131e's `OSDEV_LIBC_NO_GETOPT`
guard: `userspace/libc/stdlib.h` now wraps its entire
getopt block (decls + statics + getopt body) in
`#ifndef OSDEV_LIBC_NO_GETOPT`. The
`vendor/binutils-aarch64-osdev.patch` hunk 5 already
defines that macro at the top of `libiberty/getopt.c`,
so libiberty wins for vendor TUs; every other TU keeps
the file-local static copy as a fallback. The
`getopt_long` resolution then resolves cleanly
through libiberty's externs.

### Pitfall — `lrealpath` UB fall-through

**Symptom:** from the user's seat:

```
/bin/ld /tmp/hello.o -o /tmp/hello
/bin/ld: /tmp/hello.o: file format not recognized
```

...even though [scripts/test_bin_as.py](../../../scripts/test_bin_as.py)
is simultaneously passing 8/8 — `as` did write a valid
ELF to `/tmp/hello.o`, so `ld` is rejecting an object
it should accept.

**Cause.** Two days of progressive DBG-printf
instrumentation through `bfd/cache.c::cache_bread`,
`bfd/bfdio.c::bfd_read`, and `bfd/elfcode.h::elf_object_p`
showed `bfd_read` for `/tmp/hello.o` was returning
non-ELF bytes at offset 0. That instrumentation is
gone from the live source now but the diagnostic shape
is captured in /memories/repo for the next time
"file format not recognized" with a perfectly-good ELF
on disk shows up.

The actual cause lives one layer up.
`libiberty/lrealpath.c::lrealpath()` in the freestanding
build has all four of its bodies #if'd out: no
`realpath`, no `canonicalize_file_name`, no
PATH_MAX-bounded buffer, no Windows. Control falls off
the end of a non-void function. On AArch64 that returns
whatever happens to be in `x0` — which is the input
`filename` pointer (a slice of `argv[1]`).

`ld/lexsup.c` gets that pointer back from `lrealpath`
and calls `free()` on it (correctly assuming it was a
fresh `malloc`'d copy). That `free()` corrupts argv.
Subsequent `bfd_openr` calls see a mangled path. Then
the BFD cache returns a stale FILE* for some OTHER
recently-opened file, and `bfd_read` returns bytes that
were never part of `/tmp/hello.o`.

**Why strdup didn't paper over it.** lrealpath's other
return paths use `strdup()`. An early attempt inserted
`return strdup(filename);` at the bottom. It returned
NULL every time. Diagnosis: `strdup` in this build
resolves to `userspace/libc/cstring.c`'s global export
(per chapter 131e's libc-bridge pattern), which calls
its file-local K&R-style allocator. That allocator's
free-list head had never been touched by this process
(every other malloc in libiberty resolves to a
per-TU `static inline` copy from `malloc.h`). Its
first-call sbrk() path had a subtle interaction with
the running heap state and returned NULL.

**Fix.** Skip strdup; allocate via `extern void *malloc`
(which resolves to lrealpath.o's own static-inline
`malloc` from `malloc.h`, same heap shape every other
libiberty TU uses) and do a manual byte copy:

```c
if (!filename) return (char *)0;
{
  extern size_t strlen (const char *);
  extern void *malloc (size_t);
  size_t n = strlen (filename) + 1;
  char *r = (char *) malloc (n);
  if (!r) return (char *)0;
  for (size_t i = 0; i < n; i++) r[i] = filename[i];
  return r;
}
```

Captured as hunk 6 in
`vendor/binutils-aarch64-osdev.patch`.

### Pitfall — `tmpfs_write` ignored the fd offset

**Symptom:** with the lrealpath fix landed the test
still failed. `ld` again reported `file format not
recognized` on a `.o` that the assembler had just
written.

**Diagnosis.** `scripts/_dbg_dump_hello_o.py` (kept per
the debug-scripts policy) reads `data.img` directly off
the host, parses OSFS-2 inline, and dumps `/tmp/hello.o`.
The first few bytes:

```
0000: 00 24 78 00 5f 73 74 61 72 74 00 40 05 80 d2 48
```

ELF magic `7f 45 4c 46` does not appear until offset
**211**. The file contains every byte expected —
section data, string table, all seven section headers,
the ELF header — but laid out in the wrong order.

**Cause.** GNU `as` writes its output file in classic
fseek pattern: header at offset 0, sections in order,
then seeks back to offset 0 and overwrites the header
with the final form (now that section offsets/sizes are
known). That pattern only works if the underlying
filesystem honors the fd offset. OSdev's `tmpfs_write`
did not. It ignored the fd offset entirely and always
appended at `f->size`:

```c
/* OLD */
long tmpfs_write(int idx, const void *buf, size_t len)
{
    ...
    if (tmpfs_grow_to(f, f->size + len) != 0) return -12;
    for (size_t i = 0; i < len; i++)
        f->data[f->size + i] = src[i];
    f->size += len;
    return (long)len;
}
```

Until now `tmpfs_write` had only ever been driven by
the shell's `>` (open with O_TRUNC, write once at
offset 0) and `>>` (open with O_APPEND, write once at
offset = old size). Both of those happen to work with
an append-only implementation. GNU as is the first user
that actually seeks back.

**Fix.** Two coordinated edits:

```c
/* NEW: kernel/core/tmpfs.c */
long tmpfs_write(int idx, uint32_t offset,
                 const void *buf, size_t len)
{
    ...
    uint64_t need64 = (uint64_t)offset + (uint64_t)len;
    if (need64 > TMPFS_MAX_FILE_SIZE) return -28;
    if (tmpfs_grow_to(f, (uint32_t)need64) != 0) return -12;
    /* Sparse zero-fill of any gap. */
    if (offset > f->size)
        for (uint32_t i = f->size; i < offset; i++) f->data[i] = 0;
    for (size_t i = 0; i < len; i++)
        f->data[offset + i] = src[i];
    uint32_t end = (uint32_t)(offset + len);
    if (end > f->size) f->size = end;
    return (long)len;
}
```

```c
/* NEW: kernel/core/syscall.c (FD_TMPFS_RW write branch) */
long w = tmpfs_write(e->ramfs_index, (uint32_t)e->offset,
                     chunk, n);
if (w > 0) e->offset += (uint64_t)w;
```

That broke `>>` immediately (the test sweep regressed
to 2/10). The shell opens with O_APPEND and writes once
at the fd's initial offset; the fd's initial offset
was 0; positional write therefore overwrote the
existing contents from offset 0.

**Second fix.** `tmpfs_op_open` now sets the initial fd
offset based on the open flags:

```c
out->offset = (flags & O_APPEND)
              ? (uint64_t)g_files[tidx].size
              : 0;
```

This is enough for the shell's "single write per fd"
pattern. A full POSIX O_APPEND (re-seek to EOF on
every write of the same fd) would need an `is_append`
bit on `struct fd_entry`. No user demands that yet so
the slot is left for a future chapter.

With both fixes the test sweep returned to 12/12.

---

## What this unlocks

This is a tools-only chapter, but every existing app
that drives the cc/as/ld toolchain automatically picks
up the upgrade:

- **`/bin/cc`** ([userspace/cc/cc.c](../../../userspace/cc/cc.c))
  now drives the real GNU assembler and linker. Adding
  the `-T /bin/osdev.ld` arg was the only source change.
  Same `chapter 122` toolchain contract.
- **`/bin/make`** ([userspace/make/make.c](../../../userspace/make/make.c))
  unchanged. Its `cc → as → ld` recipes already worked
  via the toy versions and continue to work via GNU.
- **notepad Build button** (chapter 127) unchanged in
  source — Ctrl-B still goes notepad → make → cc → as →
  ld, with the as and ld arms now being the real
  binutils.
- **`scripts/test_notepad_build.py`** continues to PASS
  10/10: the end-to-end "type C in notepad, hit Build,
  run the result" loop now exercises GNU binutils.

## Run it / Test it

- Existing tests upgraded to the GNU pipeline:
  [test_bin_as.py](../../../scripts/test_bin_as.py),
  [test_bin_ld_ar.py](../../../scripts/test_bin_ld_ar.py)
  (docstrings rewritten to call out chapter 131f as
  the contract).
- Regression sweep covering the upgrade:
  [test_cc_hello.py](../../../scripts/test_cc_hello.py),
  [test_cc_vars.py](../../../scripts/test_cc_vars.py),
  [test_notepad_build.py](../../../scripts/test_notepad_build.py).
- New debug script kept per policy:
  [scripts/_dbg_dump_hello_o.py](../../../scripts/_dbg_dump_hello_o.py)
  — boots a guest, runs `as` on a tiny input, copies the
  output to `/data`, shuts down, parses the OSFS-2
  image on the host, dumps the raw bytes. This is the
  tool that surfaced the `tmpfs_write` trap ("ELF magic
  at offset 211").

---

## What's next

Chapter 132a brings up GNU `gcc-14.2.0` against the
same `aarch64-osdev` target triple. The libc-bridge
pattern from 131e and the `cstring.o`-as-LDADD trick
from this chapter carry forward unchanged.


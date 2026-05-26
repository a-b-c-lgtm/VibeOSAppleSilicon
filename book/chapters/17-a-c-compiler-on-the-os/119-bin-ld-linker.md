# Chapter 119 — A linker and an archiver: /bin/ld and /bin/ar

**Status:** Shipped. The second and third stages of our
in-guest toolchain. Code lives in
`userspace/ld/ld.c` (~570 lines) and
`userspace/ar/ar.c` (~250 lines), plus a small kernel patch
to [kernel/core/tmpfs.c](../../../kernel/core/tmpfs.c) so
linked outputs in `/tmp` can actually be exec'd.

## Why this chapter exists

[Chapter 118](118-bin-as-assembler.md) gave us `/bin/as`,
which turns one `.s` text file into one ELF64-LSB
relocatable (`ET_REL`). That's an object file. Objects
can't be exec'd: they have no entry point and no program
headers, and they leave forward references unresolved.

A linker is the program that:

1. Reads several `ET_REL` objects.
2. Concatenates each named section across them
   (every input's `.text` end-to-end into one output
   `.text`, same for `.rodata` / `.data` / `.bss`).
3. Merges their symbol tables, deciding for each
   global symbol which input owns the definition.
4. Walks every relocation record produced by chapter
   118 and patches the merged section bytes so that
   addresses and `bl` targets refer to the final
   layout, not the per-input layout the assembler
   guessed at.
5. Wraps everything in a single `ET_EXEC` ELF64 file
   with `PT_LOAD` program headers describing how
   each segment should be mapped at runtime.
6. Sets `e_entry` to a chosen symbol.

That output is what the kernel ELF loader (chapter 22 +
[kernel/core/elf.c](../../../kernel/core/elf.c)) maps when
the shell calls `spawn()`.

`/bin/ar` is the trivial sibling. It bundles `.o` files
into a `.a` archive. We don't need archive scanning in
`/bin/ld` yet — chapter 118's tests link individual
`.o` files — but the bookkeeping is so cheap (one file,
~250 lines) that we ship `ar` in the same chapter so the
toolchain can quote `libfoo.a` filenames if a future
compiler ever wants them.

## What shipped

| Component | File | Lines | Role |
|---|---|---|---|
| `/bin/ld` | [userspace/ld/ld.c](../../../userspace/ld/ld.c) | ~570 | The linker proper. |
| `/bin/ar` | [userspace/ar/ar.c](../../../userspace/ar/ar.c) | ~250 | SysV ar archiver. |
| tmpfs exec gap fix | [kernel/core/tmpfs.c](../../../kernel/core/tmpfs.c) | ~25 added | `tmpfs_op_load` so `/tmp/hello` can be exec'd. |
| Build wiring | [Makefile](../../../Makefile) | new `LD_OBJS` / `AR_OBJS` rules | Compiles, links, strips, stages on disk. |
| Smoke test | [scripts/test_bin_ld_ar.py](../../../scripts/test_bin_ld_ar.py) | ~180 | End-to-end: assemble, link, run, archive, list. |

Both binaries reuse the chapter-118 plumbing:

- `elf_write.h` typedefs and constants
  (`ew_ehdr64_t`, `ew_shdr64_t`, `ew_sym64_t`,
  `ew_rela64_t`, the reloc-type names like
  `R_AARCH64_CALL26`).
- The same `*_realloc` shim that reads
  `malloc.h`'s block header to discover the old
  payload size and copy it to a newly-allocated
  buffer.
- The same `memset` / `memcpy` 5-liners to dodge
  the freestanding-memset trap (see
  [/memories/freestanding-c-memset-trap.md](https://example.invalid)
  for the user-memory note this discipline came from).

## The linker, end to end

### Inputs

`ld -o out.elf [-e entry] file1.o [file2.o ...]`

`-e` defaults to `_user_start` (our crt0 convention)
with `_start` as the fallback if `_user_start` isn't
defined. `-o` is required.

Each input file is read fully into a `malloc`'d buffer
and recorded in a fixed-size `g_in[LD_MAX_INPUTS]`
table (`LD_MAX_INPUTS = 16`). The ELF header is
checked verbatim:

```
EI_MAG       == { 0x7F, 'E', 'L', 'F' }
EI_CLASS     == ELFCLASS64
EI_DATA      == ELFDATA2LSB
e_type       == ET_REL
e_machine    == EM_AARCH64
```

Section names are looked up via `.shstrtab` (the
section whose index lives in `e_shstrndx`) and the
six sections we care about are remembered by their
`shdr` index inside the input:

```
.text  .rodata  .data  .bss  .symtab  .strtab  .rela.text
```

### Merge symbol tables

For each input, every `STB_GLOBAL` symbol gets
copied into `g_gsyms[LD_MAX_SYMS]`. The merge rule is
**first defined wins, double-definition is an error**.
This is enough for a hand-written C corpus; real `ld`
has weak symbols, COMDAT groups, and version scripts,
none of which the assembler in chapter 118 can produce
yet so none of which we need to model.

`STB_LOCAL` symbols stay inside their input's `.symtab`.
They never enter `g_gsyms`. Their values resolve
relative to whichever section the input itself put
them in.

### Layout

Two passes:

1. **Per-section concatenation.** For each section
   kind `k ∈ {TEXT, RODATA, DATA, BSS}`, walk every
   input, honour each input section's `sh_addralign`,
   place the input's section at the current
   `g_sec_size[k]` cursor, and bump the cursor.
   `g_in[i].out_off[k]` records where input `i`'s
   piece landed within the merged section.

2. **File / VA placement.** The output file looks like:

   ```
   +---------------------+ file offset 0,  VA LD_LOAD_BASE
   |  Elf64_Ehdr (64 B)  |
   |  Elf64_Phdr × 2     |  (each phdr = 56 bytes)
   +---------------------+
   |  ...gap to 4K...    |
   +---------------------+ page-aligned
   |  .text              |  PROGBITS, mapped R+X
   |  .rodata            |  PROGBITS, mapped R+X (same PT_LOAD)
   +---------------------+ page-aligned
   |  .data              |  PROGBITS, mapped R+W
   |  .bss (no file bytes)| NOBITS,   memsz > filesz on PT_LOAD #2
   +---------------------+
   ```

   `LD_LOAD_BASE = 0x1000100000ULL` — the same VA
   `userspace/linker_user.ld` puts everything our host
   toolchain produces, so all our other binaries
   already live here.

   Two `PT_LOAD` phdrs are emitted. The first covers
   `[file 0 .. .rodata end)` with VA == `LD_LOAD_BASE`,
   flags `PF_R|PF_X`. The second covers
   `[.data start .. .data end)` with `p_memsz = .data + .bss`
   and flags `PF_R|PF_W`. The headers themselves ride
   the front of the first segment, exactly like
   binutils `ld` does it.

### Resolve and apply relocations

For each input that has a `.rela.text` section, walk
the array of `ew_rela64_t` entries. Each one has:

- `r_offset`: byte offset inside this input's `.text`
- `r_info = (sym_idx << 32) | reloc_type`
- `r_addend`: signed addend baked into the patch

The destination VA in the output is
`g_sec_va[TEXT] + in->out_off[TEXT] + r_offset`. The
source VA is whatever `resolve_input_sym()` returns
for `sym_idx`:

- `STT_SECTION` → the VA of the input's section.
- `STB_GLOBAL` → look up in `g_gsyms`; emit an
  `undefined reference` error if not found.
- `STB_LOCAL` → resolve against the input's own
  section + `st_value`.

Three reloc types are supported. They cover everything
chapter 118's assembler can emit:

| Reloc | Patch action |
|---|---|
| `R_AARCH64_ABS64` (257) | Store `S + A` as a little-endian `u64` at `r_offset`. |
| `R_AARCH64_CALL26` (283) | Patch the low 26 bits of the `bl` instruction with `((S + A − P) >> 2) & 0x03FF_FFFF`. |
| `R_AARCH64_JUMP26` (282) | Same encoding as CALL26 but for `b`. |

`S` is the symbol's final VA, `A` is `r_addend`,
`P` is the patch site's VA. The instruction word is
fetched and stored little-endian.

### Emit the file

The final output is one contiguous `g_out[]` buffer.
The ehdr fills the first 64 bytes, the two phdrs the
next 112, then the data is written into
`g_sec_foff[k]` slots and finally `write(fd)`'d as a
single block. `e_entry` is set to the resolved VA of
the entry symbol (`_user_start` by default).

## /bin/ar in 250 lines

The SysV ar format is gloriously simple:

```
+----------------+
| "!<arch>\n"    |    8-byte global magic
+----------------+
| 60-byte header |    ASCII fields, all space-padded:
|                |      name  16  ('foo.o/' terminator)
|                |      mtime 12  ("0")
|                |      uid    6  ("0")
|                |      gid    6  ("0")
|                |      mode   8  ("100644")
|                |      size  10  decimal payload size
|                |      end    2  "`\n"  (backtick + LF)
+----------------+
| member bytes   |    raw .o payload
| (pad to even   |
|  with '\n')    |
+----------------+
... repeats per member ...
```

Long names (>15 bytes) use the BSD `#1/NN`
extension: the header `name` field is literally
`#1/NN` where `NN` is the real length, and the
member payload begins with `NN` bytes of name
followed by the actual `.o` bytes. We pick this
extension because it doesn't require a separate
`//` long-name string-table member.

Two commands:

- `ar rc out.a a.o b.o ...` — create / replace.
  Reads each input fully, writes header + payload,
  prints `ar: wrote out.a (N members)`.
- `ar t out.a` — list. Walks the file, prints
  each member name, finishes with `ar: N members`.

No `SYMDEF` index, no incremental update, no
deletion. `/bin/ld` doesn't consume archives yet
(chapter 121 will), so the missing index is fine.

## The tmpfs exec gap

The first smoke-test run came back **11 PASS / 1 FAIL**.
`/bin/ld` produced a valid 8 KiB ELF and exited 0, but
the next line of the test —
`spawn("/tmp/hello", ...)` — failed with
`[sh] no such command: /tmp/hello (errno=2)`.

`vfs_load()` in
[kernel/core/vfs.c](../../../kernel/core/vfs.c)
dispatches to a mount's `.load` op via the
filesystem vtable. The tmpfs vtable had:

```c
const struct fs_ops tmpfs_fs_ops = {
    .open    = tmpfs_op_open,
    .read    = tmpfs_op_read,
    ...
    .load    = NULL,         /* <-- the gap */
};
```

So tmpfs could be `cat`'d but never `exec`'d. The
shell never bothered to ask the loader because the
loader could refuse without anyone noticing —
until chapter 119 became the first thing that
*wrote* a binary to `/tmp` and immediately tried
to run it.

Fix is 25 lines. New `tmpfs_op_load` reads the
file into a fresh `kmalloc` buffer, sized by
`tmpfs_size_of()`, populated by `tmpfs_read()`,
and hands `(buf, size)` back to `vfs_load`:

```c
static long tmpfs_op_load(void *cookie, const char *rel,
                          uint8_t **out_buf, size_t *out_size)
{
    const char *bare = tmpfs_strip_slash(rel);
    if (!*bare) return -ENOENT_VFS;
    int idx = tmpfs_lookup(bare);
    if (idx < 0) return -ENOENT_VFS;
    uint32_t sz = tmpfs_size_of(idx);
    uint8_t *buf = kmalloc(sz ? sz : 1);
    if (!buf) return -ENOMEM_VFS;
    long got = tmpfs_read(idx, 0, buf, sz);
    if (got < 0 || (uint32_t)got != sz)
        { kfree(buf); return -EINVAL_VFS; }
    *out_buf = buf;
    *out_size = (size_t)sz;
    return 0;
}
```

After re-mounting the vtable's `.load` slot to this
function, `/tmp/hello` runs and the test goes
**12 PASS / 0 FAIL**.

The bigger lesson: `vfs_load` is a separate
permission gate from `vfs_open`. Filesystems that
want to host executables have to opt in. We will
need the same op on every new filesystem from now
on (the persistence layer in chapter 84 already
has it; the userfs proxy in chapter 113 still
doesn't and is a future trap).

## Lessons

### `s_eq` is not `strcmp`

We define this little prefix-vs-equal predicate in
several toolchain files now:

```c
static int s_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
```

It returns 1 iff the strings are *equal*, not iff one
prefixes the other. It's correct because it advances
both pointers in lock-step and only succeeds when
both hit `'\0'` simultaneously. The chapter-118
assembler uses the same pattern. If you ever swap it
out for a `strncmp` you will introduce a bug where
`.text2` is treated as `.text`.

### `malloc.h`-aware realloc

Our header-only allocator has no `realloc`. Both
`/bin/as` and `/bin/ld` use the same trick to grow
buffers:

```c
size_t old_total = *((size_t *)((char *)p - UALLOC_HDR_SIZE));
size_t old_payload = old_total - UALLOC_HDR_SIZE;
```

The 8 bytes immediately before any allocation hold
the total block size including the header. Subtract
the header to get the usable payload, then `malloc`
+ copy + `free`. This is exactly how `realloc`
would be implemented atop a sized-block allocator.

### Default entry symbol with two-name fallback

`-e` defaults to `_user_start` but falls back to
`_start` if that isn't defined. That's because our
crt0 calls everything `_user_start` (chapter 22 ELF
loader expects it) but a tiny hand-written `.s`
test program is more likely to call its entry
`_start`. The two-name fallback means a programmer
can write either and the test will exec without
needing to remember which convention.

## Applied to

- **Existing apps:** none. `/bin/ld` produces
  output that runs alongside everything else
  today, but no shipped app has source files
  it would relink. They are built by the host
  toolchain and staged on disk via mkosfs2.
- **New apps:** `/bin/ld`, `/bin/ar`.
- **New tests:** `scripts/test_bin_ld_ar.py`.
- **Kernel change applied to:** every future
  binary the in-guest toolchain produces.
  Without `tmpfs_op_load` you can compile but
  not run. Chapter 121's `/bin/cc` and chapter
  124's first native compile both depend on this.

The first existing app that will actually be
relinked from source is gated on chapters 120
(crt0 + libgcc-style stubs) and 121 (`/bin/cc`).
Once we have a compiler in the guest, the natural
first target is `notepad` — small, single-TU, no
network — and chapter 127's "Build" button is
the GUI surface for that loop.

## What gets exercised in tests

[scripts/test_bin_ld_ar.py](../../../scripts/test_bin_ld_ar.py)
boots the OS, then:

1. Stages a 5-line `hello.s` into `/tmp/hello.s`
   line-by-line via `echo >>` (still no shell
   heredoc).
2. Runs `/bin/as /tmp/hello.s -o /tmp/hello.o`
   and byte-checks the result: ELF magic,
   `EM_AARCH64`, the encoded `MOVZ x0,#42` and
   `MOVZ x8,#2` words are present.
3. Runs `/bin/ld /tmp/hello.o -o /tmp/hello -e _start`
   and byte-checks the linked image (same
   instruction words, `ET_EXEC` shape).
4. **Runs `/tmp/hello` and asserts exit code 42.**
   This is the assertion that surfaced the tmpfs
   gap.
5. Runs `ar rc /tmp/libhello.a /tmp/hello.o`,
   checks `!<arch>\n` magic.
6. Runs `ar t /tmp/libhello.a`, asserts that the
   listing names `hello.o` and reports
   `1 members`.

Final result: **12 PASS / 0 FAIL**.

The chapter-119 regression sweep was 18 tests
(chapter-118's 17 plus the new `test_bin_ld_ar`):
**18 PASS / 0 FAIL**.

## Deferred

Things real `ld` does that ours doesn't, with the
chapter where they will land:

- `adrp + add` pair relocations
  (`R_AARCH64_ADR_PREL_PG_HI21` /
  `R_AARCH64_ADD_ABS_LO12_NC`) — needed once any
  in-guest compiler emits PIE-style address
  computation for globals.
- Archive scanning — pull only the members that
  satisfy unresolved symbols. Lands the day a
  later compiler ships a real `libc.a`.
- GOT / PLT / TLS — not on the roadmap; we don't
  link shared objects.
- Linker scripts — current layout is hard-coded
  to match `userspace/linker_user.ld`. A `-T`
  parser is a follow-up if anyone needs a custom
  layout.
- `/bin/nm` and `/bin/strip` — convenience tools
  that share `ld`'s symbol-table reader. Not
  needed by `/bin/cc`, so deferred indefinitely.

## Next

[Chapter 120](120-crt0-and-libgcc-stubs.md) gives
`/bin/ld` something interesting to link: a real
in-guest crt0 and a tiny set of `libgcc`-style
helper routines (`memset`, `memcpy`, `__popcountdi2`,
…) so the next compiler (`/bin/cc`, chapter 121)
has the runtime it expects.

# Chapter 118 — An AArch64 assembler: /bin/as

> **Milestone in this chapter:** ship the first toolchain stage
> that runs inside the OS — a small AArch64 assembler at
> `/bin/as`.
> **Code referenced:**
> - [userspace/as/as.c](../../../userspace/as/as.c) (~770 LoC)
> - [userspace/libc/elf_write.h](../../../userspace/libc/elf_write.h)
>   (~210 LoC, header-only)
>
> **At the end of this chapter** you will have a `/bin/as`
> that turns an `.s` file into a relocatable `.o`, the first
> link in the chapter-122 `cc → as → ld` pipeline. Builds on
> chapter 117 (stat / fcntl / dirent) and chapter 116b
> (stdio).

## Why this chapter exists

GCC emits assembly text. It does not emit object files. The
`gcc` driver invokes `as` to turn each `.s` into a `.o` and
then `ld` to combine them into an executable. If we want a
compiler running on the OS — the small `/bin/cc` of
[Chapter 121](121-bin-cc.md), or a future GCC port —
we need `as` and `ld` running there first.

Of the two, `as` is the harder one. It has to:

1. Parse a slightly unforgiving line-based grammar
   (mnemonics, directives, labels, comments, escapes).
2. Encode 32-bit AArch64 instruction words by hand. There is
   no library to call.
3. Emit a real ELF64-LSB relocatable object that the linker
   we write in [Chapter 119](119-bin-ld-ar.md) can consume.
4. Track symbols, sections, and relocations so the linker
   can fix up forward references it can't see.

A real assembler (binutils `gas`) is 150,000+ lines. We are
not building a real assembler. We are building the smallest
assembler that can consume the output of a small C compiler
and turn it into linkable objects. That is roughly 20–30
mnemonics plus a dozen directives, hand-encoded, one pass,
with patch lists for forward refs.

This chapter delivers exactly that. It does not implement
macros, conditional assembly, expressions beyond `+ -` of two
ints, or floating-point.

## What this chapter adds

| Component | File | Lines | Role |
|---|---|---|---|
| ELF64 writer | `userspace/libc/elf_write.h` | ~210 | Header-only. Typedefs, `ew_buf_t` growable buffer, ehdr/shdr/sym/rela emit helpers, reloc-type constants. |
| Assembler   | `userspace/as/as.c`          | ~770 | One-pass lexer + encoder + patch list + ELF emitter. Provides local `memset`/`memcpy` and `as_realloc`. |
| Smoke test  | `scripts/test_bin_as.py`     | ~155 | Boots OS, stages `/tmp/hello.s`, runs `/bin/as`, verifies ELF magic + AArch64 machine + encoded MOVZ/RET bytes. |

## Mnemonic coverage

Curated subset. Each line below also lists the exact AArch64
encoding the assembler emits. Bit layouts are derived from
the Arm Architecture Reference Manual (ARM DDI 0487).

| Mnemonic | Form | Encoding |
|---|---|---|
| `mov Xd, #imm` | wide immediate | MOVZ `1101 0010 1 00 imm16 Rd` (`0xD2800000 \| imm<<5 \| Rd`) |
| `mov Xd, Xn` | reg-to-reg | ORR `Xd, XZR, Xn` shifted-reg form |
| `movz Xd, #imm{, lsl #hw*16}` | full MOVZ | `1101 0010 1 hw imm16 Rd` |
| `movk Xd, #imm{, lsl #hw*16}` | keep bits | `1111 0010 1 hw imm16 Rd` |
| `add/sub Xd, Xn, #imm12` | imm | `1xx1 0001 00 imm12 Rn Rd` (x=sf+op) |
| `add/sub Xd, Xn, Xm` | reg | `1xx0 1011 000 Rm 000000 Rn Rd` |
| `cmp Xn, #imm` | alias of SUBS XZR, ... | imm form |
| `cmp Xn, Xm` | alias of SUBS XZR, ... | reg form |
| `ldr Xt, [Xn, #imm]` | unsigned offset | `1111 1001 01 imm12/8 Rn Rt` |
| `str Xt, [Xn, #imm]` | unsigned offset | `1111 1001 00 imm12/8 Rn Rt` |
| `b label`  | unconditional branch | `0001 01 imm26` (CALL26-style) |
| `bl label` | branch-link | `1001 01 imm26` |
| `br Xn`    | branch to reg | `1101 0110 0001 1111 0000 00 Rn 00000` |
| `ret`      | return | `0xD65F03C0` |
| `nop`      | no-op | `0xD503201F` |
| `wfe`      | wait-for-event | `0xD503205F` |
| `svc #imm16` | supervisor call | `1101 0100 000 imm16 00001` |

W-register variants of `mov` / `add` / `sub` use the same
encoders with the `sf` bit cleared. The assembler also
accepts `xzr` / `wzr` as register 31.

## Directives

| Directive | Effect |
|---|---|
| `.text` / `.data` / `.bss` / `.rodata` | switch current section |
| `.section NAME [, "flags"]` | switch to one of the four known names |
| `.global` / `.globl SYM` | mark `SYM` as `STB_GLOBAL` |
| `.balign N` / `.align N` / `.p2align N` | pad current section to alignment |
| `.byte`, `.word` / `.long` / `.4byte`, `.quad` / `.8byte` | emit bytes / 4 / 8 bytes |
| `.ascii "..."`, `.asciz "..."`, `.string "..."` | emit string literal (with `\n \t \0 \\ \"`) |
| `.skip` / `.space` / `.zero N` | reserve N zero bytes |
| `.type`, `.size`, `.cfi_*`, `.loc`, `.file` | parsed and ignored |

The "parsed and ignored" set is what GCC emits unconditionally
even when nothing useful would come of it. Silently dropping
them is the simplest way to be GCC-compatible without owning
the full ELF debug-info pipeline.

## Section layout

The output object always has the same fixed section index
plan. The linker in [Chapter 119](119-bin-ld-ar.md) relies
on this:

| Index | Section | Type | Notes |
|---|---|---|---|
| 0 | (null) | `SHT_NULL` | required by ELF |
| 1 | `.text` | `PROGBITS` | `SHF_ALLOC \| SHF_EXECINSTR` |
| 2 | `.data` | `PROGBITS` | `SHF_ALLOC \| SHF_WRITE` |
| 3 | `.bss`  | `NOBITS`  | `SHF_ALLOC \| SHF_WRITE`, payload size only |
| 4 | `.rodata` | `PROGBITS` | `SHF_ALLOC` |
| 5 | `.symtab` | `SYMTAB` | links to strtab=6, info=first-global |
| 6 | `.strtab` | `STRTAB` | symbol names |
| 7 | `.shstrtab` | `STRTAB` | section header names |
| 8 | `.rela.text` | `RELA` | only present if relocations were kept; link=5, info=1 |

The symbol table opens with the reserved `STN_UNDEF`, then
section symbols (`STT_SECTION`, `STB_LOCAL`) for each of the
four user sections in order, then real labels — locals
first, then globals. The `sh_info` field of `.symtab` is
the index of the first global, as ELF requires.

## Relocation strategy

The assembler is one-pass. Forward references to labels are
unavoidable. We deal with them two different ways:

### In-place patch for resolved intra-`.text` branches

When we emit a `b label` or `bl label` and the target label
is later defined in the same `.text` section, we don't
actually need a relocation in the object — the offset is
fully known by end of pass. So during `write_elf()` we walk
the reloc list once, and for any `R_AARCH64_CALL26` /
`R_AARCH64_JUMP26` whose target symbol is defined and
shndx-equals `.text`, we patch the 26-bit signed word
offset into the already-emitted instruction in place and
drop the reloc record.

### `.rela.text` for everything else

External symbols (`bl printf`), data references
(`adrp/add x0, msg`), and `.quad sym` words become real
`Elf64_Rela` records with type:

| Reloc | Type # | Used by |
|---|---|---|
| `R_AARCH64_ABS64` | 257 | `.quad sym` |
| `R_AARCH64_CALL26` | 283 | `bl extern_func` |
| `R_AARCH64_JUMP26` | 282 | `b extern_label` |
| `R_AARCH64_ADR_PREL_PG_HI21` | 275 | `adrp` (not yet wired through encoder) |
| `R_AARCH64_ADD_ABS_LO12_NC` | 277 | matching `add` for adrp/add pair |

The linker in [Chapter 119](119-bin-ld-ar.md) is the one that
consumes these.

## A few lessons learned during the build

### The stack-overflow trap

First test run faulted at EL0 inside `write_elf` with a Data
Abort on a stack address. `aarch64-elf-objdump` showed the
function prologue subtracting *~100 KiB* from SP:

```
sub sp, sp, #0x9f0
sub sp, sp, #0x18, lsl #12   ; 0x18000 = 98304 bytes
```

Our user stack is `USER_STACK_PAGES * PAGE_SIZE` = 16 × 4096
= **64 KiB** (`kernel/core/elf.c:76`). The first STP into
the new SP touched an unmapped page below the stack and
took a translation fault.

The culprit was one local: `reloc_t resolved[MAX_RELOCS]`
where `MAX_RELOCS = 4096` and `sizeof(reloc_t) ≥ 16`. That
single array is ~64 KiB. Moving it to file scope
(`static reloc_t g_resolved[MAX_RELOCS];`) dropped the
prologue to `sub sp, sp, #0x9f0` = 2544 bytes and the test
passed.

This is now a written-down convention for the future
toolchain code: **any per-function array indexed by one of
the global `MAX_*` constants belongs in `.bss` at file
scope, not on the user stack**.

### The freestanding `memset` trap (again)

GCC at `-Os` lowered every `ew_shdr64_t s = {0};` (and
that's *ten* of them in `write_elf`) into a `memset` call.
Freestanding userspace has no libc `memset`, so the link
failed with ten `relocation truncated to fit:
R_AARCH64_CALL26 against undefined symbol 'memset'`.
This is the same freestanding-C `{ 0 }` initialiser trap,
so the fix was rote — add a tiny `memset` and `memcpy` in
the same TU. Both are five lines each.

### The `ew_realloc` hook

The header-only `elf_write.h` needs to grow its buffers, so
it declares `void *ew_realloc(void *p, size_t n);` and lets
the host TU provide it. `userspace/as/as.c` implements it
on top of our `malloc.h` free-list allocator by reading the
hidden block size header (`*((size_t *)(p - UALLOC_HDR_SIZE))`),
allocating a new block, byte-copying, and freeing the old
one. Crude but correct, and it keeps `elf_write.h` from
having to know how our libc allocator stores its bookkeeping.

This pattern (header-only library declares a hook, host TU
provides it) is the same one we used for the printf sink in
[Chapter 116b](116b-stdio-FILE-printf.md). We'll reuse it
once more in [Chapter 119](119-bin-ld-ar.md) for `/bin/ld`.

## Applied to

There is no existing app for `/bin/as` to replace — this is a
new capability. The first consumer is the linker in
[Chapter 119](119-bin-ld-ar.md). The second is the tiny
compiler driver in [Chapter 121](121-bin-cc.md), which will
spawn `/bin/as` per `.s` file just like the host driver does.

The `scripts/test_bin_as.py` smoke test exercises the
end-to-end path: shell stages a `.s` file, `/bin/as`
processes it, output is read back and its bytes inspected.
Asserts the ELF magic, ELFCLASS64 + ELFDATA2LSB + EV_CURRENT
ident block, `e_machine == EM_AARCH64`, the encoded
`MOVZ x0, #42` word (`0xD2800540`), and the encoded `RET`
word (`0xD65F03C0`) all appear in the output.

## What gets exercised in tests

Added to the regression baseline:

```
test_bin_as       smoke + byte-level ELF check
```

Full sweep result after this chapter: **17 PASS / 0 FAIL out
of 17** (chapter 117 baseline + `test_bin_as`).

## What is deferred to later chapters

- `adrp` / `adr` encoding. We emit the relocation types but
  the parser doesn't accept those mnemonics yet.
- Conditional branches (`b.eq`, `cbz`, `tbz`). Not needed
  until a compiler emits them — our small `/bin/cc` does
  not yet, and a real GCC port (Part XVIII) would.
- Macros (`.macro`), conditionals (`.if/.endif`),
  expressions beyond bare integers.
- Floating-point register classes and FP mnemonics.
- DWARF `.debug_*` sections. We accept the directives but
  drop them.
- Multiple-file source (`#APP` / `#NO_APP`, multi-file
  passes). One source file per invocation.

These are all things real `as` does. We will revisit them
if a specific later compiler invocation requires them.

## Next chapter

[Chapter 119 — /bin/ld and /bin/ar](119-bin-ld-ar.md) takes
the `.o` files we just learned to emit and turns them into
ET_EXEC executables and ar archives. The relocation engine
reuses our `R_AARCH64_*` constants and the same
`elf_write.h` typedefs.

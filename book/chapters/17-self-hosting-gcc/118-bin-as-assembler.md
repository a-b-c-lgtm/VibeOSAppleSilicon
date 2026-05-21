# Chapter 118 — An AArch64 assembler: /bin/as

**Status:** Stub. Tracking the toolchain milestone "native
assembler". See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

GCC emits assembly. It does not emit object files. The
`gcc` driver spawns `as` to turn each `.s` into a `.o`,
then `ld` to combine them. If we want GCC running on the
OS (chapters 122–124), we need `/bin/as`.

TinyCC sidesteps this — it has its own assembler built
in, and chapter 121's TCC port uses that internal path.
But the moment we want GCC, we want `/bin/as`. Writing
one is also a satisfyingly bounded project: GCC emits
only ~80 of the ~1000 AArch64 mnemonics, and only
needs a handful of relocations.

## What this chapter adds

- `userspace/as/as.c` — a one-pass-with-fixups
  AArch64 assembler that consumes `gcc -S` output and
  emits ELF64 relocatable objects.
- An ELF64 writer header (`userspace/libc/elf_write.h`)
  reused by `/bin/ld` in chapter 119 and the runtime
  builders in chapter 120.
- An AArch64 instruction encoder table for the subset
  GCC actually emits (the "GCC-emitted" subset is
  ~250 encoding rules; we list it explicitly).
- `scripts/test_as_minimal.py`: assembles a hand-
  written `hello.s`, runs the result through the host
  `aarch64-elf-objdump` for sanity (cross-check), and
  also feeds it to chapter 119's `/bin/ld` once that
  lands.

## Prerequisites

- Chapter 117 — `fopen`, `creat`, `stat` for input/
  output handling.
- Chapter 22 — the existing ELF reader (we mirror its
  layout knowledge into a writer).

## Plan

1. Lexer: identifiers, numbers (decimal/hex/octal),
   strings, the `.directive` family. ~120 lines.
2. Symbol table: a flat array, since GCC's output has
   ~hundreds of symbols per `.s` (not thousands).
3. Encoder tables, one row per mnemonic:
   `{ name, opcode, operand_class[] }`. The hard ones
   (`adrp` + `add` LO12 pair, `b`/`bl` with 26-bit
   PC-relative displacement, `ldr`/`str` immediate
   shifts) get their own helpers; the rest fit in a
   single dispatch loop.
4. Two passes only — pass 1 lays out sections and
   discovers symbol values; pass 2 emits bytes and
   records relocations against unresolved symbols.
5. Relocation set we need: `R_AARCH64_ABS64`,
   `R_AARCH64_CALL26`, `R_AARCH64_JUMP26`,
   `R_AARCH64_ADR_PREL_PG_HI21`,
   `R_AARCH64_ADD_ABS_LO12_NC`, and the four
   `R_AARCH64_LDST{8,16,32,64}_ABS_LO12_NC` variants.
   That's it — the full table has 200+ entries; GCC
   plus our libc use these eight.
6. Section layout matches the existing kernel ELFs:
   `.text` (R+X), `.rodata` (R), `.data` (RW),
   `.bss` (RW, NOBITS).

## What you'll learn

- Why the AArch64 ISA is so much easier to assemble
  than x86 — every instruction is 32 bits, decoding
  is bit-field extraction, and there is one
  addressing mode per instruction (vs x86's dozens).
- The PC-relative addressing dance (`adrp` +
  `add :lo12:`) that every position-independent
  load on aarch64 uses.
- Why a relocatable object file is *exactly* the same
  format as an executable, with one section missing
  (PHDRs) and one section added (relocations + a
  symbol table that hasn't been resolved yet).

## What this unlocks

- Chapter 119 (`/bin/ld`) — once we can produce `.o`
  files inside the OS, the linker has something to
  link.
- Chapter 122 — GCC's driver can dispatch to
  `/bin/as` instead of needing its own assembler.

## Applied to

- **Existing apps:** none — this chapter adds
  brand-new functionality.
- **New apps:** `/bin/as`, and a tiny `/bin/dis`
  (one-page disassembler harness around our encoder
  table, used as a sanity-checker during bring-up).
- **New tests:** `scripts/test_as_minimal.py`,
  `scripts/test_as_relocs.py` (asserts every
  relocation type we emit round-trips through
  `objdump -r`).

## Risks called out for the implementation

- We're tying ourselves to AArch64 forever. Fine —
  the book is AArch64-only by its reader contract
  (see INDEX.md). No portability layer.
- GCC sometimes emits assembler directives we don't
  recognise (`.cfi_*`, `.loc`, `.file`). Plan: accept-
  and-ignore for all of them. Real DWARF support is
  a follow-up chapter, not in this section's scope.

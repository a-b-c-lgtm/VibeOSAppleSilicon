# Chapter 119 — An AArch64 linker: /bin/ld and /bin/ar

**Status:** Stub. Tracking the toolchain milestone "native
linker". See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

`/bin/as` (chapter 118) produces relocatable `.o` files.
Each one references symbols defined in other `.o` files
and in libc. `/bin/ld` is what resolves those references
and lays the result out as an ELF executable our existing
ELF loader (chapter 22) will accept.

`/bin/ar` is the trivial sibling: it bundles multiple
`.o` files into a `.a` archive so `ld` can pull only the
ones it actually needs. Without `ar`, libc is one
monolithic `.o` and every "hello world" links against
the entire library.

## What this chapter adds

- `userspace/ld/ld.c`: a static linker. Reads `.o` and
  `.a` inputs in command-line order; resolves symbols;
  applies the relocations chapter 118 emits; lays out
  segments; emits an ELF64 executable matching the
  layout our ELF loader expects.
- `userspace/ar/ar.c`: an archive packer/unpacker in
  the System V `ar` format (the same format every Unix
  has used since 1971). Two commands: `ar rcs lib.a
  *.o` and `ar t lib.a`. That's all GCC needs.
- A built-in linker script. We don't take user `.ld`
  files in this chapter — the layout is hard-coded to
  match `linker/kernel.ld`'s userspace twin. (A real
  `-T script.ld` parser can land as a follow-up if a
  reader needs custom layouts.)
- `scripts/test_ld_hello.py`: assembles a one-line
  `main` with our `/bin/as`, links it with `/bin/ld`
  against a tiny `crt0.o` + `libc.a`, runs the
  resulting binary on the OS, checks exit code.

## Prerequisites

- Chapter 118 — `/bin/as` (provides the `.o` files we
  link).
- Chapter 22 — the existing ELF loader (our output
  format target).
- Chapter 25 — kernel/user boundary (we link against
  the same userspace ABI the rest of the system uses).

## Plan

1. Symbol resolution: two passes. Pass 1 reads every
   `.o`'s symbol table, builds a global hash. Pass 2
   walks `.a` archives and pulls in any member that
   defines an unresolved symbol (iterate until no new
   symbols are pulled — classic archive-link semantics).
2. Layout: place `.text` from each `.o` end-to-end into
   one output `.text`, same for `.rodata` / `.data`.
   `.bss` is collected and placed last in its own
   segment with `p_filesz < p_memsz`.
3. Apply relocations from chapter 118's relocation set.
   Each one is 5–15 lines of bit manipulation; we share
   helpers with `/bin/as`.
4. Emit PHDRs: one LOAD R+X for `.text` + `.rodata`,
   one LOAD RW for `.data` + `.bss`, plus the existing
   GNU_STACK note our ELF loader looks at.
5. `ar` is one source file. The format is plain ASCII
   headers ("!<arch>\n", then per-member 60-byte
   headers) followed by raw `.o` payloads padded to
   even bytes.

## What you'll learn

- Why GNU `ld`'s manual is 400 pages and ours is 4
  paragraphs: 95% of real `ld` is variants on these
  same steps, plus a Turing-complete scripting
  language we are not implementing.
- The archive trick — `.a` files don't get
  *linked-in-whole*; only the members that satisfy
  unresolved symbols are pulled. That's why
  forgetting `-lm` works for half the programs that
  use `sqrt` and breaks the other half.
- Why every relocation type fits into one of three
  shapes: "patch a value", "patch a PC-relative
  offset", "patch a paired hi/lo pair".

## What this unlocks

- The full toolchain is in `/bin/`: `as`, `ld`, `ar`.
  Chapter 121's TCC port now has a backstop — if
  TCC's internal linker breaks, we can debug against
  our `ld`.
- Chapter 122 — GCC's driver dispatches to `/bin/ld`
  by name.
- Anyone reading this book can, after this chapter,
  hand-assemble a program and link it inside the OS
  without any host involvement.

## Applied to

- **Existing apps:** none.
- **New apps:** `/bin/ld`, `/bin/ar`. Add a
  `/bin/nm` (~80 lines) as a debugging convenience —
  it shares the symbol-table reader with `ld`.
- **New tests:** `scripts/test_ld_hello.py`,
  `scripts/test_ld_archive_pickup.py` (asserts that
  an unused archive member is NOT linked in),
  `scripts/test_ar_roundtrip.py`.

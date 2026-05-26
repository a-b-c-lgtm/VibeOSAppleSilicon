# Chapter 121 — `/bin/cc`: a one-chapter native C compiler

## Why this chapter exists

This is the watershed chapter for Part XVII. By the end
of it the OS contains a C compiler that compiles C
programs on the OS, with no host involvement past
"booted QEMU."

The plan-of-record in [Chapter 115](115-c-compiler-strategy.md)
said to port TinyCC. As we walked into this chapter the
honest accounting looked like this:

- TinyCC is ~25 KLOC.
- It depends on a preprocessor (`tccpp.c`).
- It depends on a sysroot of headers — at minimum
  `stddef.h`, `stdarg.h`, `string.h`, the libc shapes
  we don't yet have.
- Its build system expects a host with `make`, `ar`,
  and a working compiler — exactly the chicken-and-egg
  we're trying to break.

So instead of porting TinyCC, we wrote our own one-
chapter compiler from scratch. It's deliberately tiny
— ~580 lines of C — and it accepts only the subset
that fits the early bring-up programs:

```c
int main(void) {
    printf("hello, osdev\n");
    return 0;
}
```

This is enough to prove the loop. A richer compiler
(the Part XVIII GCC port, or any other future backend
that satisfies the chapter-122 cross-toolchain contract)
is a separate, multi-chapter undertaking that Part XVII
does not take on.

## What shipped

| File | Lines | Role |
| --- | --- | --- |
| [userspace/cc/cc.c](../../../userspace/cc/cc.c) | ~580 | The whole compiler |
| [Makefile](../../../Makefile) | +20 | CC_OBJS, link rule, mkosfs manifest |
| [scripts/test_cc_hello.py](../../../scripts/test_cc_hello.py) | ~230 | 15 assertions across 3 phases |
| [userspace/as/as.c](../../../userspace/as/as.c) | +5 | Forward-`bl` placeholder fix |
| [scripts/_dbg_cc_inspect.py](../../../scripts/_dbg_cc_inspect.py) | ~80 | Asm + ELF dump probe |
| [scripts/_dbg_cc_bytes.py](../../../scripts/_dbg_cc_bytes.py) | ~125 | Raw-bytes-over-serial probe |

The compiler binary is at `/bin/cc` on disk; the
mkosfs manifest pulls it in alongside the existing
binaries.

## The curated C subset

The lexer + parser accept exactly what's needed for
the bring-up programs of this section. Anything else
is a parse error — that keeps the front end honest.

Accepted top-level form:

```
int main(void)            { stmt; ... }
int main()                { stmt; ... }
int main(int argc, char **argv)  { stmt; ... }
```

Accepted statements:

```
printf("LITERAL");
puts("LITERAL");
write(FD, "LITERAL", LEN);
return INT;
```

Accepted tokens inside string literals:

- `\n` `\t` `\r` `\0` `\\` `\"` — the usual escapes
- everything else is passed through verbatim

Accepted comment styles:

- `// rest of line`
- `/* ... */`

That's it. No `#include`, no variables, no
expressions, no extra functions, no `if`/`for`. The
deferred list at the end of this chapter spells out
what each of those costs.

## Codegen: the `bl`-past-string trick

The hardest thing for a tiny aarch64 compiler is
materialising a pointer to a string literal. The
"real" answer is:

```
    adrp x1, .Lstr0
    add  x1, x1, :lo12:.Lstr0
```

But our [chapter 118](118-bin-as-assembler.md)
assembler doesn't emit `ADRP` — that relocation kind
(`R_AARCH64_ADR_PREL_PG_HI21`) is non-trivial and
chapter 118 deliberately stopped at `CALL26` /
`JUMP26`. So `/bin/cc` cannot ask for ADRP.

The fallback we use is the **LR trick**. After
`bl LABEL`, the link register `x30` holds the address
of the instruction *immediately following* `bl`. If
we arrange for that following address to be the
.ascii payload, we get a pointer for free:

```
    bl   Ljmp_N            ; x30 <- &Ldata_N
Ldata_N:
    .ascii "..."
    .balign 4
Ljmp_N:
    mov  x1, x30           ; x30 is now the data pointer
    mov  x2, #LEN
    mov  x0, #1            ; fd = stdout
    mov  x8, #1            ; SYS_WRITE
    svc  #0
```

Cost per literal: one `bl`, the bytes of the string,
0–3 alignment pad bytes. Zero relocations. Zero data
section. Works for any literal length.

`return INT;` becomes:

```
    mov  x0, #INT
    mov  x8, #2            ; SYS_EXIT
    svc  #0
```

`main`'s closing brace also emits an exit-0 epilogue
in case the source didn't write an explicit return.

## The driver pipeline

`/bin/cc foo.c -o foo` runs three programs in
sequence:

```
    ┌─────────┐    foo.cc.s   ┌─────────┐   foo.cc.o   ┌─────────┐
    │ /bin/cc │ ────────────► │ /bin/as │ ───────────► │ /bin/ld │ ──► foo
    └─────────┘               └─────────┘              └─────────┘
```

All three children are launched with `spawn(...)` and
reaped with `waitpid(pid, &code, 0)` — exactly the
syscalls shipped in
[chapter 78](../15-process-model/78-sigchld-waitpid.md).
If any child exits non-zero, the driver propagates
that exit code and stops. Intermediate files live in
tmpfs (`/tmp/foo.cc.s`, `/tmp/foo.cc.o`) and are not
deleted; if the build fails you can `cat` them.

Flags:

- `-S` — stop after asm emission; output is the asm
  source.
- `-c` — stop after assembling; output is the
  relocatable `.o`.
- `-o PATH` — destination for the final stage.

Default entry symbol passed to `/bin/ld` is
`_user_start`, matching crt0 from
[chapter 120](120-crt0-and-libgcc-stubs.md). The
compiler emits `_user_start` directly — `main` is just
the name in the source.

## The `/bin/as` bug this chapter exposed

The first build looked clean. Every "compile + link"
check in `test_cc_hello.py` passed. Every "actually
run the binary" check failed with the same kernel
trap:

```
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x0000000002000000
        EC      = 0x0000000000000000
        FAR_EL1 = ...
        ELR_EL1 = 0x0000001000101000
```

`ELR_EL1` matched the program's entry point exactly,
and `EC=0` is "unknown reason" — usually an undefined
instruction.

[`_dbg_cc_bytes.py`](../../../scripts/_dbg_cc_bytes.py)
catted `/tmp/hello` over the serial socket and
decoded the first word at file offset `0x1000`:

```
02 00 00 00 68 69 0a 00 e1 03 1e aa ...
                                              ^^^^ first insn = 0x00000002
```

`0x00000002` is not a valid A64 instruction — but the
bytes that *should* have been there were `02 00 00 94`
(little-endian `0x94000002`, which is `bl PC+8`).
The opcode byte `0x94` was missing.

Root cause: `/bin/as` from
[chapter 118](118-bin-as-assembler.md) handles a
forward-referenced `bl LABEL` like this:

```c
emit_word(0);           /* placeholder; reloc fills it */
reloc_add(off, sidx, R_AARCH64_CALL26, 0);
```

A second pass walks all relocations and, for any
target that landed in `.text` of the same input,
patches the displacement in place:

```c
insn = (insn & 0xFC000000u) | ((uint32_t)d & 0x03FFFFFFu);
```

The mask `0xFC000000` preserves the top 6 bits — the
opcode bits. But the placeholder was zero, so there
was no opcode to preserve. The patch wrote a
displacement-only word with no `bl` opcode at all.

Fix: emit the opcode-carrying placeholder, so both
the in-pass patcher and `/bin/ld`'s reloc pass leave
the top 6 bits alone:

```c
emit_word(link ? 0x94000000u : 0x14000000u);
```

This bug had never fired before because chapter 119's
linker smoke test used `mov` and `svc` only — no
forward `bl`. `/bin/cc` is the first emitter in our
tree to use forward branches at all.

The lesson is bigger than one bug: **whenever an
encoder writes a placeholder for a later patch, the
placeholder must carry every bit the patcher won't
touch.** Either set the opcode at emit time, or
change the patcher to OR in the opcode. We chose the
former because it makes the bytes self-describing
even before relocation.

## How a 4-line C program flows through

Source:

```c
int main(void) {
    printf("hi\n");
    return 0;
}
```

`/bin/cc -S hello.c -o hello.s` produces:

```
/* generated by /bin/cc — chapter 121 */
.text
.global _user_start
_user_start:
    bl   .LSjmp0
.LSdata0:
    .ascii "hi\n"
    .balign 4
.LSjmp0:
    mov  x1, x30
    mov  x2, #3
    mov  x0, #1
    mov  x8, #1
    svc  #0
    mov  x0, #0
    mov  x8, #2
    svc  #0
    mov  x0, #0       ; main-epilogue exit-0
    mov  x8, #2
    svc  #0
```

`/bin/as` turns that into a 912-byte ET_REL with
8 sections; `/bin/ld -e _user_start` produces an
8192-byte ET_EXEC whose `.text` (file offset
`0x1000`) starts with:

```
02 00 00 94    bl   PC+8
68 69 0a 00    "hi\n" + 1 pad byte
e1 03 1e aa    mov  x1, x30
62 00 80 d2    mov  x2, #3
20 00 80 d2    mov  x0, #1
28 00 80 d2    mov  x8, #1
01 00 00 d4    svc  #0
00 00 80 d2    mov  x0, #0
48 00 80 d2    mov  x8, #2
01 00 00 d4    svc  #0
```

`/tmp/hello` then prints `hi` and exits 0. The entire
journey — from `int main(void)` to bytes on the wire —
costs ten 32-bit words.

## Tests (`scripts/test_cc_hello.py`)

15 assertions, three phases. All green:

**Phase 1: compile + run a real program**

- `hello.c staged correctly`
- `/bin/cc reported success`
- `/bin/cc emitted intermediate .cc.s`
- `intermediate .o starts with ELF magic`
- `linked /tmp/hello starts with ELF magic`
- `/tmp/hello printed 'hello, osdev'`
- `/tmp/hello exited with code 0`

**Phase 2: `-S` mode is well-formed**

- `/bin/cc -S emitted /tmp/hello.s`
- asm declares `_user_start`
- asm contains an escaped ascii literal
- asm uses SYS_WRITE
- asm ends in SYS_EXIT

**Phase 3: exit-code propagation**

- `/bin/cc built exit42`
- `exit42 printed via puts`
- `exit42 exited with code 42`

The 20-test regression sweep is also green:

```
TOTAL: 20 / 20
PASS: test_cc_hello   test_atexit       test_bin_as
PASS: test_bin_ld_ar  test_libc_stat    test_libc_errno
PASS: test_libc_stdio test_libc_env     test_boot_to_desktop
PASS: test_userfs_echo test_clipboard    test_mount_ro
PASS: test_userfs_timeout test_httpd_forward test_browser_proxy
PASS: test_cow        test_fork_exec    test_busy_on_mix
PASS: test_clone_files test_directories
```

## Lessons

1. **Encoder placeholders must be self-describing.**
   If a patcher uses `(insn & opcode_mask) | imm` to
   fix up an instruction, the placeholder has to
   already carry the opcode bits. A zero placeholder
   silently turns `bl` into "undefined instruction"
   and the failure mode is a sync exception three
   stages downstream.
2. **The LR trick is enough to materialise pointers
   without ADRP.** A real compiler avoids the wasted
   `bl`, but for a one-chapter compiler the cost is
   four bytes of code per literal and the
   architectural simplification is huge.
3. **`spawn + waitpid` is the entire driver
   contract.** Once you have those two syscalls, any
   pipeline of compilers / assemblers / linkers
   composes the same way the host's `gcc` does.
4. **A toy compiler is the right shape for the
   first one.** Porting TinyCC would have meant
   porting its preprocessor, its sysroot expectations,
   and its build system. A 580-line compiler that
   accepts a curated subset proves the loop without
   any of that machinery.

## Applied to

- **Existing apps:** none changed. `/bin/cc` is a
  new app; nothing in the system asks it to compile
  anything yet. Chapter 124 will be the first user.
- **New apps:** `/bin/cc` itself.
- **New tests:** `scripts/test_cc_hello.py`. Added
  to the chapter 121 regression sweep alongside
  every test from chapter 119–120.
- **Debug scripts (kept per the policy):**
  `scripts/_dbg_cc_inspect.py`,
  `scripts/_dbg_cc_bytes.py`,
  `scripts/_dbg_sweep_ch121.sh`.

## What's deferred

Everything we'd need to compile real software:

| Feature | Lands in |
| --- | --- |
| Local + global variables | 123 |
| Expressions (`a + b * c`) | 123 |
| `if` / `while` / `for` | future / Part XVIII |
| Multiple functions | future / Part XVIII |
| `printf` format specifiers | future (needs varargs lowering) |
| `#include` and a preprocessor | future / Part XVIII |
| Floats / doubles | future / Part XVIII |
| Struct types | future / Part XVIII |
| `mmap` / `dlopen` etc. | never (deliberately) |

The plan for the rest of Part XVII is: chapter 122
locks down the cross-toolchain contract; chapter 123
grows `/bin/cc` with locals and arithmetic; chapter 124
relies on an on-disk source file for the first real
native compile; chapter 125 is an honest accounting
of the self-hosting gap that Part XVII does not close.
A real GCC port is left for Part XVIII.

## Next

[Chapter 122](122-cross-toolchain-contract.md) —
writing down (and regression-gating) the cross-
toolchain contract every userspace binary already
respects, and that any future compiler port would
have to satisfy.

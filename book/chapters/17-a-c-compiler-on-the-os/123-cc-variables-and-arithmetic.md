# Chapter 123 — `/bin/cc` grows variables and arithmetic

## Why this chapter exists

[Chapter 121](121-bin-cc.md) shipped a one-page compiler.
It could only compile programs of the form:

```c
int main(void) {
    printf("a literal");
    puts("another literal");
    return 42;
}
```

Useful as a proof-of-concept; useless as a compiler. A
program that prints a fixed string is just a string —
nothing was computed. To call `/bin/cc` a compiler in any
meaningful sense, the language it accepts has to include
**values that don't exist until the program runs.**

[Chapter 122](122-cross-toolchain-contract.md) wrote down the
cross-toolchain contract any future compiler port has to
satisfy. This chapter takes the opposite tack: instead of
adding more host-side infrastructure, it grows the
in-guest compiler itself. By the end you can compile,
on the OS, a program that allocates local variables,
assigns values to them, computes arithmetic expressions,
and returns the result as the exit code.

## What shipped

| File | Δ lines | Role |
| --- | --- | --- |
| [userspace/cc/cc.c](../../../userspace/cc/cc.c) | +180 | Locals, expressions, frame prologue |
| [scripts/test_cc_vars.py](../../../scripts/test_cc_vars.py) | +180 | 16-assertion smoke test, 5 programs |
| [scripts/_dbg_cc_vars.py](../../../scripts/_dbg_cc_vars.py) | +50 | Single-program serial inspector (kept) |
| [scripts/_dbg_sweep_ch123.sh](../../../scripts/_dbg_sweep_ch123.sh) | +30 | 21-test regression runner |

No kernel changes; no other userspace changes; no new
syscalls. The whole chapter lives inside
[userspace/cc/cc.c](../../../userspace/cc/cc.c) and the
new test.

## The new language surface

Chapter 121's grammar grew exactly four things:

```ebnf
declaration  =  "int" IDENT ( "=" expr )? ";"
assignment   =  IDENT "=" expr ";"
exit_stmt    =  "exit" "(" expr ")" ";"
return_stmt  =  "return" expr? ";"

expr         =  primary ( ("+" | "-") primary )*
primary      =  INT_LITERAL | IDENT | "(" expr ")"
```

That's enough for a tangible program:

```c
int main(void) {
    int x = 10 + 5;
    int y = 3 + 1;
    int z;
    z = x - y;
    return z;
}
```

Exit code: `0x0b` (= 11). This is a real compile, not a
string-substitution. Change `5` to `9` and the exit code
changes to `15`. The compiler is computing.

## How the codegen works

The chapter-121 codegen had no notion of a stack frame
because it had no variables. Everything was register
churn: print a literal, exit with an immediate. Chapter
123's codegen adds **one fixed-size stack frame per
function**, plus a **compile-time tracked expression
stack** that uses preallocated slots inside that frame.

### The frame

Each function begins with:

```asm
_user_start:
    sub  sp, sp, #256
```

256 bytes = 32 × 8 byte slots, partitioned at compile
time:

| Slot range | Use |
| --- | --- |
| `[sp, #0]` … `[sp, #120]`   | 16 local variables (`int`, one per slot) |
| `[sp, #128]` … `[sp, #248]` | 16 expression-stack slots for binop spill |

The frame is the same size for every function: paying 256
bytes of stack to skip having to compute frame size during
codegen is a great trade for a compiler this small. We
hard-cap locals at 16 and expression nesting at 16, which
is more than the chapter-123 language can express anyway.

### Why no frame pointer

`/bin/as` (chapter 118) encodes `mov` between registers as
`ORR Rd, XZR, Rm` — but the AArch64 encoding of `ORR`
with `Rm=31` means **XZR**, not **SP**. So `mov x29, sp`
through our assembler silently produces `x29 = 0`. That
would make every variable access read from `[x29 + N]
= [0 + N]` which is a userspace page-zero fault.

We work around this by **never using a frame pointer**.
Instead, the variable offsets are baked relative to `sp`
directly. Since the expression-stack lives in fixed slots
inside the frame (not pushed/popped with sp adjustments),
`sp` is constant for the lifetime of the function — and
constant-sp lets us address everything as `[sp, #N]`.

This is also why the expression stack lives at
`sp+128..sp+248` instead of being a true growable stack:
a true stack would require either pre/post-indexed
addressing modes (`str x0, [sp, #-16]!`) which our
[/bin/as](../../../userspace/as/as.c) doesn't encode, or
runtime `sub sp, sp, #16` instructions, which would
invalidate the variable offsets.

### Push and pop on the expression stack

The compiler tracks `g_expr_depth` at compile time. Every
binary operator emits the same sequence:

```
parse left  → x0
push x0     ; str x0, [sp, #(128 + d*8)]; d++
parse right → x0
pop  x1     ; d--; ldr x1, [sp, #(128 + d*8)]
op           x0 = x1 ⊕ x0
```

For `int z = (10 + 5) - (3 + 1);`:

| asm | depth | meaning |
| --- | --- | --- |
| `mov x0, #10`                    | 0 | left of `+` |
| `str x0, [sp, #128]`             | 1 | push |
| `mov x0, #5`                     | 1 | right of `+` |
| `ldr x1, [sp, #128]`             | 0 | pop into x1 |
| `add x0, x1, x0`                 | 0 | x0 = 15 (= left of `-`) |
| `str x0, [sp, #128]`             | 1 | push |
| `mov x0, #3`                     | 1 | left of nested `+` |
| `str x0, [sp, #136]`             | 2 | push |
| `mov x0, #1`                     | 2 | right of nested `+` |
| `ldr x1, [sp, #136]`             | 1 | pop into x1 |
| `add x0, x1, x0`                 | 1 | x0 = 4 |
| `ldr x1, [sp, #128]`             | 0 | pop into x1 |
| `sub x0, x1, x0`                 | 0 | x0 = 11 |
| `str x0, [sp, #16]`              | 0 | store into `z` (slot 2) |

The depth column is a compile-time tracker — it ensures
push/pop are balanced and bounds-checked. The runtime
never knows or cares about it.

### Why `sub x0, x1, x0` and not `sub x0, x0, x1`

Look at the parse order: the **left** operand is pushed
first, the **right** is computed second (and lives in x0
when the binop fires), then `pop x1` retrieves the left
operand. So at op time `x1 = left`, `x0 = right`, and we
want `result = left ⊕ right = x1 ⊕ x0`. Subtraction is
the one operator where order matters, hence
`sub x0, x1, x0`.

## The four token additions

The lexer changed by exactly three characters of
significance: `+`, `-`, `=`.

```c
case '+': g_tok_kind = TK_PLUS;   return;
case '-': g_tok_kind = TK_MINUS;  return;
case '=': g_tok_kind = TK_EQ;     return;
```

(`+` and `-` had no meaning in the chapter-121 grammar —
they were single-char errors. `=` was only used inside
`int main()` parameter lists where the lexer never saw
it.)

## The `int v; v = 99;` case

A declaration without an initializer must still leave the
slot in a well-defined state. Otherwise:

```c
int v;          // slot 0 holds whatever the stack used to
int w = v;      // copy garbage into slot 1
return w;       // exit with garbage
```

We default-zero-init by emitting `mov x0, #0; str x0, [sp, #N]`
when the declaration has no `= EXPR`. Costs two instructions
per uninit'd variable; pays for itself the first time a
user writes `int v;` and forgets to initialize.

This is the behaviour of `int v;` at file scope in
ISO C anyway (`= 0` per 6.7.9p10). At block scope C
says the value is "indeterminate" — but a tiny educational
compiler defaulting to zero is the right call: it makes
buggy programs deterministic instead of stack-leaky.

## The TMPFS_MAX_FILES gotcha

Writing the regression test surfaced an OS limit we hadn't
been close to before. `/bin/cc` produces 4 files per
invocation:

```
/tmp/<name>.c       (source, staged by the test)
/tmp/<name>.cc.s    (intermediate asm)
/tmp/<name>.cc.o    (intermediate object)
/tmp/<name>         (final binary)
```

[kernel/core/tmpfs.h](../../../kernel/core/tmpfs.h) caps
`TMPFS_MAX_FILES` at 16. The 5th `/bin/cc` invocation in
a single boot hits a brick wall:

```
cc: cannot open '/tmp/noinit.c'
[sys_exit] thread '/bin/cc' exited with code 0x0000000000000001
```

The fix is in the test, not the kernel: every iteration
of [test_cc_vars.py](../../../scripts/test_cc_vars.py)
ends with `rm /tmp/<name>.* 2>/dev/null; true` to reclaim
the four slots. Bumping the kernel constant would have
worked too, but the discipline of "test cleans up after
itself" generalizes better.

If a future chapter exercises `/bin/cc` more aggressively
(e.g. compiling 20+ programs in a single boot), bump
`TMPFS_MAX_FILES` to 64. The struct is just 4 entries
of `(name, size_t, ptr-to-block-list)` and we have plenty
of address space.

## What it took to make the new tests stable

Once compilation worked, the test harness still flaked. Two
secondary issues showed up and were fixed in the test —
both documented here so the next chapter that builds on
this scaffolding doesn't have to rediscover them.

1. **Drain between iterations.** After `[sh] exit N`
   prints, there's a `/$ ` prompt and sometimes also a
   `[wmclient] DAMAGE failed status=-5` retry line.
   The next `wait_for(PROMPT)` can match the *prompt
   that already happened*, not the next one. We drain
   the socket for 400 ms between iterations so the next
   match starts from a clean buffer.
2. **Same pattern, different shape, in the boot path.**
   After `wait_for(PROMPT, 20.0)`, sleep 1.5 s and drain.
   The first command of a test fires from a known-clean
   state.

Both fixes are pure test-side; the OS is honest about
what it printed.

## Tests

[scripts/test_cc_vars.py](../../../scripts/test_cc_vars.py)
ships 16 assertions across 6 programs:

| Program | What it proves | Exit |
| --- | --- | --- |
| ARITH  | `int + int` works end-to-end | 7 |
| PAREN  | precedence via `()`, subtraction | 11 |
| EXIT   | `exit(EXPR)` parses and runs | 21 |
| MIX    | locals coexist with `printf` (no frame corruption) | 42 |
| NOINIT | declaration without initializer reads as 0 | 98 |
| -S ASM | asm output contains prologue, ADD, LDR/STR via SP | (asm inspection) |

Result: **16 PASS / 0 FAIL.**

Full regression sweep
([scripts/_dbg_sweep_ch123.sh](../../../scripts/_dbg_sweep_ch123.sh))
runs all 21 chapter-XVII-relevant tests. See the chapter
end for the result.

## Applied to (per the apps-must-use-features rule)

- **`/bin/cc` itself is the app.** Chapter 121 shipped it
  as a printf-only stub. Chapter 123 turns it into the
  smallest credible C compiler in this codebase. Every
  feature added is exercised end-to-end by a binary
  that's compiled, linked, and run on the OS.
- **New test:**
  [scripts/test_cc_vars.py](../../../scripts/test_cc_vars.py).
- **New debug script kept** (per the debug-scripts policy):
  [scripts/_dbg_cc_vars.py](../../../scripts/_dbg_cc_vars.py)
  boots, stages one program, dumps its `.s` output, runs
  it. Used to root-cause the TMPFS_MAX_FILES wall above.
- **No existing app modified** — the compiler doesn't
  need to be used by another app yet. Chapter 124's
  test will compile a real `hello.c` *from disk* (not
  from a string baked into a Python harness), and
  chapter 127 wires `/bin/cc` into notepad's Build
  button. Both of those will exercise the chapter-123
  language additions on a real source.

## Lessons

1. **Fixed frame, no frame pointer.** Adding a frame
   pointer in a compiler this small fights the assembler
   over how to encode `mov x29, sp`. A fixed-offset
   frame addressed off `sp` is simpler and works with
   the AArch64 instructions our `/bin/as` can encode.
2. **Compile-time tracked stack.** The push/pop depth
   is known at compile time for our grammar, so we
   pre-allocate spill slots inside the frame instead of
   dynamic sp adjustments. Less code, fewer corner
   cases. Generalises to anything that doesn't allow
   `alloca` or variable-length arrays.
3. **Default-init `int v;` to zero.** It's not ISO at
   block scope, but it makes the compiler educational
   instead of mysterious. Costs two instructions.
4. **Test cleanup is part of the test.** The
   TMPFS_MAX_FILES wall would have looked like a parser
   bug if we'd hit it without first thinking about
   resource lifetime. Every `/bin/cc` run produces 4
   files; harnesses that loop need to free them.

## What's still missing

Toward a real compiler — chapters 124+ will bite off
some of these. None block this chapter from being
"done":

| Feature | Why it's deferred |
| --- | --- |
| `*` and `/`               | Need `mul`/`udiv`/`sdiv` in `/bin/as` |
| `if` / `while` / `for`    | Needs conditional branches (we have `cmp`, no `b.lt` family) |
| Multiple functions        | Needs symbol table beyond `main`, prologue/epilogue per fn |
| Function calls            | Needs `bl name` codegen + caller-save discipline |
| `char` and pointers       | Needs `ldrb`/`strb` and address-of operator |
| Format specifiers in printf | Needs varargs codegen and real libc printf |
| `#include`                | Needs a preprocessor (probably a separate `/bin/cpp`) |
| Types larger than `int`   | Needs type tracking; currently every value is treated as 64-bit |

A real compiler (the Part XVIII GCC port, or any other
future backend that respects the chapter-122 cross-
toolchain contract) would cover all of these. Until
then, the language surface above is the canonical "what
runs on osdev-without-host-help".

## Next

[Chapter 124 — First native compile: `hello.c` from
`/data/src/`](124-first-native-compile.md) takes the
language this chapter added and uses it: a real `.c`
file ships on the data partition, the test boots,
`/bin/cc /data/src/hello.c -o /tmp/hello`, runs the
result. No string baked into a Python harness — the
source is on disk.

# Chapter 165 — setjmp / longjmp

> **Milestone in this chapter:** add the AArch64 implementation
> of `setjmp` / `longjmp` (and the `sig*` pair) that every
> upstream C program assumes will work.
> **Code referenced:**
> - [userspace/libc/setjmp.h](../../../userspace/libc/setjmp.h)
> - [userspace/libc/setjmp.S](../../../userspace/libc/setjmp.S)
> - [scripts/test_setjmp.py](../../../scripts/test_setjmp.py)
>
> **At the end of this chapter** you will have `setjmp` /
> `longjmp` / `sigsetjmp` / `siglongjmp` linked into every
> userspace binary via `libosdevc.a`, and a green
> `test_setjmp.py` regression. Prerequisite: chapter 12 (ELF
> loader and the userspace ABI).

---

## What you'll do in this chapter

1. Add `userspace/libc/setjmp.h` declaring `jmp_buf` and the
   `setjmp` / `longjmp` prototypes.
2. Add `userspace/libc/setjmp.S` with the 22-instruction
   AArch64 implementation that saves and restores the
   callee-saved set.
3. Reserve eight slots in `jmp_buf` for `d8..d15` so chapter
   129 (FP/SIMD at EL0) can fill them in without an ABI break.
4. Write a `setjmptest` binary that exercises the round trip,
   including the C99 "longjmp(env, 0) returns 1" rule.
5. Run `scripts/test_setjmp.py` and watch it land green.

Nothing here touches the kernel. Two source files, one test
binary, one regression script.

## Why this is the right place to start

Part XVIII's [chapter 164 plan](164-plan-real-gcc-and-doom.md)
lists six libc chapters that must land before binutils and GCC
get cross-built. Start with the smallest of them — a real
`setjmp` and `longjmp`. It earns its keep on three axes at
once:

- **No kernel work.** Everything lives in userspace. That's
  exactly what we want for the opening chapter of a long
  section — start with a piece that can land in isolation and
  ship green on its own.
- **It's load-bearing for the GCC port.** GCC's own diagnostic
  recovery path uses `setjmp` to unwind out of the lexer / parser
  on fatal errors. So do every C compiler we'd plausibly pick as
  a fallback, BearSSL's `t_*.c` selftests, Lua's error handler,
  and parts of GNU make. Adding it now means none of those ports
  have to wait.
- **It's the smallest unit of real ABI engineering.** AArch64's
  callee-saved set is exact (`x19..x28`, `x29`, `x30`, `sp`,
  plus FP `d8..d15`). Saving and restoring it is a 10-line piece
  of asm that you either get exactly right or get exactly wrong;
  there is no in-between. Good warm-up for chapter 171's FP-state
  context-switch work.

The C99 spec ([§7.13](https://port70.net/~nsz/c/c99/n1256.html#7.13))
defines exactly what each function must do; implement that
literally and stop.

## What you'll write

Four small pieces of code and one regression test:

1. [userspace/libc/setjmp.h](../../../userspace/libc/setjmp.h) —
   the public header. Declares `jmp_buf` as a 22-slot
   `unsigned long` array (so callers pass it by name without
   writing `&`), and prototypes `setjmp` / `longjmp`. Inline
   documentation pins down the buffer layout so chapter 171
   knows exactly which slots it owns when FP/SIMD turns on at
   EL0.
2. [userspace/libc/setjmp.S](../../../userspace/libc/setjmp.S) —
   the AArch64 implementation. 22 instructions across both
   functions. Saves the callee-saved integer regs + SP into the
   buffer, restores them on the way back, encodes the
   "longjmp(env, 0) returns 1" rule with a single `csinc`.
3. [userspace/setjmptest/setjmptest.c](../../../userspace/setjmptest/setjmptest.c) —
   exercises every promise: 0-first, value-passes-through, 0→1,
   and a callee-saved-marker invariant the compiler will
   reasonably put in x19..x28.
4. [scripts/test_setjmp.py](../../../scripts/test_setjmp.py) —
   boots the kernel headless, runs `setjmptest`, asserts no
   `FAIL:` line and the `all checks passed` marker. Same shape
   as [scripts/test_printftest.py](../../../scripts/test_printftest.py).

## The buffer layout

The AAPCS64 (the AArch64 Procedure Call Standard, §6.1.1) says
the compiler must preserve exactly this set across a function
call:

| Class      | Registers                       |
|------------|---------------------------------|
| Integer    | `x19..x28`, `x29` (FP), `x30` (LR), `SP` |
| FP / SIMD  | `d8..d15` (lower 64 bits of `v8..v15`)   |

Everything else (`x0..x18`, `q0..q7`, `v16..v31`) is
*caller*-saved — free to be clobbered across any call. Those
are the slots `setjmp` saves, and the slots `longjmp`
restores. Nothing more, nothing less.

The OS boots today with `-mgeneral-regs-only`, which means no
FP/SIMD code is emitted and `CPACR_EL1.FPEN` traps any FP
instruction that does run. Chapter 171 is going to flip both —
extend the context switch with `q0..q31` + `fpsr` + `fpcr` and
drop the flag. So the buffer reserves the eight FP slots today
and the asm leaves them untouched:

```
   offset  contents
    0..15  x19, x20
   16..31  x21, x22
   32..47  x23, x24
   48..63  x25, x26
   64..79  x27, x28
   80..95  x29, x30
       96  sp
  104..   d8..d15   ── reserved for chapter 171
      168  pad      ── keeps size a multiple of 16
```

When chapter 171 lands it will add eight `stp` pairs in the
middle of the `setjmp` / `longjmp` bodies. The layout doesn't
change, so this chapter's compiled binaries keep working — they
just won't *save* their FP regs until they're rebuilt against
the chapter-129 asm. Which is exactly what you want: any code
that compiled and ran before chapter 171 didn't have FP regs
to spill in the first place.

## The asm, end to end

`setjmp` is six `stp` pairs + a `mov`-to-sp + a `str` + the
zero return:

```aarch64
setjmp:
_setjmp:
    stp     x19, x20, [x0,  #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    mov     x0, #0
    ret
```

Three things to notice:

- **`stp` does 16 bytes at a time.** AArch64's pair-store
  encoding takes a 7-bit signed *scaled* offset (i.e. the
  offset is a multiple of 8). Six `stp` instructions cover
  96 bytes of callee-saved registers in six cycles instead of
  twelve, and stay inside the immediate's range.
- **SP needs the `mov`-via-x1 dance.** AArch64 does not let you
  `str sp, [...]` directly — SP is special-cased out of the
  general-register encoding for store. Copy SP into a scratch
  register (x1, caller-saved and unused at this point) and
  store *that*. Cheap.
- **Don't touch x0 until the very last instruction.** Until
  then x0 is still the caller's `jmp_buf *`. You need it as
  both the address operand and as the "return 0" value, in
  that order.

`longjmp` is the same shape, run backwards, plus one cleverness
for the C99 zero-becomes-one rule:

```aarch64
longjmp:
_longjmp:
    ldp     x19, x20, [x0,  #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2,  [x0, #96]
    mov     sp,  x2
    cmp     x1, #0
    csinc   x0, x1, xzr, ne
    ret
```

The `csinc x0, x1, xzr, ne` line is the part worth pausing on.
C99 [§7.13.2.1#3](https://port70.net/~nsz/c/c99/n1256.html#7.13.2.1p3):

> If the argument `val` is 0, the function returns the value 1
> from the corresponding `setjmp` invocation; otherwise, it
> returns the value `val`.

`csinc` (conditional select-and-increment) does exactly this in
one instruction:

- If the condition (here `NE`) is true: x0 = x1 + 0 = x1.
  This is the val != 0 path; pass it through unchanged.
- If the condition is false (val == 0): x0 = xzr + 1 = 1.
  This is the val == 0 path; the standard says return 1.

No branch, no second instruction. The `ret` then jumps to the
freshly-restored `x30` — which is the address inside the
original caller of `setjmp`, just past the `bl setjmp`
instruction. The `mov sp, x2` immediately before that has
already rewound the stack pointer, so as far as the caller's
abstract-machine view is concerned, control has returned from
the matching `setjmp` with x0 holding the longjmp value.

## The "callee-saved survives" check

The strongest invariant `setjmptest.c` pins down is this:

```c
unsigned long marker = 0xC0FFEE00DEADBEEFUL;

int r = setjmp(env);
if (marker != 0xC0FFEE00DEADBEEFUL) { /* FAIL */ }
```

`marker` is live across the call to `inner()`, which the
compiler will most likely satisfy by parking it in one of
`x19..x28` (the registers it's *allowed* to assume are
preserved across calls). If your `setjmp` / `longjmp` saved
and restored that register correctly, `marker` still reads
`0xC0FFEE00DEADBEEFUL` after the round trip. If they didn't,
you'd see garbage — whatever `inner()` happened to leave in
that register.

This is the right way to test the invariant without forcing a
specific register allocation: let the compiler pick the slot,
then let the AAPCS guarantee do the rest. The actual asm
verifies it via the test:

```
[setjmptest] starting
  setjmp(env) returned 0 (marker=0xc0ffee00deadbeef)
  inner(target=7) about to longjmp
  setjmp(env) returned 7 (marker=0xc0ffee00deadbeef)
  inner(target=42) about to longjmp
  setjmp(env) returned 42 (marker=0xc0ffee00deadbeef)
  setjmp(env2) returned 0
  inner2() about to longjmp(env2, 0)
  setjmp(env2) returned 1
[setjmptest] all checks passed
```

Marker stays consistent across all three longjmps. The third
test (`env2`) drives the `0 → 1` rule.

## Run it

With both source files in place:

```sh
$ make
$ python3 scripts/test_setjmp.py
```

The test boots the kernel headless, runs `setjmptest`, and
asserts the marker survived every round trip:

```
[setjmptest] starting
  setjmp(env) returned 0 (marker=0xc0ffee00deadbeef)
  inner(target=7) about to longjmp
  setjmp(env) returned 7 (marker=0xc0ffee00deadbeef)
  inner(target=42) about to longjmp
  setjmp(env) returned 42 (marker=0xc0ffee00deadbeef)
  setjmp(env2) returned 0
  inner2() about to longjmp(env2, 0)
  setjmp(env2) returned 1
[setjmptest] all checks passed
```

Marker stays consistent across all three longjmps. The third
test (`env2`) drives the `0 → 1` rule.

## What this unlocks

- **The libc gains `setjmp` / `longjmp`.** Nothing in-tree
  needed them before, so no existing app uses them yet; this
  chapter is the foundation, and chapters 166–170 / 175–194
  are the consumers.
- **The build** picks up `setjmp.S` automatically via the
  existing `userspace/%.S` pattern rule — no new wiring.
- **The chapter-181 GCC port** can be cross-built without a
  `--disable-fatal-errors` hack. `gcc/diagnostic.c`'s
  `fatal_error` path calls `longjmp` to unwind back to the
  driver's top-level error handler, and now that call resolves.
- **The chapter-174 cross-built Doom port** sees a libc
  surface where `<setjmp.h>` is present and prototyped, so its
  platform shim doesn't have to fake the header.

## Things to remember

- **`csinc` over a branch.** Any time a function has a
  "default-1, else identity" return-value rule, the
  conditional-select-and-increment family is one instruction.
  Branch-free is easier to reason about, and cheaper.
- **Reserve, don't redesign.** Adding the eight FP slots
  *now*, even though you don't fill them yet, means chapter
  129 lands without an ABI break for whatever depends on
  `jmp_buf` between now and then. The cost is 64 bytes per
  `jmp_buf`. Worth it.
- **AArch64's "no `str sp`" is the kind of detail that
  catches you once and never again.** Document it in the asm
  comment so future readers don't burn 20 minutes wondering
  why `str sp, [x0, #96]` doesn't assemble.

## What's deferred

- **`sigsetjmp` / `siglongjmp`** — these save/restore the
  signal mask in addition to the register state. We don't have
  signal masks in libc yet; that lands with the signal-wrapper
  work in **chapter 166**.
- **`setjmp` on FP-using code paths** — the buffer reserves
  slots for `d8..d15` but the asm does not yet touch them.
  Today this is fine because the OS boots with
  `-mgeneral-regs-only` and traps any FP at EL0. **Chapter
  129** fills in those eight `stp` / `ldp` instructions as
  part of turning FP/SIMD on at EL0.
- **Cross-thread `longjmp`** — undefined behaviour per the
  spec; the implementation does not try to detect or catch
  it. If a future thread implementation makes this fall over
  loudly (rather than corrupt the stack silently), revisit;
  until then it's documented and out of scope.
- **Unwind-aware C++ exceptions** — out of scope for Part
  XVIII entirely; C++ in-guest gets its own section.

## What's next

[Chapter 166](166-signal-and-raise.md) adds `signal()` /
`raise()` wrappers on top of chapter 76's `sigaction`, plus
the default handlers for `SIGINT` / `SIGSEGV` / `SIGFPE` that
real upstream code (including GCC's own diagnostic path)
expects to find. After that: ctype, time, qsort, real printf
(chapters 167–f), then the kernel landmark — **chapter 171**
turns FP/SIMD on at EL0, the single biggest unblocker for
everything downstream.

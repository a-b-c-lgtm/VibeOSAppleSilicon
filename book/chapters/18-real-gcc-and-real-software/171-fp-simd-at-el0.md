# Chapter 171 — FP / SIMD at EL0

> **Milestone in this chapter:** turn on FP / SIMD for userspace
> and extend the context-switch frame to save and restore the
> 32-register vector state.
> **Code referenced:**
> - [kernel/arch/mmu.S](../../../kernel/arch/mmu.S)
>   (`CPACR_EL1.FPEN = 0b11`)
> - [kernel/arch/context_switch.s](../../../kernel/arch/context_switch.s)
>   (`q0..q31`, `fpsr`, `fpcr` save / restore)
> - [kernel/core/thread.h](../../../kernel/core/thread.h)
>   (`FRAME_SIZE`)
> - [scripts/test_fp.py](../../../scripts/test_fp.py)
>
> **At the end of this chapter** you will have FP / SIMD
> available to every EL0 thread, an 816-byte context-switch
> frame that preserves the full vector state on every
> `cswitch_to`, and a green `test_fp.py` regression. That
> unlocks Doom's renderer (`R_PointToAngle`,
> `R_PointToDist`), libpng's filter heuristics, and the
> eventual `%f` / `strtod` work. Prerequisites: chapters 10
> (threading + context switch), 13 (ELF + crt0), 165 (setjmp
> / longjmp), 170 (printf without `%f`).

---

## What you'll do in this chapter

1. Enable FP/SIMD at EL0 by setting `CPACR_EL1.FPEN = 0b11`
   inside `kernel/arch/mmu.S` right after the MMU comes on.
2. Extend the context-switch frame in
   `kernel/arch/context_switch.s` from 288 bytes to 816 bytes
   so every `cswitch_to` saves and restores `q0..q31` plus
   `fpsr` and `fpcr`.
3. Update `kernel/core/thread.h::FRAME_SIZE` to match.
4. Fill in the eight `stp d*, d*` / `ldp d*, d*` pairs in
   `userspace/libc/setjmp.S` that chapter 165 reserved space
   for.
5. Drop `-mgeneral-regs-only` from `USER_CFLAGS` (keep it on
   `CFLAGS` and `BEARSSL_CFLAGS`).
6. Write `userspace/fptest/fptest.c` to cover basic FP, yield
   preservation, and setjmp preservation. Run
   `scripts/test_fp.py` and the regression sweep.

## Why now

Two upcoming chapters cannot land without floating-point at
EL0:

1. **130 — Doom on host, then in-guest.** Doom Vanilla has a
   handful of `double` uses in `r_main.c` and `p_user.c`. The
   sourceport ChocolateDoom carries them forward. Even the
   "fixed-point only" parts of the renderer round-trip through
   `double` in a few utility functions. A guest binary that
   contains a single FP instruction without the FPU enabled
   dies the moment EL1 sees `EC=0x07` (SIMD / FP access trap).

2. **131 — GCC's runtime artifacts.** `libgcc` itself
   compiles routines for soft-float emulation (e.g.
   `__addsf3`, `__mulsf3`) only when the target is
   `-msoft-float`. The cross toolchain (`aarch64-elf-gcc`) is
   configured for *hardware* FP by default — that's the
   standard for `aarch64-elf`. Switching to soft-float would
   mean rebuilding the toolchain; enabling the FPU is one
   paragraph of code.

Plus several smaller deferred items finally light up:

- `difftime` (chapter 168) becomes real instead of a stub —
  it really does subtract `time_t`s and return a `double`.
- `setjmp` / `longjmp` (chapter 165) finally save the AAPCS64
  callee-saved FP registers `d8..d15` like the spec demands.
- A future sub-chapter can enable `%f` in `printf` and `strtod`
  in `stdlib` without needing yet another kernel change.

---

## The decision: eager save/restore, not lazy

Most textbook OSes do FP lazily:

1. Boot with `CPACR_EL1.FPEN = 00` (trap all FP at EL0).
2. On the first trap, allocate a per-thread FP context block,
   save the previous owner's regs into it, restore this
   thread's regs, set `FPEN = 11`.
3. On context switch, just flip a "trap on next FP use" flag.
   If the new thread never touches FP this slice, we never pay
   the save/restore cost.

This is clever and wins when most threads don't use FP. It is
also a small state machine with its own bugs (lost-update races
on the "owner" pointer, SMP cross-CPU invalidation, signal
handlers that interrupt mid-restore, ...).

**Pick eager.** Reasons, in order of weight:

1. **The kernel is built `-mgeneral-regs-only`.** That means
   kernel code never *emits* FP instructions. So the FP
   registers between two context switches are *always*
   user state — the kernel never overwrites them with its
   own working values. The only consumers are EL0 threads.
   Save once per `cswitch_to`, restore once. No trap, no
   "who owned the FPU last" bookkeeping.

2. **Doom and GCC both use FP everywhere.** The lazy
   optimisation pays off when FP-using threads are the
   minority. Once the guest is running real software, FP
   is in the working set of the busy-loop processes.

3. **AArch64 FP register file is 512 bytes.** Sixteen 32-byte
   `Q` registers as `q0..q31`. That's the dominant cost. The
   existing frame already saves 256 bytes of GPR state plus
   ELR/SPSR/SP_EL0. Doubling the frame is acceptable: the
   threading limit on number of in-flight kernel-stack frames
   hasn't been anywhere near the slab limit since chapter 93.

4. **SMP simplicity.** Lazy FP under SMP needs to migrate
   the FP context if a thread runs on a different CPU
   than its last owner. Eager just saves/restores at the
   cswitch boundary; nothing CPU-affine to track.

The cost is roughly 32 STP/LDP pairs per context switch. In
practice this is invisible against the timer-tick rate (250 Hz
in this build); there's no perceptible boot-time difference
between before/after the chapter.

---

## What you'll write

| File | Change |
|---|---|
| `kernel/arch/mmu.S` | After enabling SCTLR.M, RMW `CPACR_EL1.FPEN = 0b11` on every CPU |
| `kernel/arch/context_switch.s` | `cswitch_to` frame grows from 288 to **816** bytes; save/restore q0..q31 + fpsr + fpcr around the existing GPR save/restore |
| `kernel/core/thread.h` | `FRAME_SIZE` macro 288 → 816 |
| `kernel/core/thread.c` | Comment update; all callers already use the macro |
| `userspace/libc/setjmp.S` | Save / restore `d8..d15` per AAPCS64 |
| `Makefile` | Drop `-mgeneral-regs-only` from `USER_CFLAGS`; add `fptest` build target |
| `userspace/fptest/fptest.c` | New ~190-line regression — basic FP, yield-preservation, setjmp-preservation |
| `scripts/test_fp.py` | New regression harness — boots, runs `fptest`, looks for "all checks passed" |

`CFLAGS` (kernel) keeps `-mgeneral-regs-only`. `BEARSSL_CFLAGS`
keeps it too. The discipline is: anything that runs in EL1
must not emit FP instructions, because EL1 never restores FP
on entry to itself. Only EL0 code is allowed to use the FPU.

---

## Step 1 — enable the FPU at EL1

AArch64's coprocessor trap register is `CPACR_EL1`. Two bits
that matter:

- **FPEN[21:20]** — 4-bit field. `0b00` traps all FP/SIMD
  from EL0 and EL1. `0b01` traps EL0 only. `0b11` traps
  neither. Reset value is `0b00`.
- **ZEN[17:16]** — SVE. We don't use SVE; leave it at 00.

In `kernel/arch/mmu.S`, right after the `msr SCTLR_EL1, x0`
that turns the MMU on:

```asm
    /* Chapter 171 — enable FP/SIMD at EL0 and EL1.
     * Read-modify-write CPACR_EL1.FPEN = 0b11. */
    mrs     x0, CPACR_EL1
    mov     x1, #(0b11 << 20)
    orr     x0, x0, x1
    msr     CPACR_EL1, x0
    isb
```

The `isb` is non-negotiable. CPACR changes do not take effect
on subsequent instructions until after a context-synchronising
event. Without it, the very next instruction (which may be the
return to whatever called `mmu_enable`) can still trap if it
happens to be FP — and even though `mmu_enable` itself never
emits FP, the kernel functions that run shortly after on the
same CPU are not guaranteed to.

This runs once per CPU because `mmu_enable` runs once per CPU
during PSCI bring-up (chapter 87). After this point, every EL0
thread on every CPU sees `FPEN=11` for its entire lifetime.

---

## Step 2 — grow the context-switch frame

`cswitch_to(prev, next)` saves the outgoing thread's CPU state
to its kernel stack, loads the incoming thread's saved state
from its kernel stack, and returns to wherever the new thread
last yielded. Before this chapter the frame was 288 bytes:

```
offset 0..255      x18..x30, x0..x17        (general-purpose registers, 32 × 8)
offset 256..271    ELR_EL1, SPSR_EL1        (return PC and PSTATE)
offset 272..287    SP_EL0, pad              (user stack pointer + padding)
```

(Exact field ordering is the chapter-11 layout; don't disturb
it. New fields stack *on top*.)

After this chapter:

```
offset 0..271      (unchanged)
offset 272..287    SP_EL0 + pad             (unchanged)
offset 288..799    q0..q31                  (32 × 16-byte Q registers via STP Q)
offset 800         fpsr                     (low 32 bits via STR X)
offset 808         fpcr                     (low 32 bits via STR X)
```

So 528 new bytes — sixteen `stp q*, q*` instructions covers
`q0..q31`, plus two `str` instructions for `fpsr`/`fpcr`. The
new total `FRAME_SIZE` is **816 bytes**.

### The save sequence

In `kernel/arch/context_switch.s`, after the existing GPR /
ELR / SPSR / SP_EL0 save block:

```asm
    /* Chapter 171 — save FP/SIMD state.  Per AAPCS64 every
     * Q register may be live in a yielding user thread, so
     * we save all 32 plus the two status registers. */
    stp     q0,  q1,  [sp, #288]
    stp     q2,  q3,  [sp, #320]
    stp     q4,  q5,  [sp, #352]
    stp     q6,  q7,  [sp, #384]
    stp     q8,  q9,  [sp, #416]
    stp     q10, q11, [sp, #448]
    stp     q12, q13, [sp, #480]
    stp     q14, q15, [sp, #512]
    stp     q16, q17, [sp, #544]
    stp     q18, q19, [sp, #576]
    stp     q20, q21, [sp, #608]
    stp     q22, q23, [sp, #640]
    stp     q24, q25, [sp, #672]
    stp     q26, q27, [sp, #704]
    stp     q28, q29, [sp, #736]
    stp     q30, q31, [sp, #768]
    mrs     x16, fpsr
    mrs     x17, fpcr
    add     x6, sp, #800
    stp     x16, x17, [x6]
```

### The restore sequence

Symmetric, but **runs first** (before the GPR restore) so that
`x16`/`x17` are still free to use as scratch:

```asm
    /* Chapter 171 — restore FP/SIMD state before GPRs.
     * x16/x17 used as scratch for fpsr/fpcr; the subsequent
     * GPR ldp x16, x17 from offset 128 overwrites them. */
    add     x6, sp, #800
    ldp     x16, x17, [x6]
    msr     fpsr, x16
    msr     fpcr, x17
    ldp     q0,  q1,  [sp, #288]
    /* ... q2..q31 symmetric ... */
    ldp     q30, q31, [sp, #768]
```

### The `stp` immediate-range trap

This bit out of the ARMv8 ARM is easy to miss until the
assembler refuses to assemble it:

| Instruction | Immediate encoding | Range |
|---|---|---|
| `stp Xt1, Xt2, [base, #imm]` | signed 7-bit × 8 | **−512 … +504** |
| `stp Qt1, Qt2, [base, #imm]` | signed 7-bit × 16 | −1024 … +1008 |
| `str Xt, [base, #imm]` (unscaled) | signed 9-bit | −256 … +255 |
| `str Xt, [base, #imm]` (scaled) | unsigned 12-bit × 8 | 0 … +32760 |

The Q-register `stp` reaches `q30,q31,[sp,#768]` fine (768 is
well below 1008). But for `fpsr`/`fpcr` at offset 800 the
X-register `stp` overflows — 800 > 504. The assembler is
explicit:

```
Error: immediate offset out of range -512 to 504 at operand 3
  -- `stp x16,x17,[sp,#800]'
```

Three fixes, in order of preference:

1. **Materialise the address in a scratch register first.**
   `add x6, sp, #800; stp x16, x17, [x6]`. One extra
   instruction; clearest intent. This is what the chapter
   uses.
2. **Two `str` instructions in scaled form.** `str x16, [sp,
   #800]; str x17, [sp, #808]`. Scaled-`str` accepts offsets
   up to 32760. Same number of instructions; loses the "these
   two are paired" hint.
3. **Use a different base register.** Could keep an "FP-area"
   pointer in `x6` for the whole save block. More invasive;
   no upside.

This is worth remembering for any future per-thread state
that wants to live beyond offset 504.

### Why save the *whole* register file?

A user thread may have any of `q0..q31` live at the moment
it yields. AAPCS64 (the standard ARM calling convention)
designates:

- `q0..q7` — argument / scratch registers.
- `q8..q15` — actually `d8..d15` are callee-saved (low 64
  bits only); the high 64 bits of `q8..q15` are caller-saved.
- `q16..q31` — caller-saved.

A yield can happen at *any* instruction boundary, not just at
a function call. So you cannot trust the "caller-saved" half
— the thread could be mid-expression with values held in
`q20`. Save all 32 Q registers. (For `setjmp` the rule is
different: setjmp is a function call, so only `d8..d15` are
live across it. See step 4.)

---

## Step 3 — `FRAME_SIZE` and the thread-construction code

`kernel/core/thread.h` defines:

```c
#define FRAME_SIZE 816   /* 272 GPR/ELR/SPSR + 16 SP_EL0+pad + 528 FP (ch 129) */
```

`kernel/core/thread.c::thread_create` and friends already use
this macro for the initial frame layout when a thread is
born. No FP state needs initialising — the new thread's first
"restored" FP registers are all zeros from the kernel-stack
allocator (`bzero`-on-allocation invariant), which is a valid
FP state (`fpsr=0, fpcr=0` is what the spec calls "default
NaN, round-to-nearest, no exceptions enabled"). The first
`stp q0, q1` from this zero region restores 32 IEEE-754
zeros. The thread starts life seeing zeroed FP — exactly what
a freshly-`exec`'d process expects.

The only thing to remember: any place that builds a kernel
frame by raw byte offset (none in tree — everywhere goes
through `FRAME_SIZE` or struct accessors) would need to be
updated. After the change, `grep` for the literal `288` and
confirm only comments reference it.

---

## Step 4 — `setjmp` / `longjmp` and `d8..d15`

`setjmp` is a function call. So AAPCS64 says only the
callee-saved subset is live across it. For integer registers
that's `x19..x29` plus `sp` plus `lr`. For FP registers it's
the **low 64 bits** of `d8..d15` (the high half of `q8..q15`
is not callee-saved; the upper Q half above `d15` is not
saved at all).

Chapter 165's `setjmp.S` only handled the integer set,
leaving the FP slots as a TODO. The trap for any code that
calls `setjmp` while holding a `double` in `d8` is silent
corruption: `longjmp` returns and `d8` has whatever the
intervening computation left there.

The fix is mechanical. Eight `stp d*, d*` pairs in `setjmp`,
matching eight `ldp d*, d*` pairs in `longjmp`:

```asm
setjmp:
    /* ... existing x19..x29, sp, lr save into [x0, #0..#96] ... */
    stp     d8,  d9,  [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    mov     w0, wzr
    ret

longjmp:
    /* ... existing GPR restore from [x0, #0..#96] ... */
    ldp     d8,  d9,  [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]
    /* ... val handling, then ret ... */
```

`d`-form `stp` is the 64-bit Q-pair (i.e. saves 16 bytes per
instruction). Offsets are 104, 120, 136, 152 — eight pairs at
16 bytes each = 64 bytes added. `jmp_buf` was already sized
with room for these (the header reserved space; the fields
were marked "// reserved for FP").

Tested in `userspace/fptest/fptest.c::test_setjmp_fp` by
pinning known values into `d8..d15` via `register double dN
asm("dN")` bindings, calling `longjmp_back()` which clobbers
all eight with `99.0`, and verifying the post-`longjmp` values
match the pre-`setjmp` values.

---

## Step 5 — drop `-mgeneral-regs-only` from `USER_CFLAGS`

In `Makefile`:

```diff
-USER_CFLAGS := -ffreestanding -nostdlib -nostartfiles \
-               -mcpu=cortex-a72 -mgeneral-regs-only \
-               -fno-stack-protector -fno-pie -fno-pic \
-               ...
+USER_CFLAGS := -ffreestanding -nostdlib -nostartfiles \
+               -mcpu=cortex-a72 \
+               -fno-stack-protector -fno-pie -fno-pic \
+               ...
```

`-mgeneral-regs-only` tells GCC "don't emit any FP/SIMD
instructions, even for things like passing a struct by value
that GCC might otherwise vectorise". With this flag, any
`double` operand is a compile error:

```
error: ‘-mgeneral-regs-only’ is incompatible with the use of
       floating-point types
```

So dropping it has two effects:

1. **Real `double` arithmetic compiles.** `fptest.c` now
   compiles. So does `difftime`. So can future Doom and GCC.
2. **GCC may opportunistically vectorise.** A loop that
   copies 32 bytes might become a single `ldp q*, q*` /
   `stp q*, q*` pair. That's fine — all 32 Q regs are saved
   and restored on context switch — but the user binaries
   are marginally larger and tighter. (Not measured
   carefully; no observed regression.)

`CFLAGS` (kernel) **keeps** `-mgeneral-regs-only`. The kernel
must not emit FP instructions: it does not have an FP context
of its own, and any FP register touched by kernel code would
corrupt whatever EL0 thread last ran on that CPU.

`BEARSSL_CFLAGS` keeps `-mgeneral-regs-only` too, because
BearSSL is linked into both kernel-side test paths and the
user-side TLS bridge. Safer to ban FP from it entirely than
audit which call paths run in EL1.

---

## Step 6 — the regression: `userspace/fptest/fptest.c`

Three test functions. Marker on success: `"all checks passed"`.
Marker on individual failures: `"FAIL <file>:<line>: <expr>"`.

### `test_basic_fp` — proves FP doesn't trap

```c
double a = 3.14159265358979;
double b = 2.71828182845905;
double c = a * b;                 /* ≈ 8.5397... */
CHECK(dclose(c, 8.539734222673566));

double d = (a + b) / (a - b);     /* ≈ 13.8429... */
CHECK(dclose(d, 13.842959201997754));

double x = 2.0;
for (int i = 0; i < 20; i++)
    x = 0.5 * (x + 2.0 / x);      /* Newton's sqrt */
CHECK(dclose(x, 1.4142135623730951));
```

`dclose(a, b)` is a 1e-9 absolute-tolerance comparison —
inlined since `<math.h>` doesn't exist yet. Printing happens
through `print_double_parts(name, x)`, which decomposes the
double into integer + fractional×1e6 parts and prints with
`%ld.%06ld` (no `%f` dependency; `%f` is deferred).

The expected values come from running the *same* expressions
in CPython, then taking `repr()`. CPython's `double` is IEEE
754 64-bit just like ours, so the bit pattern matches and
`dclose` passes trivially.

> **Tip.** Always cross-check expected doubles against an
> authoritative IEEE implementation, not against arithmetic
> done in your head. `(a+b)/(a-b)` with `a = pi` and `b = e`
> *looks* like "about 13.844" by eye; the actual IEEE result
> is `13.842959201997754`.

### `test_yield_preserves_fp` — proves context switch saves FP

```c
volatile double v = 1.4142135623730951;
sleep_ms(10);
double r = v;
CHECK(dclose(r, 1.4142135623730951));
```

`volatile` keeps the compiler from constant-folding across
the `sleep_ms`. `sleep_ms` is a syscall that schedules other
threads, so by the time control returns the per-CPU FP file
has held *other* threads' values and been restored to this
thread's. If `cswitch_to` were missing FP save/restore, `r`
would either be zero (frame init), the idle thread's last FP
value, or in the worst case whatever the kernel happened to
leave there (this is precisely why kernel code must be
`-mgeneral-regs-only` — to make that last case impossible).

Repeated with four distinct doubles in four named locals
across two short sleeps to exercise multiple FP register
slots, not just one.

### `test_setjmp_fp` — proves d8..d15 round-trip

`register double dN asm("dN")` bindings pin known values
into specific FP registers. `longjmp_back()` clobbers all
eight with `99.0`. After `longjmp`, re-read via fresh
`register double rN asm("dN")` bindings; verify each matches
its pre-setjmp value.

This is the smallest test that actually exercises the
`setjmp.S` changes from step 4.

---

## Pitfalls

### Pitfall — `stp x16,x17,[sp,#800]` out of range

**Symptom.** Assembler refuses: `immediate offset out of
range -512 to 504`.

**Cause.** X-register `stp` only encodes offsets up to +504.

**Fix.** Materialise the address in a scratch register:
`add x6, sp, #800; stp x16, x17, [x6]`. See the
"`stp` immediate-range trap" table above.

### Pitfall — `#include <stdio.h>` in `fptest.c`

**Symptom.** Freestanding build can't find `stdio.h` on a
standard include path.

**Cause.** Convention in this codebase is to use the
project-relative path (`"../libc/printf.h"`) until chapter
189 lands the full umbrella headers on disk.

**Fix.** Match the include style of every other test binary:
`#include "../libc/printf.h"`.

### Pitfall — hand-computed expected `double` values

**Symptom.** Test fails with `(a+b)/(a-b) = 13.842959` vs
expected `13.844`.

**Cause.** IEEE 754 result is not what your head produces.

**Fix.** Compute the expected value in CPython (or any other
IEEE-754-compliant runtime), use `repr(x)` to get the exact
double, paste into the test.

---

## What's deferred

- **`%f` / `%e` / `%g` in `printf`** — needs the dragon4 /
  Grisu / Ryū algorithm for round-trip printing. None of
  Doom or GCC's runtime artifacts call `printf("%f", ...)`
  directly (Doom prints via integer demos; GCC's runtime is
  data-only). Punted.
- **`strtod` / `strtof` in `stdlib`** — needs the inverse
  parser. `atof` is a one-liner once `strtod` exists; today
  no in-tree caller needs it.
- **`scanf %f`** — same parser as `strtod`.
- **`math.h`** — none of `sin`/`cos`/`log`/`exp` are in
  scope yet. Doom's renderer uses a precomputed `finetangent`
  table; libpng can be configured without `pow`. Land math.h
  when something concrete demands it.

---

## What this unlocks

- **`difftime` (chapter 168)** — was a stub returning
  `(double)(int)(b - a)` with the cast forced through long
  to dodge FP. Now does the real `(double)b - (double)a`
  subtraction in IEEE arithmetic. No app currently calls it,
  but the next time a port wants it, it works.
- **`setjmp` / `longjmp` (chapter 165)** — now AAPCS64-
  compliant for FP-using callers. Doom's `I_Error` recovery
  path uses `setjmp`; the previous chapter-165 version
  would have silently dropped any FP register state the
  caller held in `d8..d15`.
- **`userspace/fptest`** — new binary, regression only. Not
  a user-facing tool, but demonstrates the surface.

Future-app uplift this chapter unlocks:

- Browser (`userspace/browser/`) can do real CSS pixel math
  with `font-size: 1.5em` etc. Currently uses fixed-point
  with a `* 1000` denominator scheme.
- Notepad's word-count statistics can compute a real average
  word-length instead of integer division. (Cosmetic.)

---

## Run it / Test it

- New: `scripts/test_fp.py` — boots, runs `fptest`, requires
  "all checks passed".
- Sweep: every existing `scripts/test_*.py` still passes.
  Particularly relevant ones to verify (because they exercise
  cswitch heavily): `test_threading.py`, `test_smp.py`,
  `test_browser_*` (all of them), `test_libc_*` (all of them).
- Manual: `fptest` from the desktop shell. Prints the
  decomposed double values and the per-check verdicts.

---

## Things to remember

1. **Eager FP save/restore is the right default for a small
   kernel.** It costs ~32 stp/ldp pairs per context switch
   and removes an entire trap-handling code path. Only go
   lazy when you've measured the cost and proved most threads
   don't use FP.

2. **The kernel-stays-`-mgeneral-regs-only` invariant is
   load-bearing.** It's the reason there's no separate kernel
   FP context: there is no kernel FP state to save. Whenever
   you add a kernel feature that wants to do FP arithmetic
   (e.g. an audio mixer doing volume scaling), the right
   answer is *push the computation to userspace*, not *add
   FP support to the kernel*.

3. **AArch64 `stp` immediate ranges differ by register
   width.** Q-register pair-store reaches 1008; X-register
   pair-store only 504. If you're laying out a frame larger
   than 512 bytes, the X-register pairs need to be in the
   first half. Or use `str`/`ldr` (12-bit range = 32760),
   or compute the address in a scratch register.

4. **`isb` after `CPACR_EL1` writes is non-optional.** Most
   AArch64 system-register writes only take effect after a
   context-synchronisation event. Forgetting the `isb`
   produces flaky boots where the FPU is enabled "most of
   the time" depending on instruction scheduling.

5. **Test expected values against a known-good IEEE 754
   implementation.** Don't compute them by hand. CPython's
   `repr(x)` for a `float` gives you the exact double; that's
   what your guest will compute too.

---

## What's next

Chapter 172 — cross-build a working Doom on the host
toolchain. Vanilla Doom sources, `aarch64-elf-gcc` cross,
output an ELF that *would* run in our guest if it could
read its own WAD. (173 adds WAD reading via the existing
`open`/`read` syscalls; 174 brings the framebuffer.)

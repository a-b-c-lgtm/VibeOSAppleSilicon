# Chapter 10 — Kernel threads and the AArch64 context switch

> **Milestone in this chapter:** 4 — cooperative kernel threads.
> **Code referenced:** [kernel/arch/context_switch.s](../../../kernel/arch/context_switch.s),
> [kernel/core/thread.h](../../../kernel/core/thread.h),
> [kernel/core/thread.c](../../../kernel/core/thread.c),
> [kernel/core/main.c](../../../kernel/core/main.c) (`thread_demo`).
>
> **At the end of this chapter** you will have a kernel that can
> spawn multiple named threads, give them tiny stacks of their
> own, and round-robin between them whenever any of them calls
> `yield()`. The next chapter wires the timer ISR into the same
> path so threads get preempted whether they yield or not. The
> two-step split — cooperative first, preemptive second — is
> deliberate: cooperative scheduling is purely a software
> exercise, while preemption introduces architectural concerns
> (in particular what `SPSR_EL1` looks like when the scheduler is
> entered from inside an exception handler) that are easier to
> reason about once the cooperative path already works.

## What a "thread" actually is

A thread is three things glued together:

1. A **stack** — its own region of memory where its local
   variables and call frames live.
2. A **register snapshot** — the GPRs, the program counter, and
   the processor state (`PSTATE`) that need to be restored to
   resume it.
3. A **bookkeeping record** that the scheduler can put on a
   queue, mark RUNNING/READY/EXITED, etc.

Everything else — names, priorities, parent/child relationships,
file descriptors — is policy on top of those three. We will keep
adding policy in later chapters; for now the absolute minimum is
all we need.

The `struct thread` we settle on is correspondingly small:

```c
enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_EXITED,
};

struct thread {
    uint64_t            sp;          /* saved stack pointer    */
    uint8_t            *stack_base;  /* heap-allocated stack   */
    size_t              stack_size;
    int                 id;
    enum thread_state   state;
    const char         *name;
    struct thread      *next;        /* runqueue link          */
};
```

The `sp` field is the only register we ever touch from C — every
other register is saved on the thread's own stack, in a frame
whose layout is dictated by the assembly context switch we are
about to write. The implementation lives in
[kernel/core/thread.c](../../../kernel/core/thread.c) and is
under 200 lines including comments.

## Picking a frame layout

When we save a thread's registers we have to put them
*somewhere*. The natural place is the thread's own stack: it is
already allocated, it is private to that thread, and unwinding
it is a single SP write away. The question is what the saved
frame should look like.

There are two reasonable answers:

1. **A minimal AAPCS-callee-saved frame.** The AArch64
   procedure call standard (AAPCS) divides registers into
   caller-saved and callee-saved. A function that calls
   `cswitch_to` has already had any caller-saved values it cared
   about spilled by the compiler, so we only need to preserve
   the callee-saved set: `x19`–`x29` and `x30` (the link
   register). That is 12 registers, plus 8 bytes of padding to
   keep SP 16-byte aligned, total 104 bytes per saved context.
   It is small and fast.

2. **A full exception frame.** Save everything — all 31 GPRs,
   plus `ELR_EL1` and `SPSR_EL1` — and use `eret` to resume.
   This is bigger (272 bytes) and slower, but it has one
   enormous advantage: the frame is *identical in shape* to the
   one the IRQ entry vector pushes when the CPU takes an
   interrupt.

We pick option 2, even though we are about to use it from a
purely cooperative call. The reason is the chapter that follows
this one. As soon as we hook the timer into the scheduler the
context switch is going to be invoked from inside an IRQ
handler, where the thread's *full* register set is already on
the stack. Having the cooperative path build the same frame
shape means there is no special "preemption-only" code to write
later: the same `cswitch_to` works from C and from an exception
handler interchangeably. We pay 168 extra bytes per switch and
get preemption almost for free.

The frame layout matches `save_context` in
[kernel/arch/vectors.S](../../../kernel/arch/vectors.S) exactly:

```
[sp +   0..15]   x0,  x1
[sp +  16..31]   x2,  x3
   …
[sp + 224..239]  x28, x29
[sp + 240..247]  x30
[sp + 248..255]  padding
[sp + 256..263]  ELR_EL1
[sp + 264..271]  SPSR_EL1
```

Total: 272 bytes, 16-byte aligned. Both `cswitch_to` and
`irq_entry` push this exact shape; both restore it the same way.

## `cswitch_to` walked in three parts

The function lives in
[kernel/arch/context_switch.s](../../../kernel/arch/context_switch.s):

```c
extern void cswitch_to(uint64_t *save_sp, uint64_t load_sp);
```

It is a C-callable assembly routine with two arguments: a
pointer to the outgoing thread's `sp` field, and the incoming
thread's already-saved `sp` value. Its job, in plain English:

1. Build a 272-byte frame on the current stack.
2. Write the new SP into `*save_sp`.
3. Switch SP to `load_sp`.
4. Restore the frame at the new SP and `eret`.

### Step 1 — push the frame

```asm
sub     sp, sp, #272

stp     x0,  x1,  [sp, #0]
stp     x2,  x3,  [sp, #16]
…
stp     x28, x29, [sp, #224]
str     x30,      [sp, #240]
```

We deliberately save *every* GPR, including `x0`/`x1` (which
hold the function arguments). Strictly speaking AAPCS lets us
clobber the caller-saves, but if we are ever going to `eret`
into a thread that was last seen in the middle of executing
unrelated work (the preemption path of the next chapter), every
register matters. Uniformity wins.

For the synthesised exception-state slots we have to be more
careful:

```asm
mov     x16, #0x345
stp     x30, x16, [sp, #256]    /* elr_el1, spsr_el1 */
```

`ELR_EL1` is set to the link register — that is the address
inside the C caller that we want to return to after the eventual
`eret`. `SPSR_EL1` is set to the constant `0x345`, which deserves
its own paragraph (and a sidebar in the next chapter).

### Sidebar — why SPSR is hardcoded, not captured

`0x345` is `M[3:0]=0101 (EL1h)`, `F=1`, `I=0`, `A=1`, `D=1` — in
other words "stay at EL1, IRQs unmasked, FIQ/SError/Debug
masked". We *could* have written

```asm
mrs     x16, nzcv
mrs     x17, daif
orr     x16, x16, x17
mov     x17, #0x5
orr     x16, x16, x17
```

to capture the live PSTATE bits. We must not. The next chapter
calls `cswitch_to` from inside an IRQ handler, and on
exception entry the architecture *automatically* sets
`PSTATE.I = 1`. If we captured it then the resumed thread would
run with IRQs masked and never get preempted again. Hardcoding
the SPSR makes the cooperative path identical in spirit to what
we will need for preemption, and saves us from a debugging
session where two CPU-bound threads stubbornly refuse to
interleave.

NZCV is also dropped: AAPCS treats condition flags as
caller-saved, so the C call to `cswitch_to` has already
discarded them.

### Step 2 — store SP into `*save_sp`

`x16` and `x17` are AAPCS *intra-procedure-call* scratch
registers — they exist precisely so that helper code like a
linker veneer or a context switch can use them without
disturbing caller state. We use them to dereference `save_sp`
(which lives in the saved frame at offset 0):

```asm
ldr     x16, [sp, #0]           /* x16 = save_sp pointer */
mov     x17, sp
str     x17, [x16]              /* *save_sp = sp          */
```

Note the read-our-own-frame trick: the original `x0` argument
is sitting at offset 0 because we just wrote it there. We could
equally have stashed it before the `stp`s, but pulling it out
of the frame is cheaper than yet another spill slot.

### Step 3 — switch stacks

```asm
ldr     x16, [sp, #8]           /* x16 = load_sp value */
mov     sp, x16
```

After this instruction we are conceptually executing inside the
*incoming* thread. The current thread's saved frame is sitting
at the SP we wrote into `*save_sp`; the incoming thread's frame
is sitting at the SP we are now using.

### Step 4 — restore and `eret`

The restore is the inverse of the save, with one ordering
constraint: we must write `ELR_EL1` and `SPSR_EL1` before
restoring `x0`/`x1`, because the `msr` instructions go through
those registers.

```asm
msr     daifset, #2
ldp     x0,  x1,  [sp, #256]
msr     elr_el1, x0
msr     spsr_el1, x1
ldr     x30,      [sp, #240]
ldp     x28, x29, [sp, #224]
…
ldp     x0,  x1,  [sp, #0]
add     sp, sp, #272
eret
```

`eret` from EL1h to EL1h is valid: it loads `ELR_EL1` into the
PC, `SPSR_EL1` into PSTATE, and resumes execution. Because
`SPSR_EL1.I = 0`, IRQs are unmasked again on the new thread.

The `msr daifset, #2` at the very top is what keeps a stray
timer tick out of this sequence — without it, an IRQ that lands
between `msr elr_el1, x0` and `eret` will overwrite `ELR_EL1`
with a kernel PC and we will eventually `eret` EL0 to that
kernel address. We discuss why in detail in
[chapter 11](011-preemption.md#the-eret-window-trap).
The masking does not leak into the new thread because `eret`
restores `PSTATE.I` from the saved SPSR, where it is `0`.

## Bootstrapping a brand-new thread

A thread that has never run does not have a "previous frame" to
restore — we have to *forge* one. `thread_create` allocates the
struct, allocates a 16 KiB stack out of `kmalloc`, then writes
the initial frame at the top of that stack:

```c
uint8_t *frame_top   = stack + THREAD_STACK_SIZE;
uintptr_t top_aligned = ((uintptr_t)frame_top) & ~((uintptr_t)0xF);
uint8_t *frame       = (uint8_t *)(top_aligned - FRAME_SIZE);

for (size_t i = 0; i < FRAME_SIZE; i++)
    frame[i] = 0;

uint64_t *gpr = (uint64_t *)frame;
gpr[19] = (uint64_t)(uintptr_t)entry;     /* x19 = entry fn */
gpr[20] = (uint64_t)(uintptr_t)arg;       /* x20 = arg      */

*(uint64_t *)(frame + 240) = (uint64_t)(uintptr_t)thread_trampoline;
*(uint64_t *)(frame + 256) = (uint64_t)(uintptr_t)thread_trampoline;
*(uint64_t *)(frame + 264) = 0x345ULL;

t->sp = (uint64_t)(uintptr_t)frame;
```

Three planted slots are doing all the work:

* **`x19`** holds the entry function pointer.
* **`x20`** holds the argument to pass.
* **`x30` and `ELR_EL1`** both point at `thread_trampoline`.
  Why both? `x30` because if anyone ever issues a `ret` (rather
  than the `eret` we are planning) it will land in the right
  place; `ELR_EL1` because that is what the `eret` actually
  uses. They agree, so it does not matter which path executes.
* **`SPSR_EL1`** is the same `0x345` we synthesise during a
  live switch — kernel mode, IRQs on.

Why callee-saves (`x19`, `x20`) for the entry function and arg,
rather than `x0` and `x30` directly? Because the trampoline is
written in assembly precisely so the values survive any function
prologue the compiler might emit. AAPCS guarantees `x19`–`x28`
are preserved across calls; nothing is guaranteed about `x0`
once a function returns. We park the data in safe registers and
let the trampoline marshal it.

## The trampoline

```asm
.global thread_trampoline
thread_trampoline:
    mov     x0, x20             /* arg → x0 */
    blr     x19                 /* call entry(arg) */
    bl      thread_exit         /* never returns */
1:  wfe
    b       1b
```

Three instructions of useful work plus a defensive halt loop.
`mov x0, x20` puts the argument in the AAPCS-mandated first
slot; `blr x19` calls the entry function; if the entry function
ever returns, `bl thread_exit` cleans up. The trailing `wfe`
loop only exists so that if `thread_exit` itself ever malfuncs
the CPU stops instead of running off the end of the section.

The trampoline *must* be assembly. A C version would have a
function prologue that immediately spills `x19`/`x20` to make
room for its own locals — defeating the entire reason we used
those registers in the first place.

## The runqueue and `yield()`

With `cswitch_to` and `thread_trampoline` in hand, the
scheduler is mostly bookkeeping. We keep:

```c
static struct thread *g_current;
static struct thread *g_runq_head;
static struct thread *g_runq_tail;
static struct thread *g_zombie;
```

`g_current` is the thread on the CPU; the runqueue is a
NULL-terminated singly linked list of READY threads in
arrival order. `g_zombie` holds a thread that has called
`thread_exit` but whose stack we cannot free yet — *we are
still standing on it*. The next scheduling pass reaps it.

```c
void yield(void)
{
    reap_zombie();

    struct thread *prev = g_current;
    struct thread *next = runq_pop();
    if (!next) return;                 /* nobody else ready */

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        runq_push(prev);
    }
    next->state = THREAD_RUNNING;
    g_current   = next;

    cswitch_to(&prev->sp, next->sp);
}
```

That is the whole scheduler. Round-robin, FIFO, no priorities,
no locks (because we are uniprocessor and IRQs are masked
inside `cswitch_to`'s eret-restored frame for exactly the right
window). Adding any of those features later is policy, not
mechanism.

`thread_exit` is even shorter:

```c
void thread_exit(void)
{
    g_current->state = THREAD_EXITED;
    g_zombie         = g_current;
    yield();
    for (;;) __asm__ volatile("wfe");
}
```

It marks itself EXITED, parks the pointer in `g_zombie`, and
yields. The next `yield()` from any thread will see the zombie
in `reap_zombie()` and free its stack and struct.

## The boot thread

Before any of this can run, we need `g_current` to point at
*something*. The very first time `cswitch_to` is called, it
will write the boot CPU's saved SP into `&g_current->sp` — so
`g_current` must be a live, kmalloc'd `struct thread`.
`thread_init` does that:

```c
void thread_init(void)
{
    struct thread *boot = kmalloc(sizeof(*boot));
    boot->sp         = 0;            /* filled in by first cswitch_to */
    boot->stack_base = NULL;         /* boot stack lives in linker .stack */
    boot->stack_size = 0;
    boot->id         = g_next_id++;
    boot->state      = THREAD_RUNNING;
    boot->name       = "boot";
    boot->next       = NULL;
    g_current        = boot;
    g_thread_count   = 1;
}
```

The boot thread is special only in that we never allocated its
stack — it lives in the `.stack` section reserved by the linker
script. We mark `stack_base = NULL` so that if the boot thread
ever calls `thread_exit` (it does not, but defensively) we will
not try to `kfree` something we never `kmalloc`'d.

## The demo

`thread_demo()` in [kernel/core/main.c](../../../kernel/core/main.c)
spawns two named workers that each print a few lines and yield
between them. The boot thread waits for them with

```c
while (thread_count() > 1) yield();
```

A clean run produces:

```
[thread] spawning two cooperative workers
[worker-A] iter 0x0000000000000000
[worker-B] iter 0x0000000000000000
[worker-A] iter 0x0000000000000001
[worker-B] iter 0x0000000000000001
[worker-A] iter 0x0000000000000002
[worker-B] iter 0x0000000000000002
[worker-A] done
[worker-B] done
[thread] all workers reaped
```

Two things to notice:

* The interleaving is exact — A, B, A, B — because each `yield`
  rotates the runqueue by exactly one slot.
* "done" lines are not interleaved; they are the last thing
  each worker prints before returning, and `thread_exit` does
  not yield to anyone except the next-up thread, which is the
  *other* worker (still running) on A's exit and the *boot
  thread* on B's exit.

After both workers reap, the heartbeat resumes. The heap
returns to its baseline (boot-thread struct + the kmalloc
free-list header) — proof that `reap_zombie` released both
worker stacks.

## What chapter 11 changes

Cooperative scheduling is fragile. A worker that forgets to
call `yield` monopolises the CPU forever. We could insist that
every long-running loop sprinkle yields throughout — many
historical kernels did exactly that — but the CPU already has a
mechanism that interrupts arbitrary code at fixed intervals: the
generic timer from chapter 8.

The next chapter hooks the timer ISR into `schedule()`. The
contract becomes: a thread runs until either (a) it calls
`yield()` voluntarily, or (b) the next 100 ms tick fires. The
context switch itself does not change at all. The whole purpose
of the unified 272-byte frame was to make this transition free.

## Checkpoint

Run `make all` followed by

```
qemu-system-aarch64 -M virt,gic-version=3 -cpu host -accel hvf \
    -m 2G -nographic -kernel build/kernel.elf
```

You should see the two-worker interleave above, followed by the
heartbeat lines from chapter 8. `Ctrl-A X` quits. With that
working, chapter 11 is a four-line edit to `irq_dispatch`.

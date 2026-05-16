/*
 * kernel/arch/context_switch.S — unified context switch.
 *
 *   void cswitch_to(uint64_t *save_sp, uint64_t load_sp);
 *
 * Builds a 272-byte exception-frame-shaped context on the current
 * stack, stores the resulting SP into *save_sp, loads `load_sp`
 * into SP, restores the frame found there, and `eret`s.
 *
 * The frame layout is identical to the IRQ-entry frame produced
 * by save_context in vectors.S:
 *
 *   [sp +   0..15]  x0,  x1
 *   [sp +  16..31]  x2,  x3
 *      ...
 *   [sp + 224..239] x28, x29
 *   [sp + 240..255] x30, padding
 *   [sp + 256..271] ELR_EL1, SPSR_EL1
 *
 * Using the same shape for both cooperative and preemptive context
 * switches means the two code paths interoperate freely:
 *
 *   - A cooperative cswitch_to from C synthesises ELR_EL1 = x30
 *     (the C return address) and SPSR_EL1 = current PSTATE.
 *   - An IRQ preemption from irq_dispatch() reuses the IRQ-entry
 *     frame already on the stack (cswitch_to writes a second one
 *     on top of it; both unwind correctly when the thread is
 *     resumed).
 *
 * `eret` from EL1h to EL1h is valid: it copies SPSR_EL1 into PSTATE
 * and ELR_EL1 into PC.  PSTATE.M = 0b0101 keeps us in EL1h.
 *
 * x16/x17 are AAPCS intra-procedure-call scratch; we use them as
 * working registers because they are guaranteed not to hold
 * caller state.
 */

.section .text
.global cswitch_to
cswitch_to:
    sub     sp, sp, #288

    /* Save every GP register, including x0/x1 (the args).  This is
     * deliberately uniform with the IRQ-entry frame so the same
     * restore path works either way. */
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    /* Synthesise an exception-style ELR/SPSR.  ELR is just the
     * return address (x30 — where the C caller wants to resume).
     *
     * SPSR is hardcoded for kernel threads: M[3:0] = 0101 (EL1h),
     * F = A = D = 1 (FIQ / SError / Debug masked), I = 0 (IRQs
     * unmasked).  Bits laid out:
     *
     *   bit 9 D = 1
     *   bit 8 A = 1
     *   bit 7 I = 0   <-- IRQs ON for the resumed thread
     *   bit 6 F = 1
     *   bit 4 M[4]= 0
     *   bit 3..0 M[3:0] = 0101
     *
     * Value = 0x345.
     *
     * We deliberately do NOT capture the current DAIF — when this
     * function runs from inside an IRQ handler (the preemption
     * path), DAIF.I = 1 because the architecture auto-masks IRQs
     * on exception entry.  Capturing it would cause the resumed
     * thread to run with IRQs masked and never get preempted
     * again.  We also do not bother preserving NZCV: the AAPCS
     * makes condition flags caller-saved, so the C call to
     * cswitch_to has already discarded them. */
    mov     x16, #0x345
    stp     x30, x16, [sp, #256]    /* elr_el1, spsr_el1 */

    /* Capture SP_EL0 alongside the frame so it survives the
     * context switch.  When this thread resumes, the matching
     * load below restores SP_EL0 before eret.  Critical for user
     * threads: each one has its own SP_EL0 and we must not let
     * one user thread run with another's stack pointer. */
    mrs     x16, sp_el0
    stp     x16, xzr, [sp, #272]    /* sp_el0 (+ pad for 16-byte align) */

    /* Persist current SP into *save_sp.  Original x0 (save_sp) is
     * in the frame at offset 0; original x1 (load_sp) at offset 8. */
    ldr     x16, [sp, #0]           /* x16 = save_sp pointer       */
    mov     x17, sp

    /* M58 DIAG: trap if the SP we are about to save is outside the
     * heap range [0x80000000, 0x90000000), the boot-stack range
     * [stack_bottom, stack_top], OR (chapter 89) the secondary
     * boot-stack range [secondary_stacks_bottom,
     * secondary_stacks_top].  Originally caught the M58
     * boot->sp=0x400d8310 corruption; chapter 89 added the third
     * range so per-CPU idle threads (which run on .secondary_stacks)
     * pass the check. */
    mov     x6, #0x80000000
    cmp     x17, x6
    b.lo    9000f                   /* < 0x80000000 -> check stack ranges */
    movz    x6, #0x9000, lsl #16
    cmp     x17, x6
    b.lo    9001f                   /* in [0x80000000, 0x90000000) - OK */
    b       9002f                   /* > heap end -> trap */
9000:
    /* Try boot stack first. */
    adrp    x6, stack_bottom
    add     x6, x6, :lo12:stack_bottom
    cmp     x17, x6
    b.lo    9005f                   /* below boot stack -> try secondary */
    adrp    x6, stack_top
    add     x6, x6, :lo12:stack_top
    cmp     x17, x6
    b.hi    9005f                   /* above boot stack -> try secondary */
    b       9001f                   /* in boot stack -> OK */
9005:
    /* Try secondary boot stacks (chapter 89: idle threads on
     * CPU >= 1 live here). */
    adrp    x6, secondary_stacks_bottom
    add     x6, x6, :lo12:secondary_stacks_bottom
    cmp     x17, x6
    b.lo    9002f
    adrp    x6, secondary_stacks_top
    add     x6, x6, :lo12:secondary_stacks_top
    cmp     x17, x6
    b.hi    9002f
    /* fall through to OK */
9001:
    str     x17, [x16]              /* *save_sp = sp                */
    b       9003f
9002:
    /* SP about to be saved is outside any valid kernel stack.
     * Print a marker, the bad SP value, AND the &prev->sp pointer
     * so we can identify which thread struct receives this. */
    mov     x6, #0x09000000         /* PL011 base */
    mov     w7, #'X'; str w7, [x6]
    mov     w7, #'X'; str w7, [x6]
    mov     w7, #'X'; str w7, [x6]
    mov     w7, #' '; str w7, [x6]
    mov     w7, #'s'; str w7, [x6]
    mov     w7, #'p'; str w7, [x6]
    mov     w7, #'='; str w7, [x6]
    mov     w7, #'0'; str w7, [x6]
    mov     w7, #'x'; str w7, [x6]
    /* Print x17 (bad sp) as 16 hex nibbles, MSB first.  x18 is
     * scratch shift counter, x19 nibble.  Both are caller-saved
     * inside cswitch_to so trashing them here is fine. */
    mov     x18, #60
9020:
    lsr     x19, x17, x18
    and     x19, x19, #0xF
    cmp     x19, #10
    b.lt    9021f
    add     x19, x19, #('a' - '0' - 10)
9021:
    add     x19, x19, #'0'
    str     w19, [x6]
    sub     x18, x18, #4
    cmp     x18, #0
    b.ge    9020b
    /* Now print &prev->sp (= x16) */
    mov     w7, #' '; str w7, [x6]
    mov     w7, #'s'; str w7, [x6]
    mov     w7, #'a'; str w7, [x6]
    mov     w7, #'v'; str w7, [x6]
    mov     w7, #'e'; str w7, [x6]
    mov     w7, #'='; str w7, [x6]
    mov     w7, #'0'; str w7, [x6]
    mov     w7, #'x'; str w7, [x6]
    mov     x18, #60
9030:
    lsr     x19, x16, x18
    and     x19, x19, #0xF
    cmp     x19, #10
    b.lt    9031f
    add     x19, x19, #('a' - '0' - 10)
9031:
    add     x19, x19, #'0'
    str     w19, [x6]
    sub     x18, x18, #4
    cmp     x18, #0
    b.ge    9030b
    /* Now print x30 (return address = caller of cswitch_to) */
    mov     w7, #' '; str w7, [x6]
    mov     w7, #'l'; str w7, [x6]
    mov     w7, #'r'; str w7, [x6]
    mov     w7, #'='; str w7, [x6]
    mov     w7, #'0'; str w7, [x6]
    mov     w7, #'x'; str w7, [x6]
    mov     x18, #60
9040:
    lsr     x19, x30, x18
    and     x19, x19, #0xF
    cmp     x19, #10
    b.lt    9041f
    add     x19, x19, #('a' - '0' - 10)
9041:
    add     x19, x19, #'0'
    str     w19, [x6]
    sub     x18, x18, #4
    cmp     x18, #0
    b.ge    9040b
    mov     w7, #0x0a; str w7, [x6]
    /* fall into halt loop */
9004:
    wfe
    b       9004b
9003:

    /* Switch to the incoming thread's stack. */
    ldr     x16, [sp, #8]           /* x16 = load_sp value          */
    mov     sp, x16

    /* Mask IRQs across the eret window.  Same reasoning as
     * restore_context in vectors.S: between `msr elr_el1, x0` and
     * `eret`, a stray interrupt would overwrite ELR_EL1 with the
     * kernel return address inside this function and we'd then
     * eret EL0 to a kernel PC.  PSTATE.I is restored from the
     * resumed thread's saved SPSR (0x345 with I=0 for kernel
     * threads, 0x340 with I=0 for user threads), so the masking
     * does not leak into the new context. */
    msr     daifset, #2

    /* Restore SP_EL0 from the incoming frame before reloading the
     * GPRs (so x16 stays scratch). */
    ldr     x16, [sp, #272]
    msr     sp_el0, x16

    /* Restore the new context.  Order matters: we must write
     * ELR_EL1 and SPSR_EL1 before restoring x0/x1 because the
     * inline pop uses x0/x1 as scratch. */
    ldp     x0,  x1,  [sp, #256]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldr     x30,      [sp, #240]
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8,  x9,  [sp, #64]
    ldp     x6,  x7,  [sp, #48]
    ldp     x4,  x5,  [sp, #32]
    ldp     x2,  x3,  [sp, #16]
    ldp     x0,  x1,  [sp, #0]
    add     sp, sp, #288
    eret

/* thread_trampoline — the address landed at by `eret` for a freshly
 * created thread.  By the time we get here cswitch_to has restored
 * the synthesized initial frame, which set:
 *
 *     x19 = entry function pointer
 *     x20 = argument to pass to entry
 *     x30 = thread_trampoline (so x30 still points here)
 *     x0  = 0 (frame is zero-initialised apart from the named slots)
 *
 * Move the args into the AAPCS-mandated places, call the entry
 * function, then call thread_exit if it ever returns.  Pure asm
 * because we cannot trust C to leave x19/x20 alone across a
 * function prologue. */
.global thread_trampoline
thread_trampoline:
    mov     x0, x20             /* arg → x0 */
    blr     x19                 /* call entry(arg) */
    mov     x0, #0              /* exit code 0 if entry returned   */
    bl      thread_exit         /* never returns */
1:  wfe
    b       1b

/* user_trampoline — first-time launch of a user thread.
 *
 * cswitch_to's eret lands here (still at EL1, because the synthesised
 * SPSR for the trampoline frame is EL1h).  By the time we arrive,
 * the saved frame's restored state is:
 *
 *     x19 = user-space entry virtual address (becomes ELR_EL1)
 *     x20 = user-space SP_EL0 value
 *
 * We now perform the second-stage transition to EL0:
 *   1. Write SP_EL0 from x20.
 *   2. Write ELR_EL1 = x19  (where the user code will start).
 *   3. Write SPSR_EL1 = EL0t with IRQs unmasked.
 *      M[3:0] = 0000, F=A=D=1, I=0  →  0x340.
 *   4. eret.
 *
 * After eret, the CPU is at EL0 executing the user binary with
 * IRQs on, SP = the user stack we set up.  The kernel stack the
 * thread was riding remains parked exactly where it was — every
 * future SVC or preemption from this user thread comes back into
 * the same kernel stack via the EL0 vector slots in vectors.S. */
.global user_trampoline
user_trampoline:
    msr     sp_el0, x20         /* user SP */
    msr     elr_el1, x19        /* user PC */
    mov     x16, #0x340         /* EL0t, F=A=D=1, I=0 */
    msr     spsr_el1, x16
    /* Zero the GPRs so the user program does not see kernel state. */
    mov     x0,  xzr
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret

/* user_clone_trampoline — first-time launch of a SYS_CLONE child.
 *
 * Identical to user_trampoline except the user thread receives
 * arguments instead of being zeroed out:
 *
 *   x19 = user-space entry VA           (becomes ELR_EL1)
 *   x20 = user-space SP_EL0
 *   x21 = arg                           (becomes x0 in user mode)
 *   x22 = TLS pointer                   (becomes TPIDR_EL0)
 *
 * The child's first user-mode instruction is therefore
 *   entry(arg)
 * with TPIDR_EL0 set up so libc's per-thread state can be reached
 * via `mrs <reg>, tpidr_el0` (chapter 91 doesn't actually use TLS
 * yet — the slot is reserved so future per-thread errno / __thread
 * variables don't need another syscall to set up). */
.global user_clone_trampoline
user_clone_trampoline:
    msr     sp_el0, x20         /* user SP */
    msr     elr_el1, x19        /* user PC */
    msr     tpidr_el0, x22      /* per-thread TLS base */
    mov     x16, #0x340         /* EL0t, F=A=D=1, I=0 */
    msr     spsr_el1, x16
    /* x0 = arg; zero the rest so the child doesn't observe
     * kernel state in any other GPR. */
    mov     x0,  x21
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret

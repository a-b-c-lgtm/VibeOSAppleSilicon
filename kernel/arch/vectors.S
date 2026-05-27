/* vectors.S — AArch64 EL1 exception vector table.
 *
 * The hardware reads VBAR_EL1 (Vector Base Address Register) on every
 * exception and jumps to a 128-byte slot computed from the cause.
 * The table itself must be 2 KiB aligned (16 slots × 128 bytes).
 *
 * Slot layout (ARM ARM D1.10.2):
 *   offset 0x000  Sync   from current EL using SP_EL0
 *   offset 0x080  IRQ    from current EL using SP_EL0
 *   offset 0x100  FIQ    from current EL using SP_EL0
 *   offset 0x180  SError from current EL using SP_EL0
 *   offset 0x200  Sync   from current EL using SP_ELx       <— we live here
 *   offset 0x280  IRQ    from current EL using SP_ELx       <— and here
 *   offset 0x300  FIQ    from current EL using SP_ELx
 *   offset 0x380  SError from current EL using SP_ELx
 *   offset 0x400  Sync   from lower EL using AArch64        <— userspace later
 *   offset 0x480  IRQ    from lower EL using AArch64
 *   offset 0x500  FIQ    from lower EL using AArch64
 *   offset 0x580  SError from lower EL using AArch64
 *   offset 0x600..0x780  Lower EL using AArch32 — we never use AArch32.
 *
 * Initially, every slot routes to a single panic handler that
 * prints the exception class, faulting address, exception link
 * register, and saved PSTATE, then halts. As subsequent chapters
 * land we replace individual slots:
 *   - slot 0x280 (IRQ) → timer + GIC handler
 *   - slot 0x400 (Sync from lower EL) → SVC handler
 */

.section .text.vectors

/* save_context — push every general-purpose register plus ELR_EL1
 * and SPSR_EL1 onto the kernel stack. The C handler receives a
 * pointer to this frame so it can dump or unwind state.
 *
 * Frame layout (low addr first):
 *   [sp + 0]    x0,  x1
 *   [sp + 16]   x2,  x3
 *   ...
 *   [sp + 224]  x28, x29
 *   [sp + 240]  x30, padding
 *   [sp + 256]  ELR_EL1, SPSR_EL1
 * Total: 272 bytes, padded to 16-byte alignment by caller. */
.macro save_context
    sub     sp, sp, #272
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
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #256]
.endm

/* restore_context — symmetric inverse used by the IRQ return path.
 * Pops the frame in reverse order and writes ELR_EL1 and SPSR_EL1
 * back so that the subsequent eret returns to exactly where the
 * interrupt was taken with the right PSTATE.
 *
 * IRQs are masked at the top of the macro and stay masked through
 * eret.  Why: callers that re-enabled IRQs while running in EL1
 * (e.g. sys_sleep_ms, or any syscall that went through schedule()
 * — schedule's cswitch_to resumes with SPSR.I = 0) leave us racing
 * with the timer between `msr elr_el1, x0` and the eret.  If a
 * tick lands in that window, irq_entry's save_context overwrites
 * ELR_EL1 with a kernel return address; on resume we then `msr
 * spsr_el1, x1` followed by `eret` and jump EL0 to that kernel PC,
 * which traps back as a non-SVC sync exception with EC=0 (privileged
 * insn at EL0) or EC=0x20 (instruction abort if the kernel pages
 * aren't EL0-readable in the active address space).  Masking
 * here makes the four MSRs + restores + eret atomic.  PSTATE.I is
 * restored from the SPSR loaded by eret, so the masking does NOT
 * leak into the resumed thread. */
.macro restore_context
    msr     daifset, #2
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
    add     sp, sp, #272
.endm

/* Each vector slot must be exactly 128 bytes long. We use the
 * .balign 0x80 directive to reach the next slot boundary; binutils
 * pads with NOP. The actual code in each slot is short — load a
 * vector ID into x0 and branch to a common entry point. The entry
 * point saves context, then calls the C panic handler. */
.balign 2048
.global vector_table
vector_table:
    /* Current EL with SP_EL0 — we never run on SP_EL0 at EL1, so
     * arrival here is a serious bug. IDs 0..3. */
    .balign 0x80
    mov     x0, #0
    b       panic_entry
    .balign 0x80
    mov     x0, #1
    b       panic_entry
    .balign 0x80
    mov     x0, #2
    b       panic_entry
    .balign 0x80
    mov     x0, #3
    b       panic_entry

    /* Current EL with SP_ELx — synchronous exceptions, IRQs, FIQs,
     * and SErrors taken from the kernel itself. IDs 4..7. */
    .balign 0x80
    mov     x0, #4
    b       panic_entry
    .balign 0x80
    b       irq_entry           /* slot 5: IRQ from EL1h */
    .balign 0x80
    mov     x0, #6
    b       panic_entry
    .balign 0x80
    mov     x0, #7
    b       panic_entry

    /* Lower EL using AArch64 — used once userspace lands. IDs 8..11. */
    .balign 0x80
    b       svc_entry           /* slot 8: Sync from EL0 — SVC + page faults */
    .balign 0x80
    b       irq_entry           /* slot 9: IRQ from EL0 — timer preemption */
    .balign 0x80
    mov     x0, #10
    b       panic_entry
    .balign 0x80
    mov     x0, #11
    b       panic_entry

    /* Lower EL using AArch32 — should never trigger. IDs 12..15. */
    .balign 0x80
    mov     x0, #12
    b       panic_entry
    .balign 0x80
    mov     x0, #13
    b       panic_entry
    .balign 0x80
    mov     x0, #14
    b       panic_entry
    .balign 0x80
    mov     x0, #15
    b       panic_entry

/* panic_entry — common landing pad for every vector slot.
 * On entry: x0 = vector ID (0..15), all other GPRs as the faulting
 * code left them. Save the entire context, then call the C handler
 * with arguments:
 *   x0 = vector ID
 *   x1 = pointer to saved frame on the stack */
.balign 8
panic_entry:
    /* Switch to a dedicated emergency stack before doing
     * anything else.  If the panic was caused by SP_EL1 being out
     * of mapped memory, save_context would itself fault and we'd
     * lose the original fault info.  Using a guaranteed-mapped
     * scratch stack lets us print the *first* fault instead of
     * the recursion. */
    msr     spsel, #1               /* make sure SPSel == 1 (paranoia) */
    mov     x18, sp                 /* preserve original SP_EL1 in x18 */
    adrp    x16, panic_emerg_stack_top
    add     x16, x16, :lo12:panic_emerg_stack_top
    mov     sp, x16

    /* Save the vector ID into a callee-saved register so the C
     * handler can recover it after save_context clobbers x0. */
    mov     x19, x0

    save_context

    mov     x0, x19         /* vector ID */
    mov     x1, sp          /* pointer to saved frame */
    bl      kernel_panic_from_vector

    /* kernel_panic_from_vector does not return. Belt-and-braces
     * halt loop in case it ever does. */
1:  wfe
    b       1b

/* Emergency stack used by panic_entry so a fault during context
 * save (e.g. SP_EL1 corrupted) does not recursively re-panic.
 * 4 KiB is overkill for one frame but we have plenty of RAM. */
.section .bss.panic_emerg
.balign 16
panic_emerg_stack:
    .skip 4096
.global panic_emerg_stack_top
panic_emerg_stack_top:

.section .text.vectors

/* vectors_init — install vector_table into VBAR_EL1.
 * Called from boot.s before mmu_enable so any fault encountered
 * during MMU bring-up itself goes through the handler. */
.global vectors_init
vectors_init:
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb
    ret

/* irq_entry — common IRQ landing pad.
 * Save the full register frame, dispatch to C, restore, eret.
 * Unlike panic_entry the IRQ path *must* return cleanly via eret
 * because the interrupted code is innocent. */
.balign 8
.global irq_entry
irq_entry:
    save_context
    mov     x0, sp
    bl      irq_dispatch
    restore_context
    eret

/* svc_entry — sync exception from a lower EL.  This is
 * overwhelmingly the SVC instruction (system call), but the same
 * slot also catches lower-EL data/instruction aborts.  We save the
 * full frame so the C dispatcher can read the syscall arguments
 * (x0..x5) and number (x8) directly from it, plus inspect ESR_EL1
 * to distinguish SVC from a fault.
 *
 * On entry the architecture has already swapped SP_EL0 → SP_EL1
 * (because we configured SPSel = 1 at boot), so save_context lands
 * the frame on the kernel stack of whichever thread is currently
 * scheduled.  After C returns we restore the frame — the syscall
 * return value lives in the saved x0 slot and is reloaded by
 * restore_context, so a syscall handler that wants to return a
 * value just writes frame->x[0] before returning. */
.balign 8
.global svc_entry
svc_entry:
    save_context
    mov     x0, sp
    bl      svc_dispatch
    restore_context
    eret

/*
 * kernel/core/thread.c — round-robin kernel-thread scheduler.
 *
 * Single CPU, single runqueue, no priorities, no locks.  The
 * runqueue is a NULL-terminated singly-linked list of READY
 * threads; `current` is the one actively running and is *not* on
 * the list.  yield()/schedule() pulls the head of the runqueue,
 * pushes the outgoing thread onto the tail (if still READY), and
 * calls cswitch_to to swap stacks.
 *
 * Stack layout for a freshly-created thread
 * -----------------------------------------
 * The stack grows down.  We pre-build a 272-byte exception-style
 * frame at the high end of the stack, configured so that when
 * cswitch_to restores it the CPU `eret`s into a small assembly
 * trampoline that:
 *
 *     1. moves x19 (= entry function) into a temp,
 *     2. moves x20 (= arg) into x0,
 *     3. branches with link to the entry function,
 *     4. on entry-function return, branches to thread_exit.
 *
 * Putting the trampoline in C (rather than asm) is fine because
 * by the time it runs the MMU is on, the kernel stack is in
 * Normal-Cacheable memory, and AAPCS prologues are safe.
 */

#include "thread.h"
#include "heap.h"
#include "serial.h"
#include "vfs.h"
#include "timer.h"
#include "wm.h"
#include "wsd_fb.h"
#include "win_fb.h"
#include "exception.h"
#include "pipe.h"
#include "pty.h"
#include "strace.h"
#include "../arch/address_space.h"
#include "../arch/atomic.h"
#include "../arch/cpu.h"
#include "../arch/ipi.h"
#include "../arch/spinlock.h"
#include <stddef.h>
#include <stdint.h>

#define THREAD_STACK_SIZE  (16 * 1024)   /* 16 KiB per thread */
#define FRAME_SIZE         288           /* 272 GPR/ELR/SPSR + 16 SP_EL0+pad */

extern void cswitch_to(uint64_t *save_sp, uint64_t load_sp);
extern void thread_trampoline(void);   /* defined in arch/context_switch.S */
extern void user_trampoline(void);     /* defined in arch/context_switch.S */
extern void user_clone_trampoline(void); /* chapter 91 — SYS_CLONE child   */

/* Frame offsets — must match save_context / cswitch_to. */
#define OFF_X30      240
#define OFF_ELR      256
#define OFF_SPSR     264
#define OFF_SP_EL0   272

/* ------------------------------------------------------------------
 * Bookkeeping
 *
 * Chapter 89 made `current`, the runqueue, and the EXITED-thread
 * stack-to-free slot per-CPU; they live in `struct cpu` now (see
 * arch/cpu.h).  The accessors below abstract that for the rest of
 * the file:
 *
 *   self()      — returns this CPU's currently-running thread.
 *   set_self(t) — installs `t` as this CPU's current thread.
 *
 * The per-CPU runqueue helpers (runq_push_local/runq_pop_local)
 * touch only this CPU's runq under this CPU's runq_lock.  The
 * remote variant (runq_push_remote) takes a target CPU id, locks
 * that CPU's runq, pushes, and IPIs the target so it can pick the
 * thread up.  No thread ever migrates between CPUs in chapter 89:
 * once a thread is on CPU N's runqueue, it stays on CPU N for
 * life.  This deliberately sidesteps the cross-CPU TLB shootdown
 * problem (no need to broadcast `tlbi vmalle1is` when an address
 * space is destroyed) at the cost of giving up dynamic load
 * balance — fine at our scale.
 *
 * `g_all_head` is the global "every live thread" list, used by
 *  thread_lookup, the thread_exit child-reap walk, the
 *  thread_wake_blocked walk, and the yield() sleeper-wake walk.
 *  All of those mutate or iterate the list, so they need to hold
 *  `g_all_lock`.
 * ------------------------------------------------------------------ */
static struct thread *g_all_head = NULL;        /* every live thread */
static spinlock_t     g_all_lock = SPINLOCK_INIT;

static inline struct thread *self(void)
{
    return cpu_current()->current;
}

static inline void set_self(struct thread *t)
{
    cpu_current()->current = t;
}

/* Chapter 89 compatibility shim.  Pre-SMP, `g_current` was a
 * plain `static struct thread *` global.  Keep that spelling for
 * the rest of this file via a macro that expands to the per-CPU
 * accessor.  Each read becomes one MRS TPIDR_EL1 + a load — the
 * single-CPU case pays one extra instruction per access, which is
 * irrelevant for code that is already calling kmalloc/runq_push/
 * cswitch_to alongside.  Works as both rvalue and lvalue:
 *   `if (!g_current)`                  --> `if (!cpu_current()->current)`
 *   `g_current = next;`                --> `cpu_current()->current = next;`
 *   `g_current->state = THREAD_RUNNING` --> `cpu_current()->current->state = ...` */
#define g_current (cpu_current()->current)

/* Chapter 87 — atomic counter for thread/process id allocation.
 * Plain `g_next_id++` was correct on uniprocessor because no
 * other CPU could observe the load without seeing the matching
 * store.  Once chapter 89's scheduler runs thread_create on
 * CPU 1 the same `++` becomes a lost-update race.  Switching
 * to atomic_add_return32 is free in the uncontended case (one
 * extra LDAXR/STLXR pair vs. a plain LDR/STR) and removes the
 * footgun ahead of time. */
static volatile uint32_t g_next_id = 0;
static volatile uint32_t g_thread_count = 0;

static int alloc_thread_id(void)
{
    /* atomic_add_return32 returns the NEW value; the old value
     * (= the id we want to hand out) is one less. */
    return (int)(atomic_add_return32(&g_next_id, 1) - 1);
}

static void all_push(struct thread *t)
{
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    t->all_next = g_all_head;
    g_all_head  = t;
    spin_unlock(&g_all_lock);
    irq_restore(f);
}

static void all_remove(struct thread *t)
{
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    struct thread **pp = &g_all_head;
    while (*pp) {
        if (*pp == t) { *pp = t->all_next; t->all_next = NULL; break; }
        pp = &(*pp)->all_next;
    }
    spin_unlock(&g_all_lock);
    irq_restore(f);
}

/* ------------------------------------------------------------------
 * Per-CPU runqueue.
 *
 * Local push/pop operate on this CPU's runq.  Remote push targets
 * a specific CPU and IPIs it.  All take the CPU's runq_lock with
 * IRQs masked because IRQ context (timer tick, IPI_RESCHED) calls
 * the local versions.
 * ------------------------------------------------------------------ */

static void runq_push_local(struct thread *t)
{
    struct cpu *c = cpu_current();
    uint64_t f = irq_save_disable();
    spin_lock(&c->runq_lock);
    t->next = NULL;
    if (c->runq_tail) {
        c->runq_tail->next = t;
    } else {
        c->runq_head = t;
    }
    c->runq_tail = t;
    spin_unlock(&c->runq_lock);
    irq_restore(f);
}

static struct thread *runq_pop_local(void)
{
    struct cpu *c = cpu_current();
    uint64_t f = irq_save_disable();
    spin_lock(&c->runq_lock);
    struct thread *t = c->runq_head;
    if (t) {
        c->runq_head = t->next;
        if (!c->runq_head)
            c->runq_tail = NULL;
        t->next = NULL;
    }
    spin_unlock(&c->runq_lock);
    irq_restore(f);
    return t;
}

/* Push onto a remote CPU's runqueue and kick it via IPI_RESCHED so
 * it wakes from WFI promptly.  Used by thread_create_on() and (in
 * the future) by anything else that wants to enqueue work on a
 * specific CPU.  Safe to call with IRQs on or off; takes the
 * target CPU's runq_lock with IRQs masked just like the local
 * variant. */
static void runq_push_remote(uint32_t cpu_id, struct thread *t)
{
    if (cpu_id >= SMP_MAX_CPUS) return;
    struct cpu *c = &g_cpus[cpu_id];
    uint64_t f = irq_save_disable();
    spin_lock(&c->runq_lock);
    t->next = NULL;
    if (c->runq_tail) {
        c->runq_tail->next = t;
    } else {
        c->runq_head = t;
    }
    c->runq_tail = t;
    spin_unlock(&c->runq_lock);
    irq_restore(f);

    /* If pushing to ourselves, no IPI needed (the timer tick or
     * the next yield() will pick the thread up). */
    if (cpu_current_id() != cpu_id)
        ipi_send(cpu_id, IPI_RESCHED);
}

/* Compatibility shim — most legacy call sites push to the
 * CURRENT CPU's runqueue.  Keep the short name for them. */
static inline void runq_push(struct thread *t) { runq_push_local(t); }
static inline struct thread *runq_pop(void)    { return runq_pop_local(); }

/* Chapter 92 — push `t` onto its HOME CPU's runqueue.  If
 * t->home_cpu is the current CPU this is a plain runq_push_local;
 * otherwise it routes via runq_push_remote and an IPI_RESCHED so
 * the target wakes from WFI promptly.  Used by the wake-side code
 * (thread_wake_blocked, the sleeper-walk in yield) so a wake
 * fired on CPU 1 can correctly enqueue a CPU-0-pinned thread on
 * CPU 0's runq instead of stealing it onto CPU 1. */
static void runq_push_to(struct thread *t)
{
    if (t->home_cpu == cpu_current_id())
        runq_push_local(t);
    else
        runq_push_remote(t->home_cpu, t);
}

/* Bounded copy into the thread's inline name buffer.  At most
 * NAME_MAX-1 source bytes; always NUL-terminated.  Truncates
 * silently. */
static void thread_set_name(struct thread *t, const char *src)
{
    if (!src) src = "?";
    size_t i = 0;
    while (i + 1 < THREAD_NAME_MAX && src[i]) {
        t->name[i] = src[i];
        i++;
    }
    t->name[i] = '\0';
}

/* Free the stack of an EXITED thread once we know the next context
 * switch is no longer using it.  We DELAY freeing the struct
 * itself: the parent's wait() needs to read exit_code first.  The
 * struct is freed inside thread_wait when the child is reaped.
 *
 * Chapter 89: this slot is per-CPU (struct cpu::stack_to_free).
 * Two CPUs can each have a thread call thread_exit() at the same
 * moment; a single global slot would lose one of the two.  Each
 * CPU drains its own slot at the top of yield(). */
static void drain_stack_to_free(void)
{
    struct cpu *c = cpu_current();
    if (!c->stack_to_free) return;
    if (c->stack_to_free->stack_base) {
        kfree(c->stack_to_free->stack_base);
        c->stack_to_free->stack_base = NULL;
    }
    c->stack_to_free = NULL;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */
struct thread *thread_current(void)
{
    return self();
}

size_t thread_count(void)
{
    return (size_t)atomic_load32(&g_thread_count);
}

void thread_init(void)
{
    struct thread *boot = (struct thread *)kmalloc(sizeof(*boot));
    if (!boot) {
        serial_puts("[thread] FATAL — kmalloc failed for boot thread\n");
        for (;;) __asm__ volatile("wfe");
    }
    boot->sp         = 0;          /* filled in on first cswitch_to    */
    boot->stack_base = NULL;       /* boot stack lives in linker .stack*/
    boot->stack_size = 0;
    boot->id         = alloc_thread_id();
    boot->parent_id  = -1;
    boot->exit_code  = 0;
    boot->state      = THREAD_RUNNING;
    thread_set_name(boot, "boot");
    boot->next       = NULL;
    boot->all_next   = NULL;
    boot->wake_at_ms = 0;
    boot->blocked_on = NULL;
    boot->tty_raw    = 0;
    boot->sig_pending = 0;
    for (int s = 0; s < 32; s++) boot->sig_handlers[s] = 0;
    boot->sig_restorer = 0;
    boot->strace     = NULL;
    boot->args[0]    = '\0';
    boot->as         = NULL;
    boot->cwd[0]     = '/';
    boot->cwd[1]     = '\0';
    /* Empty env: two NUL bytes mark an empty list. */
    boot->env[0]     = '\0';
    boot->env[1]     = '\0';
    /* Chapter 92 — boot thread runs on CPU 0 (we're the boot CPU). */
    boot->home_cpu   = 0;
    /* Chapter 93 — fdt is allocated by vfs_init_fdtable; NULL
     * in advance so the function knows we want a fresh table
     * (a non-NULL value would mean "caller already attached one
     * via CLONE_FILES" and the alloc would be skipped). */
    boot->fdt        = NULL;
    vfs_init_fdtable(boot);
    all_push(boot);

    set_self(boot);
    /* CPU 0's `idle` stays NULL; the boot thread itself fills the
     * "always-runnable" slot.  When CPU 0's runqueue is empty,
     * yield() simply keeps running boot/init.  CPU N (N >= 1)
     * creates its own idle thread in secondary_main. */
    atomic_store32(&g_thread_count, 1);
}

/* Forward declaration — defined below. */
struct thread *thread_create(thread_entry_fn entry,
                             void *arg,
                             const char *name)
{
    struct thread *t = (struct thread *)kmalloc(sizeof(*t));
    if (!t) return NULL;

    uint8_t *stack = (uint8_t *)kmalloc(THREAD_STACK_SIZE);
    if (!stack) { kfree(t); return NULL; }

    t->stack_base = stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->id         = alloc_thread_id();
    t->parent_id  = g_current ? g_current->id : -1;
    t->exit_code  = 0;
    t->state      = THREAD_READY;
    thread_set_name(t, name);
    t->next       = NULL;
    t->all_next   = NULL;
    t->args[0]    = '\0';
    t->as         = NULL;
    t->wake_at_ms = 0;
    t->blocked_on = NULL;
    t->tty_raw    = 0;     /* never inherit raw mode — children always start cooked */
    t->sig_pending = 0;    /* never inherit pending signals — child starts clean */
    for (int s = 0; s < 32; s++) t->sig_handlers[s] = 0;
    t->sig_restorer = 0;
    t->strace     = NULL;  /* opt-in via sys_trace_me; never inherited */
    /* Chapter 92 — kernel thread inherits the creating CPU as
     * its home.  thread_create is the "spawn here" path; the
     * remote variant is thread_create_on. */
    t->home_cpu   = cpu_current_id();
    /* Inherit cwd from the spawning thread.  Falls back to "/"
     * when called before thread_init (shouldn't happen). */
    if (g_current) {
        size_t i = 0;
        while (g_current->cwd[i] && i < THREAD_CWD_MAX - 1) {
            t->cwd[i] = g_current->cwd[i]; i++;
        }
        t->cwd[i] = '\0';
    } else {
        t->cwd[0] = '/'; t->cwd[1] = '\0';
    }
    /* Inherit env block.  Copy bytes through the second NUL
     * (end-of-list marker) so the child sees the parent's full
     * variable set. */
    if (g_current) {
        size_t i = 0;
        int    seen_term = 0;
        while (i < THREAD_ENV_MAX) {
            t->env[i] = g_current->env[i];
            if (g_current->env[i] == '\0') {
                if (seen_term) { i++; break; }
                seen_term = 1;
            } else {
                seen_term = 0;
            }
            i++;
        }
        /* Belt-and-braces: ensure final two bytes are zero. */
        t->env[THREAD_ENV_MAX - 2] = '\0';
        t->env[THREAD_ENV_MAX - 1] = '\0';
    } else {
        t->env[0] = '\0'; t->env[1] = '\0';
    }
    /* Chapter 93 — fresh refcounted fd_table per thread_create. */
    t->fdt = NULL;
    vfs_init_fdtable(t);
    all_push(t);

    /* Build the initial 272-byte frame at the top of the stack. */
    uint8_t *frame_top = stack + THREAD_STACK_SIZE;
    /* Round down to 16-byte alignment then reserve the frame. */
    uintptr_t top_aligned = ((uintptr_t)frame_top) & ~((uintptr_t)0xF);
    uint8_t *frame = (uint8_t *)(top_aligned - FRAME_SIZE);

    /* Zero the frame, then plant the bits we care about. */
    for (size_t i = 0; i < FRAME_SIZE; i++)
        frame[i] = 0;

    uint64_t *gpr = (uint64_t *)frame;
    gpr[19] = (uint64_t)(uintptr_t)entry;        /* x19 = entry fn */
    gpr[20] = (uint64_t)(uintptr_t)arg;          /* x20 = arg      */
    /* x30 (offset 240) = thread_trampoline so the eret-then-ret
     * dance lands on it.  Note: x30 lives at byte 240 (single
     * register, not a pair). */
    *(uint64_t *)(frame + OFF_X30) = (uint64_t)(uintptr_t)thread_trampoline;
    *(uint64_t *)(frame + OFF_ELR) = (uint64_t)(uintptr_t)thread_trampoline;
    /* SPSR_EL1 = EL1h with IRQs unmasked, FIQ/SError/Debug masked.
     * M[3:0]=0101, F=1, I=0, A=1, D=1 → 0x345. */
    *(uint64_t *)(frame + OFF_SPSR) = 0x345ULL;

    t->sp = (uint64_t)(uintptr_t)frame;

    runq_push(t);
    atomic_add_return32(&g_thread_count, 1);
    return t;
}

/*
 * thread_create_on — like thread_create but enqueues onto a chosen
 * CPU's runqueue.  Used by the chapter 89 SMP smoke test and
 * eventually by anything else that wants to balance kernel work
 * across CPUs.
 *
 * Build the thread struct + initial frame the same way as
 * thread_create, then push to the target CPU's runq under that
 * CPU's runq_lock and (if the target is a different CPU) IPI it.
 *
 * No thread migration: once placed, the thread stays on `cpu_id`.
 */
struct thread *thread_create_on(uint32_t cpu_id,
                                thread_entry_fn entry,
                                void *arg,
                                const char *name)
{
    if (cpu_id >= SMP_MAX_CPUS) return NULL;

    struct thread *t = (struct thread *)kmalloc(sizeof(*t));
    if (!t) return NULL;

    uint8_t *stack = (uint8_t *)kmalloc(THREAD_STACK_SIZE);
    if (!stack) { kfree(t); return NULL; }

    t->stack_base = stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->id         = alloc_thread_id();
    t->parent_id  = g_current ? g_current->id : -1;
    t->exit_code  = 0;
    t->state      = THREAD_READY;
    thread_set_name(t, name);
    t->next       = NULL;
    t->all_next   = NULL;
    t->args[0]    = '\0';
    t->cwd[0]     = '/'; t->cwd[1] = '\0';
    t->env[0]     = '\0'; t->env[1] = '\0';
    t->as         = NULL;
    t->wake_at_ms = 0;
    t->blocked_on = NULL;
    t->tty_raw    = 0;
    t->sig_pending = 0;
    for (int s = 0; s < 32; s++) t->sig_handlers[s] = 0;
    t->sig_restorer = 0;
    t->strace     = NULL;
    /* Chapter 92 — explicit placement.  home_cpu is the
     * caller-supplied target. */
    t->home_cpu   = cpu_id;
    /* Chapter 93 — fresh refcounted fd_table per thread_create_on. */
    t->fdt = NULL;
    vfs_init_fdtable(t);
    all_push(t);

    uint8_t *frame_top    = stack + THREAD_STACK_SIZE;
    uintptr_t top_aligned = ((uintptr_t)frame_top) & ~((uintptr_t)0xF);
    uint8_t *frame        = (uint8_t *)(top_aligned - FRAME_SIZE);
    for (size_t i = 0; i < FRAME_SIZE; i++) frame[i] = 0;

    uint64_t *gpr = (uint64_t *)frame;
    gpr[19] = (uint64_t)(uintptr_t)entry;
    gpr[20] = (uint64_t)(uintptr_t)arg;
    *(uint64_t *)(frame + OFF_X30)  = (uint64_t)(uintptr_t)thread_trampoline;
    *(uint64_t *)(frame + OFF_ELR)  = (uint64_t)(uintptr_t)thread_trampoline;
    *(uint64_t *)(frame + OFF_SPSR) = 0x345ULL;

    t->sp = (uint64_t)(uintptr_t)frame;

    runq_push_remote(cpu_id, t);
    atomic_add_return32(&g_thread_count, 1);
    return t;
}

/*
 * thread_secondary_init_idle — see header.  Called from
 * secondary_main on each CPU >= 1 BEFORE that CPU enters its
 * scheduling loop.  Idle is off-runq and off-all-list; yield()
 * switches into it when its CPU's runq is empty.
 *
 * Field setup mirrors the boot thread (see thread_init):
 *   - state = THREAD_RUNNING (we are about to "be" this thread)
 *   - stack_base/stack_size = 0 (we did NOT kmalloc this stack;
 *     it's the secondary's boot stack from .secondary_stacks).
 *     thread_exit on idle would try to kfree NULL, which is a
 *     no-op, but idle never exits anyway.
 *   - parent_id = -1 (orphan; nothing wait()s on idle)
 *
 * On the FIRST cswitch_to(&idle->sp, real->sp), cswitch's
 * prologue stashes idle's current SP into idle->sp.  No need to
 * pre-build a frame: idle hasn't suspended yet.
 */
int thread_secondary_init_idle(const char *name)
{
    struct thread *idle = (struct thread *)kmalloc(sizeof(*idle));
    if (!idle) return -1;

    idle->stack_base = NULL;
    idle->stack_size = 0;
    idle->id         = alloc_thread_id();
    idle->parent_id  = -1;
    idle->exit_code  = 0;
    idle->state      = THREAD_RUNNING;
    thread_set_name(idle, name);
    idle->next       = NULL;
    idle->all_next   = NULL;
    idle->args[0]    = '\0';
    idle->cwd[0]     = '/'; idle->cwd[1] = '\0';
    idle->env[0]     = '\0'; idle->env[1] = '\0';
    idle->as         = NULL;
    idle->wake_at_ms = 0;
    idle->blocked_on = NULL;
    idle->tty_raw    = 0;
    idle->sig_pending = 0;
    for (int s = 0; s < 32; s++) idle->sig_handlers[s] = 0;
    idle->sig_restorer = 0;
    idle->strace     = NULL;
    /* Chapter 92 — idle's home is the CPU it lives on. */
    idle->home_cpu   = cpu_current_id();
    idle->sp         = 0;   /* filled in by first cswitch_to */
    /* Chapter 93 — idle threads carry a private (unused) fd table
     * just like every other thread; cheaper than special-casing
     * the close path on idle. */
    idle->fdt        = NULL;
    vfs_init_fdtable(idle);

    cpu_current()->idle    = idle;
    cpu_current()->current = idle;
    return 0;
}

/*
 * user_thread_create — synthesise a thread that erets to EL0.
 *
 * Mechanically nearly identical to thread_create, with two
 * differences in the initial frame:
 *
 *   1. ELR slot points at user_trampoline (not thread_trampoline).
 *      cswitch_to's eret therefore lands inside user_trampoline at
 *      EL1 with SPSR=0x345 (EL1h).
 *
 *   2. x19 carries the user-space entry virtual address and x20
 *      carries the desired SP_EL0.  user_trampoline reads them out,
 *      writes ELR_EL1 and SP_EL0 accordingly, sets SPSR_EL1 to EL0t
 *      with IRQs unmasked, zeroes the rest of the GPRs, and erets.
 *
 * The kernel-side stack the thread carries is the one used by
 * exception entries (SVC, timer IRQ) while the thread is at EL0.
 */
struct thread *user_thread_create(uint64_t user_entry_va,
                                  uint64_t user_sp_top,
                                  const char *name,
                                  struct address_space *as)
{
    struct thread *t = (struct thread *)kmalloc(sizeof(*t));
    if (!t) return NULL;

    uint8_t *stack = (uint8_t *)kmalloc(THREAD_STACK_SIZE);
    if (!stack) { kfree(t); return NULL; }

    t->stack_base = stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->id         = alloc_thread_id();
    t->parent_id  = g_current ? g_current->id : -1;
    t->exit_code  = 0;
    t->state      = THREAD_READY;
    thread_set_name(t, name ? name : "user");
    t->next       = NULL;
    t->all_next   = NULL;
    t->args[0]    = '\0';
    t->as         = as;
    t->wake_at_ms = 0;
    t->blocked_on = NULL;
    t->tty_raw    = 0;     /* user thread always starts cooked */
    t->sig_pending = 0;    /* user thread starts with no pending signals */
    for (int s = 0; s < 32; s++) t->sig_handlers[s] = 0;
    t->sig_restorer = 0;
    t->strace     = NULL;
    /* Chapter 92 — user thread runs on the creating CPU.  All
     * existing user_thread_create callers (sys_spawn family,
     * sys_exec) run on CPU 0, so this preserves the chapter 91
     * floor automatically. */
    t->home_cpu   = cpu_current_id();
    /* Inherit cwd from the spawning thread (the kernel-side
     * caller of user_thread_create — usually sys_spawn). */
    if (g_current) {
        size_t i = 0;
        while (g_current->cwd[i] && i < THREAD_CWD_MAX - 1) {
            t->cwd[i] = g_current->cwd[i]; i++;
        }
        t->cwd[i] = '\0';
    } else {
        t->cwd[0] = '/'; t->cwd[1] = '\0';
    }
    /* Inherit env block (same logic as thread_create). */
    if (g_current) {
        size_t i = 0;
        int    seen_term = 0;
        while (i < THREAD_ENV_MAX) {
            t->env[i] = g_current->env[i];
            if (g_current->env[i] == '\0') {
                if (seen_term) { i++; break; }
                seen_term = 1;
            } else {
                seen_term = 0;
            }
            i++;
        }
        t->env[THREAD_ENV_MAX - 2] = '\0';
        t->env[THREAD_ENV_MAX - 1] = '\0';
    } else {
        t->env[0] = '\0'; t->env[1] = '\0';
    }
    /* Chapter 93 — fresh refcounted fd_table per user_thread_create. */
    t->fdt = NULL;
    vfs_init_fdtable(t);
    all_push(t);

    uint8_t *frame_top = stack + THREAD_STACK_SIZE;
    uintptr_t top_aligned = ((uintptr_t)frame_top) & ~((uintptr_t)0xF);
    uint8_t *frame = (uint8_t *)(top_aligned - FRAME_SIZE);

    for (size_t i = 0; i < FRAME_SIZE; i++)
        frame[i] = 0;

    uint64_t *gpr = (uint64_t *)frame;
    gpr[19] = user_entry_va;
    gpr[20] = user_sp_top;
    *(uint64_t *)(frame + OFF_X30) = (uint64_t)(uintptr_t)user_trampoline;
    *(uint64_t *)(frame + OFF_ELR) = (uint64_t)(uintptr_t)user_trampoline;
    *(uint64_t *)(frame + OFF_SPSR) = 0x345ULL;   /* EL1h, IRQs on */

    t->sp = (uint64_t)(uintptr_t)frame;

    runq_push(t);
    atomic_add_return32(&g_thread_count, 1);
    return t;
}

/*
 * user_thread_create_shared — chapter 91.
 *
 * Like user_thread_create, with three differences:
 *
 *   1. The new thread shares an existing AS instead of taking
 *      ownership of a freshly-minted one.  We bump the AS's
 *      refcount via address_space_share so subsequent
 *      address_space_destroy calls (one per thread at exit time)
 *      do the right thing — only the LAST exiter actually tears
 *      the page tables down.
 *
 *   2. The new thread's first user-mode register file carries
 *      `arg` in x0 and the kernel sets TPIDR_EL0 from `tls`.
 *      That's how userspace pthread_create-style wrappers pass
 *      the closure pointer to the worker function: the kernel
 *      jumps to entry(arg).  We use a dedicated trampoline
 *      (user_clone_trampoline) instead of teaching
 *      user_trampoline a conditional, so the existing
 *      one-program-per-thread launch path stays branch-free.
 *
 *   3. The thread is born with NO file descriptors copied from
 *      the parent (FDs are explicitly per-thread in this floor).
 *      Pre-chapter-91 every fork/spawn/exec inherited fds; in
 *      chapter 91 the threading test does not need shared fds
 *      and per-thread fds are simpler than reference-counted
 *      fd tables.  Document the limitation in the chapter.
 *
 * Caller must keep its own AS reference until this returns
 * non-NULL — failure paths free nothing AS-related, and the
 * refcount bump only happens on the success path.
 */
struct thread *user_thread_create_shared(uint64_t user_entry_va,
                                         uint64_t user_sp_top,
                                         const char *name,
                                         struct address_space *as,
                                         uint64_t arg,
                                         uint64_t tls)
{
    /* Chapter 91 entry point — preserves the original "spawn on
     * the creating CPU" semantics by passing cpu_id = -1 to the
     * chapter-92 _on() variant. */
    return user_thread_create_shared_on(user_entry_va, user_sp_top,
                                        name, as, arg, tls, -1);
}

struct thread *user_thread_create_shared_on(uint64_t user_entry_va,
                                            uint64_t user_sp_top,
                                            const char *name,
                                            struct address_space *as,
                                            uint64_t arg,
                                            uint64_t tls,
                                            int cpu_id)
{
    /* Chapter 92 entry point — preserves the original "fresh
     * fd_table per thread" semantics by passing share_fdt = 0. */
    return user_thread_create_shared_files_on(user_entry_va, user_sp_top,
                                              name, as, arg, tls,
                                              cpu_id, /*share_fdt=*/0);
}

/*
 * user_thread_create_shared_files_on — chapter 93.
 *
 * Same as user_thread_create_shared_on, with one extra knob:
 * `share_fdt`.  When non-zero AND the caller (g_current) has an
 * existing fd table, the new thread *adopts* the parent's
 * fd_table by reference (fd_table_share bumps the refcount)
 * instead of allocating a fresh one.  When zero (the default
 * preserved by the legacy _on entry point) the new thread gets
 * its own private table just like chapter 92 did.
 *
 * This is the kernel-side primitive sitting under SYS_CLONE3
 * with CLONE_FILES — see sys_clone3.
 *
 * `share_fdt` requires g_current to be a thread that already
 * has a non-NULL fdt; the boot path before vfs_init_fdtable
 * runs would otherwise null-deref.  Real callers (sys_clone3)
 * are always invoked from a fully-formed user thread, so this
 * is just a defensive note.
 */
struct thread *user_thread_create_shared_files_on(uint64_t user_entry_va,
                                                   uint64_t user_sp_top,
                                                   const char *name,
                                                   struct address_space *as,
                                                   uint64_t arg,
                                                   uint64_t tls,
                                                   int cpu_id,
                                                   int share_fdt)
{
    if (!as) return NULL;
    /* Resolve cpu_id == -1 to "current CPU".  Out-of-range
     * positive values are an immediate failure — we don't want
     * a typo'd cpu_id silently wrapping. */
    uint32_t target_cpu;
    if (cpu_id < 0) {
        target_cpu = cpu_current_id();
    } else if ((uint32_t)cpu_id >= SMP_MAX_CPUS) {
        return NULL;
    } else {
        target_cpu = (uint32_t)cpu_id;
    }

    struct thread *t = (struct thread *)kmalloc(sizeof(*t));
    if (!t) return NULL;

    uint8_t *stack = (uint8_t *)kmalloc(THREAD_STACK_SIZE);
    if (!stack) { kfree(t); return NULL; }

    t->stack_base = stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->id         = alloc_thread_id();
    t->parent_id  = g_current ? g_current->id : -1;
    t->exit_code  = 0;
    t->state      = THREAD_READY;
    thread_set_name(t, name ? name : "thread");
    t->next       = NULL;
    t->all_next   = NULL;
    t->args[0]    = '\0';
    /* Bump the AS refcount before we publish the thread so a
     * concurrent exit cannot tear the AS down between us
     * adopting it and us adding to all/runq. */
    address_space_share(as);
    t->as         = as;
    t->wake_at_ms = 0;
    t->blocked_on = NULL;
    t->tty_raw    = 0;
    t->sig_pending = 0;
    for (int s = 0; s < 32; s++) t->sig_handlers[s] = 0;
    t->sig_restorer = 0;
    t->strace     = NULL;
    /* Chapter 92 — pin the new thread to the resolved CPU. */
    t->home_cpu   = target_cpu;
    /* Inherit cwd / env from the creating thread (same logic as
     * user_thread_create — the new thread is logically part of
     * the same process). */
    if (g_current) {
        size_t i = 0;
        while (g_current->cwd[i] && i < THREAD_CWD_MAX - 1) {
            t->cwd[i] = g_current->cwd[i]; i++;
        }
        t->cwd[i] = '\0';
    } else {
        t->cwd[0] = '/'; t->cwd[1] = '\0';
    }
    if (g_current) {
        size_t i = 0;
        int    seen_term = 0;
        while (i < THREAD_ENV_MAX) {
            t->env[i] = g_current->env[i];
            if (g_current->env[i] == '\0') {
                if (seen_term) { i++; break; }
                seen_term = 1;
            } else {
                seen_term = 0;
            }
            i++;
        }
        t->env[THREAD_ENV_MAX - 2] = '\0';
        t->env[THREAD_ENV_MAX - 1] = '\0';
    } else {
        t->env[0] = '\0'; t->env[1] = '\0';
    }
    /* Chapter 93 — fd table policy depends on share_fdt:
     *   share_fdt == 0:  allocate a fresh refcounted fd_table
     *                    just like chapter 91/92 (the default
     *                    seen by every existing caller of the
     *                    plain `_on` entry point).
     *   share_fdt != 0:  adopt the parent's fd_table by
     *                    reference (CLONE_FILES semantics).
     *                    All subsequent open/close from either
     *                    thread mutates the same table; both
     *                    threads see the same fds at the same
     *                    indices, and the table is freed only
     *                    when the LAST referencing thread exits. */
    t->fdt = NULL;
    if (share_fdt && g_current && g_current->fdt) {
        t->fdt = g_current->fdt;
        fd_table_share(t->fdt);
    } else {
        vfs_init_fdtable(t);
    }
    all_push(t);

    /* Build the launch frame.  Layout matches user_trampoline
     * (cswitch_to-style frame), but with x21=arg and x22=tls
     * carrying the extra clone payload.
     * user_clone_trampoline reads x19/x20/x21/x22 to set up
     * ELR_EL1 / SP_EL0 / x0 / TPIDR_EL0 respectively. */
    uint8_t *frame_top   = stack + THREAD_STACK_SIZE;
    uintptr_t top_aligned = ((uintptr_t)frame_top) & ~((uintptr_t)0xF);
    uint8_t *frame        = (uint8_t *)(top_aligned - FRAME_SIZE);

    for (size_t i = 0; i < FRAME_SIZE; i++) frame[i] = 0;

    uint64_t *gpr = (uint64_t *)frame;
    gpr[19] = user_entry_va;
    gpr[20] = user_sp_top;
    gpr[21] = arg;
    gpr[22] = tls;
    *(uint64_t *)(frame + OFF_X30)  = (uint64_t)(uintptr_t)user_clone_trampoline;
    *(uint64_t *)(frame + OFF_ELR)  = (uint64_t)(uintptr_t)user_clone_trampoline;
    *(uint64_t *)(frame + OFF_SPSR) = 0x345ULL;   /* EL1h, IRQs on */

    t->sp = (uint64_t)(uintptr_t)frame;

    /* Chapter 92 — route to home_cpu's runqueue.  When
     * target_cpu == cpu_current_id() this is runq_push_local;
     * otherwise it goes to the remote CPU's runq + IPI_RESCHED. */
    runq_push_to(t);
    atomic_add_return32(&g_thread_count, 1);
    return t;
}

/* Public bounded name setter — exposes the static helper above
 * so sys_exec can rename the thread after a successful AS swap. */
void thread_rename(struct thread *t, const char *new_name)
{
    if (!t) return;
    thread_set_name(t, new_name);
}

/* ----------------------------------------------------------------
 * Chapter 99 — /proc snapshot helpers.
 *
 * Each helper grabs g_all_lock (with IRQs masked, like every
 * other writer of this list), walks g_all_head, and copies the
 * fields out into caller-provided storage.  The locked section
 * is memcpy-only; no formatting happens here so the lock window
 * stays bounded regardless of how many threads exist.
 *
 * Returns a count of live threads observed during the walk —
 * even if `max` was smaller than that, so callers can detect
 * truncation and warn.
 * ---------------------------------------------------------------- */

static void thread_snap_fill(struct thread_snap *out, struct thread *t)
{
    out->id         = t->id;
    out->parent_id  = t->parent_id;
    out->state      = (int)t->state;
    out->home_cpu   = t->home_cpu;
    out->tty_raw    = t->tty_raw;
    out->exit_code  = t->exit_code;
    out->wake_at_ms = t->wake_at_ms;
    /* Bounded copies; the source strings are kept NUL-terminated
     * by their writers so we just walk until NUL or the slot
     * ends. */
    for (int i = 0; i < THREAD_NAME_MAX; i++) {
        out->name[i] = t->name[i];
        if (!t->name[i]) break;
    }
    out->name[THREAD_NAME_MAX - 1] = '\0';
    for (int i = 0; i < THREAD_ARGS_MAX; i++) {
        out->args[i] = t->args[i];
        if (!t->args[i]) break;
    }
    out->args[THREAD_ARGS_MAX - 1] = '\0';
    for (int i = 0; i < THREAD_CWD_MAX; i++) {
        out->cwd[i] = t->cwd[i];
        if (!t->cwd[i]) break;
    }
    out->cwd[THREAD_CWD_MAX - 1] = '\0';
}

int thread_snapshot(struct thread_snap *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    for (struct thread *t = g_all_head; t && n < max; t = t->all_next) {
        thread_snap_fill(&out[n], t);
        n++;
    }
    spin_unlock(&g_all_lock);
    irq_restore(f);
    return n;
}

int thread_snapshot_pid(int pid, struct thread_snap *out)
{
    if (!out) return 0;
    int hit = 0;
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    for (struct thread *t = g_all_head; t; t = t->all_next) {
        if (t->id == pid) {
            thread_snap_fill(out, t);
            hit = 1;
            break;
        }
    }
    spin_unlock(&g_all_lock);
    irq_restore(f);
    return hit;
}

/* Chapter 100 — render /proc/<pid>/trace.  Holds g_all_lock for
 * the duration of the render so the target thread cannot exit
 * and be freed underneath us.  The rendered text is bounded
 * (PROCFS_MAX_FILE = 8 KiB) and the formatter takes no other
 * locks, so the window is bounded and small.  Returns -1 if no
 * such pid exists. */
long thread_strace_render_pid(int pid, char *out, size_t cap)
{
    long n = -1;
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    for (struct thread *t = g_all_head; t; t = t->all_next) {
        if (t->id == pid) {
            n = strace_render_and_drain(t, out, cap);
            break;
        }
    }
    spin_unlock(&g_all_lock);
    irq_restore(f);
    return n;
}

int thread_runqueue_len(uint32_t cpu_id)
{
    int n = 0;
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    for (struct thread *t = g_all_head; t; t = t->all_next) {
        if (t->home_cpu != cpu_id) continue;
        /* A thread is "queued for this CPU" if it's READY or
         * actively RUNNING here.  SLEEPING / WAITING / BLOCKED
         * are off-rq states.  EXITED isn't counted; the parent
         * hasn't reaped it yet, but the scheduler doesn't pick
         * it. */
        if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
            n++;
    }
    spin_unlock(&g_all_lock);
    irq_restore(f);
    return n;
}

int thread_live_count(void)
{
    int n = 0;
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    for (struct thread *t = g_all_head; t; t = t->all_next) n++;
    spin_unlock(&g_all_lock);
    irq_restore(f);
    return n;
}

/* See thread.h for the contract.
 *
 * Copies parent's in-use fd table entries over child's,
 * bumping refcounts symmetrically and tearing down any
 * pre-existing FD_PIPE_* / FD_PTY_* slot we are about to
 * overwrite.  Used by both fork (full POSIX inheritance) and
 * the various sys_spawn flavours (POSIX-equivalent of fork+exec
 * with explicit overrides).
 *
 * Sockets are NOT inherited — the single-owner refcount model
 * for tcp_cid (M64) does not yet handle multi-thread close
 * races.  Apps that need this can re-connect in the child.
 */
void thread_inherit_fds(struct thread *child, struct thread *parent)
{
    for (int fd = 0; fd < FD_TABLE_SIZE; fd++) {
        const struct fd_entry *src = &parent->fdt->fds[fd];
        if (!src->in_use) continue;
        if (src->kind == FD_SOCKET) continue;
        if (src->kind == FD_SOCKET_LISTEN) continue;  /* chapter 104 */
        /* Chapter 107 — /srv listeners are single-owner like
         * TCP listeners; the kernel-side registry keys on pid
         * via srv_listen.owner_pid and an inherited listener
         * fd would let the child accept on the parent's name
         * (almost certainly a bug).  /srv connected fds are
         * also opt-out for v1: refcount bookkeeping on the
         * conn endpoints would need the same care as TCP
         * cids, deferred to a later chapter. */
        if (src->kind == FD_SRV_LISTEN) continue;
        if (src->kind == FD_SRV_CONN)   continue;
        /* Chapter 99 — FD_PROCFS slots hold a kmalloc'd snapshot
         * buffer that the parent's vfs_close will free.  Copying
         * the pointer naively would double-free at the child's
         * close.  Just don't inherit — /proc files are always
         * cheap to re-open in the child if it really wants them. */
        if (src->kind == FD_PROCFS) continue;

        /* Drop whatever placeholder vfs_init_fdtable / a previous
         * inherit/install put in this slot.  FD_CONSOLE has no
         * refs; pipe/pty halves need a proper unref so we don't
         * leak the underlying object. */
        struct fd_entry *dst = &child->fdt->fds[fd];
        if (dst->in_use) {
            if (dst->kind == FD_PIPE_R && dst->pipe)
                pipe_unref(dst->pipe, PIPE_REF_R);
            else if (dst->kind == FD_PIPE_W && dst->pipe)
                pipe_unref(dst->pipe, PIPE_REF_W);
            else if (dst->kind == FD_PTY_MASTER && dst->pty)
                pty_close_master(dst->pty);
            else if (dst->kind == FD_PTY_SLAVE && dst->pty)
                pty_close_slave(dst->pty);
        }

        *dst = *src;
        dst->in_use = 1;

        if (dst->kind == FD_PIPE_R && dst->pipe)
            dst->pipe->r_refs++;
        else if (dst->kind == FD_PIPE_W && dst->pipe)
            dst->pipe->w_refs++;
        else if (dst->kind == FD_PTY_MASTER && dst->pty) {
            dst->pty->refs++;
            dst->pty->s2m->r_refs++;
            dst->pty->m2s->w_refs++;
        }
        else if (dst->kind == FD_PTY_SLAVE && dst->pty) {
            dst->pty->refs++;
            dst->pty->m2s->r_refs++;
            dst->pty->s2m->w_refs++;
        }
    }
}

/*
 * thread_fork_user — see thread.h for the contract.
 *
 * Synthesises a 288-byte cswitch_to-style frame at the top of
 * the child's kernel stack.  Layout matches cswitch_to's restore
 * sequence in arch/context_switch.s:
 *
 *   [sp +   0..15]  x0,  x1
 *   [sp +  16..31]  x2,  x3
 *      ...
 *   [sp + 224..239] x28, x29
 *   [sp + 240]      x30
 *   [sp + 248]      pad (zero)
 *   [sp + 256]      ELR_EL1
 *   [sp + 264]      SPSR_EL1
 *   [sp + 272]      SP_EL0
 *   [sp + 280]      pad (zero)
 *
 * Critically: x0 is forced to 0 (so the child's user code sees
 * fork() return 0), and SP_EL0 is the parent's SP_EL0 at the
 * moment of the SVC.  Everything else carries over from the
 * parent's saved trap frame so the child resumes at exactly the
 * instruction after the SVC #0, with identical user-side state.
 */
struct thread *thread_fork_user(struct thread *parent,
                                struct address_space *child_as,
                                const struct exception_frame *parent_frame,
                                uint64_t parent_sp_el0)
{
    if (!parent || !child_as || !parent_frame) return NULL;

    struct thread *t = (struct thread *)kmalloc(sizeof(*t));
    if (!t) return NULL;

    uint8_t *stack = (uint8_t *)kmalloc(THREAD_STACK_SIZE);
    if (!stack) { kfree(t); return NULL; }

    t->stack_base = stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->id         = alloc_thread_id();
    t->parent_id  = parent->id;
    t->exit_code  = 0;
    t->state      = THREAD_READY;
    /* Chapter 92 — set home_cpu for the SMP scheduler.  Every
     * other thread-creation path in this file does this; fork
     * was missed in the chapter-92 sweep, leaving home_cpu as
     * whatever garbage kmalloc returned.  When that garbage was
     * >= SMP_MAX_CPUS, runq_push_to → runq_push_remote silently
     * dropped any wake of the child (e.g. a pty pipe_read
     * unblock), so a forked-then-exec'd /bin/sh inside gui_term
     * would freeze on first input.  See chapter-93 trap notes. */
    t->home_cpu   = cpu_current_id();
    /* Inherit name verbatim — the child IS the parent at this
     * point.  After exec we'll rename via thread_rename. */
    thread_set_name(t, parent->name);
    t->next       = NULL;
    t->all_next   = NULL;
    t->as         = child_as;
    t->wake_at_ms = 0;
    t->blocked_on = NULL;
    /* tty_raw / sig_pending are deliberately NOT inherited.
     *   - tty_raw: matches the spawn() convention; the canonical
     *     fork-then-exec pattern in a shell wants the child to
     *     start in cooked mode regardless of the shell's mode.
     *   - sig_pending: signals are not inheritable (POSIX). */
    t->tty_raw    = 0;
    t->sig_pending = 0;
    /* Signal dispositions ARE inherited across fork (POSIX).
     * exec() resets them — see sys_exec. */
    for (int s = 0; s < 32; s++) t->sig_handlers[s] = parent->sig_handlers[s];
    t->sig_restorer = parent->sig_restorer;
    /* Chapter 100 — tracing is NOT inherited.  POSIX strace
     * follows exec but not fork by default; we match that. */
    t->strace     = NULL;

    /* Inherit args buffer verbatim. */
    {
        size_t i = 0;
        for (; i + 1 < THREAD_ARGS_MAX && parent->args[i]; i++)
            t->args[i] = parent->args[i];
        t->args[i] = '\0';
    }
    /* Inherit cwd verbatim. */
    {
        size_t i = 0;
        for (; i + 1 < THREAD_CWD_MAX && parent->cwd[i]; i++)
            t->cwd[i] = parent->cwd[i];
        t->cwd[i] = '\0';
    }
    /* Inherit env block byte-for-byte (terminated by a double
     * NUL — keep copying past the first NUL until we see a
     * second one). */
    {
        size_t i = 0;
        int    seen_term = 0;
        while (i < THREAD_ENV_MAX) {
            t->env[i] = parent->env[i];
            if (parent->env[i] == '\0') {
                if (seen_term) { i++; break; }
                seen_term = 1;
            } else {
                seen_term = 0;
            }
            i++;
        }
        t->env[THREAD_ENV_MAX - 2] = '\0';
        t->env[THREAD_ENV_MAX - 1] = '\0';
    }

    /* Duplicate the fd table.  Each in_use slot becomes its own
     * fd in the child; pipe / pty refcounts get bumped so closing
     * one side in either thread leaves the other intact.  See
     * thread_inherit_fds for the full rules (also called from
     * sys_spawn{,_pipe,_redir} so a shell with a pty stdio
     * propagates the pty into its children — chapter 79b). */
    /* Chapter 93 — fork() copies the fd_table (it does not share
     * it).  We allocate a fresh refcounted table on the child,
     * then thread_inherit_fds populates it slot-by-slot from
     * the parent.  CLONE_FILES sharing is opt-in via SYS_CLONE3
     * — bare fork keeps its long-standing copy semantics. */
    t->fdt = NULL;
    vfs_init_fdtable(t);   /* zero/console-fill all slots first */
    thread_inherit_fds(t, parent);

    all_push(t);

    /* Build the 288-byte exception/cswitch frame. */
    uint8_t *frame_top   = stack + THREAD_STACK_SIZE;
    uintptr_t top_aligned = ((uintptr_t)frame_top) & ~((uintptr_t)0xF);
    uint8_t *frame        = (uint8_t *)(top_aligned - 288);

    for (size_t i = 0; i < 288; i++) frame[i] = 0;

    /* x0..x30 from parent's saved trap frame.  Force x0 = 0 for
     * the child's fork() return value. */
    uint64_t *gpr = (uint64_t *)frame;
    for (int i = 0; i < 31; i++) gpr[i] = parent_frame->x[i];
    gpr[0] = 0;

    /* ELR / SPSR / SP_EL0 — child resumes exactly where parent
     * was when it issued the fork SVC. */
    *(uint64_t *)(frame + 256) = parent_frame->elr;   /* ELR_EL1  */
    *(uint64_t *)(frame + 264) = parent_frame->spsr;  /* SPSR_EL1 */
    *(uint64_t *)(frame + 272) = parent_sp_el0;       /* SP_EL0   */
    /* offset 280..287 = zero pad (already done above) */

    t->sp = (uint64_t)(uintptr_t)frame;

    runq_push(t);
    atomic_add_return32(&g_thread_count, 1);
    return t;
}

void schedule(void)
{
    /* Defensive guard: an IRQ that fires before thread_init() ran
     * (e.g. during GIC bring-up) should not crash the kernel. */
    if (!g_current)
        return;
    yield();
}

void yield(void)
{
    /* Mask IRQs for the entire scheduling critical section.
     *
     * Without this, a timer IRQ that fires AFTER we set
     * `g_current = next` but BEFORE cswitch_to has actually
     * swapped stacks recursively re-enters the scheduler, sees
     * g_current pointing at `next`, treats `next` as the
     * outgoing thread, and writes the IRQ frame's address (which
     * lives on PREV's stack) into `next->sp`.  The next time we
     * switch into `next`, cswitch_to unspools that stale frame
     * and erets to garbage ELR/SPSR — usually crashing as
     * EC=0x22 (PC alignment) or EC=0x0e (illegal exec state).
     *
     * cswitch_to's eret restores SPSR for the resumed thread
     * (kernel: 0x345 with I=0; user: 0x340 with I=0), so IRQs
     * come back on automatically when we land in `next`.  The
     * only path that needs to manually restore is the early
     * return below where we never call cswitch_to. */
    uint64_t saved_daif;
    __asm__ volatile("mrs %0, daif" : "=r"(saved_daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");

    drain_stack_to_free();

    /* Wake any sleeper whose deadline has passed.  Walks all
     * threads, so O(n) per yield.  n is small (< 20 in our
     * configuration) and the alternative is a sorted timer
     * wheel, which we don't need yet.  We skip g_current; the
     * thread_sleep_ms() loop tests the wall clock independently
     * so it doesn't need state-mutation help here.
     *
     * CHAPTER 92 — both CPUs may now run this walk.  We hold
     * g_all_lock for the iteration so a concurrent fork/exit on
     * the other CPU can't be modifying the list, and we route
     * each wake via runq_push_to so a CPU-0-pinned sleeper that
     * gets noticed first by CPU 1 still lands on CPU 0's runq
     * (preserving home_cpu affinity).  Pre-chapter 92 this walk
     * was gated on cpu_current_id() == 0 because runq_push went
     * to the LOCAL runq — so CPU 1 would have stolen sleepers
     * onto itself and silently corrupted their AS-on-stack. */
    {
        uint64_t now = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
        spin_lock(&g_all_lock);
        for (struct thread *t = g_all_head; t; t = t->all_next) {
            if (t == g_current) continue;
            if (t->state == THREAD_SLEEPING && now >= t->wake_at_ms) {
                t->state = THREAD_READY;
                runq_push_to(t);
            }
        }
        spin_unlock(&g_all_lock);
    }

    struct thread *prev = g_current;
    struct thread *next = runq_pop();

    if (!next) {
        /* Runq empty.  If this CPU has an idle thread and we're
         * not already running it, switch to it now (chapter 89).
         * Otherwise keep running prev.  Restore the caller's
         * IRQ state since we never go through eret. */
        struct thread *idle = cpu_current()->idle;
        if (idle && prev != idle) {
            next = idle;
        } else {
            __asm__ volatile("msr daif, %0" :: "r"(saved_daif) : "memory");
            return;
        }
    }

    if (prev->state == THREAD_RUNNING && prev != cpu_current()->idle) {
        prev->state = THREAD_READY;
        runq_push(prev);
    }
    /* If prev is the per-CPU idle thread, NEVER push it onto the
     * runq — it would starve real threads.  Idle gets switched
     * to via the empty-runq fall-through above instead.
     * If prev->state is EXITED, WAITING, or SLEEPING we also do
     * not re-enqueue. */

    /* Chapter 92 — prev==next fast-path.  Can happen if a
     * cross-CPU wake fired while we were inside thread_block_on
     * (between state=BLOCKED and the actual cswitch); the wake
     * marked us READY and pushed us back onto our home runq,
     * runq_pop returned us, and now we'd cswitch_to ourselves.
     * The cswitch is a wasteful no-op at best and may interact
     * badly with the diag stack-range check; short-circuit it. */
    if (next == prev) {
        next->state = THREAD_RUNNING;
        __asm__ volatile("msr daif, %0" :: "r"(saved_daif) : "memory");
        return;
    }

    next->state = THREAD_RUNNING;
    g_current   = next;

    /* Per-process address space: activate before swapping stacks
     * so that on the EL0 return (for user threads) the right page
     * tables are live.  For kernel threads (next->as == NULL) this
     * restores the boot L1.  Skip the swap entirely if both prev
     * and next share the same AS pointer — the common case for two
     * kernel threads is NULL == NULL, which means we don't pay
     * the TLBI cost on every reschedule. */
    if (next->as != prev->as)
        address_space_activate(next->as);

    /* M58 DIAG: trap any thread whose saved kernel sp lies outside
     * the heap region OR a known kernel stack region.  Heap is
     * 0x80000000..0x90000000 (256 MiB).  Boot stack is the
     * linker-defined [stack_bottom, stack_top) region.  Chapter 89
     * adds the secondary boot stacks region for per-CPU idle
     * threads, which run on .secondary_stacks (not heap). */
    extern uint8_t stack_bottom[], stack_top[];
    extern uint8_t secondary_stacks_bottom[], secondary_stacks_top[];
    int sp_in_heap   = (next->sp >= 0x80000000ULL && next->sp < 0x90000000ULL);
    int sp_in_boot   = (next->sp >= (uint64_t)(uintptr_t)stack_bottom &&
                        next->sp <  (uint64_t)(uintptr_t)stack_top);
    int sp_in_secsk  = (next->sp >= (uint64_t)(uintptr_t)secondary_stacks_bottom &&
                        next->sp <  (uint64_t)(uintptr_t)secondary_stacks_top);
    int sp_zero_idle = (next->sp == 0 && next == cpu_current()->idle);
    if (!sp_in_heap && !sp_in_boot && !sp_in_secsk && !sp_zero_idle) {
        serial_puts("[sched] PANIC: next->sp out of valid range; thread '");
        serial_puts(next->name);
        serial_puts("' sp=");
        serial_puthex(next->sp);
        serial_puts(" stack_base=");
        serial_puthex((uint64_t)(uintptr_t)next->stack_base);
        serial_puts(" tid=");
        serial_puthex((uint64_t)next->id);
        serial_puts(" state=");
        serial_puthex((uint64_t)next->state);
        serial_puts("\n");
        for (;;) __asm__ volatile("wfe");
    }

    cswitch_to(&prev->sp, next->sp);
    /* On return: eret restored SPSR with I=0, so IRQs are
     * already on for the resumed thread.  No daif restore needed. */
}

void thread_sleep_ms(uint64_t ms)
{
    if (!g_current) return;
    uint64_t target = timer_ticks() * (uint64_t)TICK_INTERVAL_MS + ms;
    /* Loop on the wall clock rather than on state.  This keeps
     * the wake-up walk simple (it doesn't have to special-case
     * current) and is robust to spurious wakeups. */
    while (timer_ticks() * (uint64_t)TICK_INTERVAL_MS < target) {
        /* Chapter 79b: if a signal has landed (e.g. Ctrl-C from
         * gui_term while we're inside `sleep 30`), break out
         * early.  The dispatcher's pre-eret sig_pending check
         * will then terminate the thread with code 128+SIGINT
         * instead of letting it idle out the full duration. */
        if (g_current->sig_pending) break;
        g_current->wake_at_ms = target;
        g_current->state      = THREAD_SLEEPING;
        yield();
        /* On the way back from yield, our state was set to
         * RUNNING by the next->state line in some other CPU's
         * yield.  If yield() happened to be a no-op (no other
         * thread to run), state is still SLEEPING but we'll
         * just loop and check the clock again. */
    }
    g_current->state = THREAD_RUNNING;
}

void thread_block_on(void *token)
{
    if (!g_current) return;
    g_current->blocked_on = token;
    g_current->state      = THREAD_BLOCKED;
    /* Single yield — the caller is expected to re-check the
     * wake condition and re-block if necessary.  Spurious
     * wakeups are allowed (current single-CPU scheduler
     * doesn't generate them, but the contract says the caller
     * must loop). */
    yield();
    /* When we get here some other thread called
     * thread_wake_blocked(token) which set our state back to
     * READY and pushed us; the scheduler then ran us and set
     * state to RUNNING.  blocked_on was cleared by the waker. */
}

/* Chapter 92 — global-lock helpers used by the futex slow path.
 *
 * thread_global_lock takes g_all_lock with IRQs masked and
 * returns the saved DAIF cookie so the caller can release
 * symmetrically.  Holding this lock blocks both fork-style
 * structural changes to g_all_head AND concurrent
 * thread_wake_blocked walks, which is exactly the serialization
 * needed to close the futex wait/wake race across CPUs.
 *
 * thread_block_on_held takes a token and the cookie returned by
 * thread_global_lock, sets the calling thread BLOCKED on the
 * token, releases g_all_lock + restores IRQs, then yields.  The
 * release-then-yield window is benign: any wake that fires inside
 * it observes us as BLOCKED, marks us READY, and pushes us back
 * onto our home runq.  yield()'s prev==next fast-path then makes
 * the round trip a no-op, or runq_pop returning a different
 * thread leaves us safely on our home runq for later. */
uint64_t thread_global_lock(void)
{
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    return f;
}

void thread_global_unlock(uint64_t flags)
{
    spin_unlock(&g_all_lock);
    irq_restore(flags);
}

void thread_block_on_held(void *token, uint64_t flags)
{
    if (!g_current) {
        spin_unlock(&g_all_lock);
        irq_restore(flags);
        return;
    }
    g_current->blocked_on = token;
    g_current->state      = THREAD_BLOCKED;
    spin_unlock(&g_all_lock);
    irq_restore(flags);
    yield();
}

void thread_wake_blocked(void *token)
{
    if (!token) return;
    /* Chapter 92 — hold g_all_lock while iterating so the walk
     * is safe on either CPU and route each wake via
     * runq_push_to so a thread blocked on CPU 0 that gets woken
     * by CPU 1 (e.g. a futex unlock from the parser thread on
     * CPU 1) lands back on CPU 0's runq.  Pre-chapter 92 this
     * was unsynchronised and used runq_push (always local) — fine
     * when only CPU 0 ran user code, broken once CPU 1 does. */
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    for (struct thread *t = g_all_head; t; t = t->all_next) {
        if (t->state == THREAD_BLOCKED && t->blocked_on == token) {
            t->blocked_on = NULL;
            t->state      = THREAD_READY;
            runq_push_to(t);
        }
    }
    spin_unlock(&g_all_lock);
    irq_restore(f);
}

struct thread *thread_lookup(int id)
{
    for (struct thread *t = g_all_head; t; t = t->all_next)
        if (t->id == id) return t;
    return NULL;
}

/*
 * thread_waitpid — generalised reaper.  See header comment.
 *
 *   target_pid > 0   : wait for that specific child only.
 *   target_pid <= 0  : wait for ANY child (legacy thread_wait).
 *   options & WNOHANG: poll-only, never block.  Returns 0 if a
 *                      matching child exists but none have
 *                      exited yet.
 *
 * Returns:
 *    > 0  : reaped child's id (exit code in *code_out if !NULL)
 *      0  : WNOHANG and no exited match
 *     -1  : no child matched the filter at all
 */
int thread_waitpid(int target_pid, int *code_out, int options)
{
    int my_id = g_current->id;

    for (;;) {
        int has_match = 0;
        struct thread *exited = NULL;

        for (struct thread *t = g_all_head; t; t = t->all_next) {
            if (t->parent_id != my_id) continue;
            if (target_pid > 0 && t->id != target_pid) continue;
            has_match = 1;
            if (t->state == THREAD_EXITED) { exited = t; break; }
        }

        if (!has_match) return -1;

        if (exited) {
            int     child_id = exited->id;
            int     code     = exited->exit_code;
            all_remove(exited);
            strace_release(exited);
            if (exited->stack_base) kfree(exited->stack_base);
            /* Tear down per-process page tables and any user pages
             * they own.  No-op for kernel threads (as == NULL). */
            if (exited->as) address_space_destroy(exited->as);
            kfree(exited);
            atomic_add_return32(&g_thread_count, (uint32_t)-1);
            if (code_out) *code_out = code;
            return child_id;
        }

        if (options & WNOHANG) return 0;

        /* Children exist but none done — sleep. */
        g_current->state = THREAD_WAITING;
        yield();
        /* When woken, loop back and re-scan. */
    }
}

/*
 * thread_wait — legacy "wait for any child" entry point.
 * Equivalent to thread_waitpid(-1, code_out, 0).
 */
int thread_wait(int *code_out)
{
    return thread_waitpid(-1, code_out, 0);
}

void thread_exit(int code)
{
    g_current->exit_code = code;
    g_current->state     = THREAD_EXITED;

    /* Drop any GUI windows owned by this pid so the desktop
     * doesn't keep painting orphans after the app dies. */
    wm_destroy_owner((uint64_t)g_current->id);

    /* Chapter 108d — if this thread was holding the
     * framebuffer mapping (i.e. it was the WSD), release the
     * single-owner slot so a respawned WSD can re-claim. */
    wsd_fb_release_owner((uint64_t)g_current->id);

    /* Chapter 108d — release every shareable per-
     * window backing object this pid owned or mapped.  Must
     * run BEFORE address_space_destroy so the mapping-AS
     * pointers in g_table[] never dangle. */
    win_fb_release_pid((uint64_t)g_current->id);

    /* Close every fd before we tear down — pipe refcounts must
     * drop so the other side of any pipe sees EOF / -EPIPE.
     * Without this, a producer's exit doesn't unblock a
     * consumer waiting on read, and the pipeline hangs. */
    vfs_close_all(g_current);

    /* Reap any of our children that have already exited (would
     * otherwise become unreapable zombies because the only
     * thing that ever reaps is wait()), and orphan the still-
     * living ones (parent_id = -1) so their own thread_exit()
     * can self-remove via the no-parent path below.
     *
     * Without this, every fire-and-forget spawn (like notepad's
     * /bin/notify toast) leaks a thread struct + a stack + an
     * address space when the parent eventually exits. */
    {
        int my_id = g_current->id;
        struct thread *t = g_all_head;
        while (t) {
            struct thread *next = t->all_next;
            if (t->parent_id == my_id) {
                if (t->state == THREAD_EXITED) {
                    all_remove(t);
                    strace_release(t);
                    if (t->stack_base) kfree(t->stack_base);
                    if (t->as) address_space_destroy(t->as);
                    kfree(t);
                    atomic_add_return32(&g_thread_count, (uint32_t)-1);
                } else {
                    t->parent_id = -1;
                }
            }
            t = next;
        }
    }

    /* The kernel-side stack of an exited thread can be freed once
     * the next context switch is in progress (we are still on it
     * right now).  Defer the free to drain_stack_to_free.  The
     * slot is per-CPU because two CPUs may have a thread exit at
     * the same moment and a single global slot would lose one. */
    cpu_current()->stack_to_free = g_current;

    /* If our parent is currently in wait(), wake it.  Either way,
     * post SIGCHLD against the parent so a long-running daemon
     * with a SIGCHLD handler is notified asynchronously.  The
     * default action for SIGCHLD in the dispatcher tail is
     * "ignore" (POSIX), so a parent that doesn't catch it isn't
     * killed by its child's exit. */
    if (g_current->parent_id >= 0) {
        thread_signal_pid(g_current->parent_id, SIGCHLD);
        struct thread *p = thread_lookup(g_current->parent_id);
        if (p && p->state == THREAD_WAITING) {
            p->state = THREAD_READY;
            /* Chapter 92 — a child can exit on a different CPU
             * than its parent runs on (e.g. parser thread on
             * CPU 1 dies while main is on CPU 0).  Route the
             * waker back to the parent's home CPU. */
            runq_push_to(p);
        }
    } else {
        /* No parent — nobody will ever reap us; free the struct
         * once our stack is gone.  Mark stack_to_free as our struct
         * AND remove from all_head so wait() never sees us. */
        all_remove(g_current);
        /* The struct itself will leak until next reaper sweep; for
         * a tiny kernel the boot thread is the only orphan and it
         * never exits, so this is fine. */
    }

    /* Yield away — never to return.  When prev->state == EXITED
     * yield() does not re-enqueue us. */
    yield();

    /* Defensive: if for some reason we ever come back, halt. */
    for (;;) __asm__ volatile("wfe");
}

/* ------------------------------------------------------------------
 * Signal delivery (minimum-viable POSIX subset)
 * ------------------------------------------------------------------
 * The only signal source today is the cooked-mode console
 * detecting Ctrl-C and signalling the foreground PID.  All
 * signals have the default action "terminate the thread with
 * exit code 128 + sig"; there are no userspace signal handlers
 * yet (no signal()/sigaction() syscalls).
 *
 * The check happens at two points:
 *   1. The cooked-mode read in vfs_read returns -EINTR after
 *      consuming a Ctrl-C byte.
 *   2. svc_dispatch examines sig_pending right before returning
 *      to user space; if any bit is set, it calls thread_exit
 *      with the appropriate code instead of returning normally.
 * ------------------------------------------------------------------ */

static int g_fg_pid = 0;     /* 0 == "no foreground process" */

void thread_signal_pid(int pid, int sig)
{
    if (pid <= 0 || sig <= 0 || sig >= 32) return;
    struct thread *t = thread_lookup(pid);
    if (!t) return;
    /* SIGKILL is unblockable / unignorable; we treat it the same
     * as everything else for now (set bit, terminate at next
     * dispatch).  Real POSIX would deliver it synchronously even
     * across blocking syscalls, but our cooperative model gets
     * close enough: blocked threads will see the signal as soon
     * as they wake. */
    t->sig_pending |= ((uint32_t)1 << sig);

    /* Chapter 79b — if the target is currently THREAD_BLOCKED
     * (e.g. sh sitting in pipe_read on the pty's m2s ring),
     * wake it so the next syscall return path notices the
     * pending signal.  Without this, Ctrl-C from gui_term
     * would only fire when the shell next did some other
     * syscall — which for a shell in read() means "never
     * until the user types an Enter."  The blocking call
     * (pipe_read, etc.) is responsible for re-checking
     * sig_pending after returning from thread_block_on and
     * propagating -EINTR up to userspace.
     *
     * Same logic for THREAD_SLEEPING: a foreground `sleep 30`
     * targeted by Ctrl-C should die immediately rather than
     * waiting out its full duration.  thread_sleep_ms checks
     * sig_pending after each yield, so just nudging the
     * thread back onto the runqueue is enough. */
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        t->blocked_on = NULL;
        t->state      = THREAD_READY;
        /* Chapter 92 — the signal might be raised from any CPU
         * (e.g. a Ctrl-C arriving on the gui_term thread on
         * CPU 1 against a sleep'ing shell on CPU 0).  Route
         * the wake back to the target's home CPU. */
        runq_push_to(t);
    }
}

int thread_get_fg_pid(void)
{
    return g_fg_pid;
}

void thread_set_fg_pid(int pid)
{
    g_fg_pid = pid;
}

/* Forward decl satisfied by arch/context_switch.S. */

# Chapter 88 — Atomics, barriers, and the audit of every shared global

Chapter 87 woke CPU 1. CPU 1 doesn't actually do anything yet —
it spins forever in a `WFE` loop — but its mere existence breaks
a guarantee the kernel has relied on for 86 chapters: that
exactly one CPU ever touches kernel data structures. Once
chapter 90's scheduler runs threads on CPU 1, every plain
`x++` on a shared variable becomes a lost-update race, every
list push/pop becomes a torn pointer, every `if (free)
{ free; reuse; }` becomes a double-free.

This chapter doesn't *fix* any of that — there's nothing to
fix yet, because CPU 1 is parked. It builds the **vocabulary**
we'll need to fix it, proves that vocabulary works on the
hardware, and produces an audit of every shared global so we
know exactly what chapter 90 will have to lock.

The proof point is concrete: at the end of `smp_init`, both
CPUs race to increment the same 64-bit counter 100,000 times.
If atomicity works, the final value is exactly 200,000. If it
doesn't, we lose updates and the boot log says so.

## Prerequisites

- Chapter 87 — second core via PSCI (atomics need two CPUs to
  be testable).
- Chapter 6 — MMU + cacheable memory (LL/SC requires the
  cacheable memory type).

## Why `x++` is wrong on SMP

A simple `g_counter++` compiles to:

```asm
ldr w0, [x1]      ; load
add w0, w0, #1    ; modify
str w0, [x1]      ; store
```

On uniprocessor this is fine: nothing else can run between
the three instructions because the only "something else" is
an IRQ, and the IRQ handler doesn't touch `g_counter`. (If it
did, we'd already be wrong, but we got away with it.)

On SMP the same three-instruction sequence on two CPUs can
interleave like this:

| Time | CPU 0          | CPU 1          | g_counter |
|------|----------------|----------------|-----------|
| 1    | ldr w0  → 0    |                | 0         |
| 2    |                | ldr w0  → 0    | 0         |
| 3    | add w0,w0,#1   |                | 0         |
| 4    |                | add w0,w0,#1   | 0         |
| 5    | str w0  → 1    |                | 1         |
| 6    |                | str w0  → 1    | 1         |

Both CPUs incremented; the counter went up by exactly **one**.
That's a lost-update race. With 100,000 increments per CPU,
roughly 1-5% of them are typically lost on a contended counter
— the smoke test in this chapter would print
`got=0x310db expected=0x30d40 MISMATCH (lost ...)` if we used
plain `++`.

## LL/SC: Load-Exclusive / Store-Exclusive

ARMv8's answer is the **exclusive monitor**, which is a piece
of hardware in each CPU's L1 cache controller. The contract:

- `LDXR Wt, [Xn]` (Load-Exclusive Register) reads from memory
  AND sets the monitor on the cache line containing `[Xn]`.
- `STXR Ws, Wt, [Xn]` (Store-Exclusive Register) writes to
  memory ONLY IF this CPU's monitor is still set on that
  cache line. If another CPU touched the line in between,
  the monitor was cleared and the store fails. The `Ws`
  register receives 0 on success, 1 on failure.

So the canonical atomic-add sequence is:

```asm
1: ldaxr   w0, [x1]       ; reserve line, load old
   add     w2, w0, w3     ; compute new
   stlxr   w4, w2, [x1]   ; try to store
   cbnz    w4, 1b         ; failed? retry from the top
```

The `a` and `l` letters in `ldaxr`/`stlxr` add memory-order
semantics: `ldaxr` is a load-acquire (no subsequent load/store
on this CPU can be reordered before it), `stlxr` is a
store-release (no prior load/store on this CPU can be reordered
after it). Together they form an acquire/release pair, which
is exactly the C11 / Linux kernel atomic ordering model.

This pattern is the entire content of `kernel/arch/atomic.h`.

## What chapter 88 ships in `atomic.h`

A header-only library with one shape, replicated for 32-bit
and 64-bit operands:

| Operation                    | Returns      | Use                  |
|------------------------------|--------------|----------------------|
| `atomic_load{32,64}`         | the value    | "what is it now?"    |
| `atomic_store{32,64}`        | void         | "set it to V"        |
| `atomic_add_return{32,64}`   | the NEW val  | counters, allocators |
| `atomic_sub_return{32,64}`   | the NEW val  | refcounts, decrements|
| `atomic_or_return32`         | the NEW val  | bitfield bit-set     |
| `atomic_cmpxchg32`           | 0/1 success  | lock-free updates    |

…plus barrier wrappers `dmb_ish`, `dmb_ishst`, `dsb_ish`,
`isb_`. The whole header is ~250 lines including comments,
all inline asm, no dependencies.

A few design choices worth calling out:

- **No ARMv8.1 LSE** (`LDADD`, `CAS`, `SWP`). LSE is shorter
  code and faster under contention but cortex-a72 — the
  `-cpu` baseline for QEMU virt — does not implement it. We'd
  need a runtime feature check (`ID_AA64ISAR0_EL1.Atomic`)
  and two code paths. At our scale (≤4 CPUs, hobby OS) the
  LL/SC retry loop is invisible; the pedagogical clarity of
  "see the explicit retry" wins.

- **`_return` everywhere**. Linux uses `atomic_inc` (no
  return value) and `atomic_inc_return` (returns new value).
  We just use `_return` for everything: it's free (the new
  value is already in a register), and it makes "this is a
  read-modify-write" explicit at every call site.

- **`volatile uint32_t *`** as the operand type. This means
  call sites don't need a cast for variables that are also
  shared with IRQ handlers (which already have to be
  `volatile`), and it forbids the compiler from folding a
  stale plain-load over our atomic.

## Memory barriers: which one when

The barriers are in `atomic.h` because they go hand-in-hand
with atomics, but they're a separate tool. The two facts you
need:

- The **shareability domain** says which observers see the
  ordering. ARM has three:
  - `IS` (Inner Shareable) — every CPU in our cache-coherent
    cluster, which on QEMU virt is every CPU PSCI lit up. This
    is what almost every kernel barrier wants.
  - `OS` (Outer Shareable) — adds devices behind cache-coherent
    interconnects; we don't have any.
  - `SY` (System) — adds non-cache-coherent observers like
    DMA controllers. Use only for MMIO and DMA descriptor
    publishing.

- The **strength** says what gets ordered:
  - `dmb` (Data Memory Barrier) — ordering only. Cheap.
  - `dsb` (Data Synchronisation Barrier) — actually wait until
    every prior load/store has *completed*. Required before
    TLBI/IC IVAU/etc cache maintenance.
  - `isb` (Instruction Synchronisation Barrier) — flush the
    prefetch pipeline. Required after writing system registers
    that affect instruction fetch (`SCTLR`, `TTBR`, `VBAR`).

The 90% case is `dmb_ish()`. We use `dsb_ish()` only inside
the MMU code (chapter 6) and after writing a CPU's READY flag
(`secondary_main`, chapter 87). `isb_` belongs to the boot
sequence and the context switch.

## Proving it works: the SMP smoke test

The proof point lives at the end of `smp_init_with_dtb`.
Choreography:

1. **CPU 0** runs the PSCI CPU_ON loop, then calls
   `smp_smoke_hammer()` — 100,000 calls to
   `atomic_add_return64(&g_smoke_counter, 1)`. This is CPU 0's
   share of the increments.

2. **CPU N (N ≥ 1)** enters `secondary_main`, immediately calls
   `smp_smoke_hammer()` (also 100,000 increments), THEN sets
   `CPU_FLAG_READY`. The READY-after-hammer ordering is the
   key: it makes CPU 0's "spin-wait until everyone READY"
   double as a join barrier for the smoke test.

3. After CPU 0's spin-wait succeeds, it calls
   `smp_smoke_verify(online_cpus)` which reads the counter
   once and compares against `100,000 * online_cpus`.

The boot log on `-smp 2`:

```
[smp] PSCI CPU_ON cpu=1 mpidr=0x...1 entry=0x...
[smp] waiting for secondaries to report ready ...
[smp] CPU 1 ready (mpidr = 0x...1)
[smp] all CPUs online
[smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
```

`0x30d40` is hex for 200,000 — exactly `100k × 2 CPUs`. If
either CPU lost a single update, the count would come out
lower and the line would say `MISMATCH`. We verified the
mismatch path during development by temporarily replacing the
atomic with a plain `g_smoke_counter++` — the lost-update
count was reproducibly in the 1,000-3,000 range.

This same smoke runs on every boot and therefore every
regression test. Atomicity isn't a feature you add and forget;
it's a feature that has to keep working.

## The audit: every shared global, classified

This is the meaty content of the chapter. Walk every
`static` global in `kernel/` and assign it to one of four
categories.

### Category 1 — Immutable post-init

Set once at boot, never modified again. No locking required;
plain reads from any CPU are safe.

| File             | Symbol(s)                              |
|------------------|----------------------------------------|
| `kernel/core/main.c`    | embedded user binary pointers   |
| `kernel/arch/cpu.c`     | `g_cpus[]` (after `smp_init`)   |
| `kernel/arch/cpu.c`     | `g_smp_count`                   |
| `kernel/arch/page_tables.c` | `l1_pgtable`, `l2_pgtable*` |
| `kernel/device/virtio_*` | `*_base`, `*_qsz`, `*_mac`     |
| `kernel/core/osfs.c`    | `g_files[]`, `g_file_count`     |
| `kernel/core/osfs2_journal.c` | `g_header_block`, `g_data_block0`, `g_max_blocks` |
| `kernel/core/timer.c`   | `g_interval_ticks`              |

### Category 2 — Per-CPU (will live behind `cpu_current()`)

Read by many CPUs but written only by the owning CPU. Today
these are all on CPU 0 because CPU 1 is parked; once chapter
89 scheduler runs they need to migrate behind
`cpu_current()->...`.

| File             | Symbol                | Future home               |
|------------------|-----------------------|---------------------------|
| `kernel/core/thread.c` | `g_current`     | `cpu_current()->current`  |
| `kernel/core/thread.c` | `g_runq_head/tail` | per-CPU runqueue       |

### Category 3 — Atomic counter

Single integer that gets incremented (or compare-exchanged).
Replace `++` with `atomic_add_return`.

| File                | Symbol                 | Done?              |
|---------------------|------------------------|--------------------|
| `kernel/core/thread.c` | `g_next_id` (tid alloc) | **Done this chapter** |
| `kernel/core/thread.c` | `g_thread_count`      | TODO, chapter 90   |
| `kernel/core/timer.c`  | `g_ticks`             | TODO, chapter 89   |
| `kernel/core/osfs2_cache.c` | `g_clock`, `g_hits`, `g_misses` | TODO, chapter 90   |
| `kernel/core/osfs2_journal.c` | `g_next_txn_id`, `g_commit_count`, `g_replay_count`, `g_journalled_blocks` | TODO, chapter 90 |
| `kernel/core/wm.c`     | `g_next_z`            | TODO, chapter 90   |

### Category 4 — Lock-protected (needs `spinlock_t` or `reclock_t`)

Multi-field state where atomicity has to span more than one
read-modify-write. Need an explicit lock around the critical
section.

| File             | State                      | Lock plan                 |
|------------------|----------------------------|---------------------------|
| `kernel/core/thread.c` | runqueue + thread table | one big spinlock first; per-CPU + work-stealing later (chapter 90) |
| `kernel/core/vfs.c`    | open-file table         | per-thread already; nothing to do until threads share fdtables (clone CLONE_FILES, far future) |
| `kernel/core/heap.c`   | kheap free-list         | one spinlock around the whole allocator (chapter 90) |
| `kernel/core/pmem.c`   | `g_free_head`, `g_free_pages` | one spinlock around alloc/free (chapter 90) |
| `kernel/core/wm.c`     | window list + focus     | one spinlock (chapter 90; today only one thread mutates this) |
| `kernel/core/osfs2_cache.c` | LRU list + slab     | one spinlock per cache (chapter 90) |
| `kernel/core/tcp.c`    | socket table + TCB list | one spinlock (chapter 89 or 89; IRQ-safe variant required) |
| `kernel/core/dns.c`    | response slot           | already volatile + barrier; works because only one outstanding query |
| `kernel/core/dhcp.c`   | state machine           | same as DNS — single-flight |
| `kernel/core/serial.c` | PL011 + `g_serial_lock` | **Done in chapter 87** |

### Category-5 anti-pattern: `volatile` instead of an atomic

There's a pattern in the kernel of using plain `volatile int`
for a flag set in one context and read in another. On
uniprocessor that's fine — `volatile` forbids the compiler
from caching the load — but on SMP the read and write may
*also* need a memory barrier so that earlier writes are
visible alongside the flag write.

The places in the kernel that do this today:

- `g_icmp_reply_seen` in `main.c` (set by network IRQ).
- `g_state`, `g_have_subnet`, `g_have_router`, `g_have_dns`
  in `dhcp.c`.
- `g_pending`, `g_resp_len` in `dns.c`.

For chapter 88 these are all left as-is because (a) only one
CPU runs the network stack today, and (b) chapter 89's IPI
work will revisit them as part of the IRQ-affinity changes.
The audit table flags them so we don't forget.

## What we did NOT do this chapter

- **No ticket-spinlock upgrade.** The flat test-and-set
  `spinlock_t` in `arch/spinlock.h` (chapter 87) is unfair —
  under contention a CPU that just released the lock can
  re-acquire before queued waiters. With `SMP_MAX_CPUS = 4`
  and our current "one global lock, short critical sections"
  pattern, that unfairness is unmeasurable. If chapter 90
  gets per-CPU runqueues with high-rate stealing, we'll
  upgrade to a ticket lock then.

- **No actual locking of category-4 state.** Until chapter
  89 runs threads on CPU 1, no shared state is contended,
  and adding the locks now would just be uncovered code.
  The audit table is the deliverable; the locking is the
  chapter-89 deliverable.

- **No LSE atomics.** As above.

## When things don't work

**Trap: forgetting `volatile` on the atomic operand.**
GCC under `-O2` happily folds `atomic_load32(&x)` calls if
`x` isn't `volatile` — it sees two loads of the same address
with no intervening store from this thread's perspective and
keeps the result of the first in a register. The fix is to
declare the variable `volatile uint32_t` in the first place,
which `atomic.h`'s API encourages by typing its argument as
`volatile uint32_t *`.

**Trap: `dsb sy` where you wanted `dmb ish`.** The system
barrier is correct but ~5x slower because it stalls until
*every* observer (including DMA controllers) acknowledges. We
used `dsb sy` in `secondary_main`'s READY publish (chapter 87)
because it was easier to reason about during bring-up; the
correct version is `dmb_ishst()`, and chapter 89 will switch
to it once the GIC IPI work makes the timing observable.

## Files added or changed

- **`kernel/arch/atomic.h`** *(new)* — header-only LL/SC
  vocabulary + barrier wrappers.
- **`kernel/arch/cpu.c`** — added `g_smoke_counter`,
  `smp_smoke_hammer`, `smp_smoke_verify`. Wired into both
  `secondary_main` (CPU 1) and `smp_init_with_dtb` (CPU 0).
- **`kernel/core/thread.c`** — `g_next_id` is now
  `volatile uint32_t` and read via `atomic_add_return32`
  through a tiny `alloc_thread_id()` helper. Four call
  sites updated.
- **`scripts/_dbg_smp_boot.py`** — added the
  `[smp-atomic] ... OK` marker to the required-list.

## Build & test

```
$ make all
$ python3 scripts/_dbg_smp_boot.py
…
[smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
…
OK   [smp] bringing up additional cores
OK   [smp] DTB reports 2 cpu(s)
OK   [smp] PSCI CPU_ON cpu=1
OK   [smp] CPU 1 ready
OK   [smp] all CPUs online
OK   [smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
```

The full single-CPU regression suite (24 tests) still passes.

## What this unlocks

- **Chapter 89 — IPIs via GICv3 SGIs.** Now that we have
  atomics, the IPI mailbox can be a `volatile uint32_t`
  bitfield modified with `atomic_or_return32` from the sender
  and `atomic_load32` + cmpxchg-clear from the receiver.
- **Chapter 90 — SMP scheduler.** The category-4 locks finally
  get installed, the runqueue gets a spinlock, and CPU 1
  actually runs threads.
- **Future per-CPU statistics.** With 64-bit atomics we can
  add cheap `g_syscall_count`, `g_irq_count_per_cpu`,
  `g_pagefault_count` counters for `procfs`-style introspection
  without locking.

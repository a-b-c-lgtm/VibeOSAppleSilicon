# Chapter 72 — fork on AArch64: the address-space copy

> **Milestone in this chapter:** 65 — `fork()` lands.
> **Code referenced:**
> - [kernel/arch/address_space.c](../../../kernel/arch/address_space.c)
>   (`address_space_clone`)
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_FORK` dispatch)
>
> **At the end of this chapter** you will have a `fork()` syscall
> that returns 0 in the child and the child pid in the parent, an
> address-space cloner that eagerly memcpy's every populated user
> page, and a `fork_test` user program that prints from both halves.
> The implementation is deliberately the slow, unambiguous version;
> chapter 74 retrofits copy-on-write.

## What this chapter adds

- `SYS_FORK` — child returns 0, parent returns child pid.
- `address_space_clone()` — walks the parent's L1/L2/L3 and
  duplicates every populated user page (heap, stack, anonymous
  brk pages, the OSFS-mapped text/data view).
- A `fork_test` user program that prints from both halves.

## Prerequisites

- Chapter 23 — Per-process address spaces
- Chapter 10 — Threads and the AArch64 context switch
- Chapter 13 — SVC and the syscall ABI

## Plan

- Walk through the trap frame: parent and child must both
  return to the same instruction with different X0 values.
- Address-space clone strategy: identity-mapped scratch
  page → memcpy → install in child's L3.
- Stack page handling: copy the SP_EL0 byte-for-byte; user
  state is captured in the saved frame.
- File-descriptor table dup: refcount pipes/sockets, share
  console fds.
- Test plan: parent prints "P", child prints "C", `wait()`
  reaps the child cleanly.

## What you'll learn

- Why the return-twice trick is just "save the trap frame
  twice in the runqueue."
- Page-table cloning at every level (L1/L2/L3).
- The cost: a 1 MiB heap means 1 MiB of memcpy on every
  fork. Chapter 74 fixes this.

## What this unlocks

- `exec` (chapter 73) finally has something to replace.
- Daemonisation patterns.
- The shell's "fork before pipe-and-exec" pattern that
  every Unix shell written since 1973 uses.

## Out of scope

- COW. Eager copy only.
- Vfork's "child shares parent's AS until exec" hack.
- Threads-share-AS clone() variant.

## Postscript: how it actually shipped

### address_space_clone()

Lives in [kernel/arch/address_space.c](../../../kernel/arch/address_space.c).
The walk is straightforward — for each valid L2 entry,
recurse into the L3 table; for each valid L3 entry decode
`writable` (from the AP bits) and `executable` (from UXN),
allocate a fresh pmem page, memcpy 4096 bytes, and re-map
under the destination AS via `address_space_map`.

The **PA-as-VA memcpy** trick deserves a callout: the boot
L1 identity-maps DRAM in slots 2..N, so the kernel can read
the parent's page (at its physical address) and write the
child's page (at its physical address) without temporarily
mapping either into the *user* slot of either AS. This is
the single fact that lets a kernel-mode page-table copy be
a one-line `__builtin_memcpy`.

On OOM the partial dst AS is unwound via
`address_space_destroy`. `heap_brk` is propagated.

### Frame-size divergence: 272 vs 288

The most surprising bug surface in fork. Two different
trap-frame layouts coexist in the kernel:

- `save_context` in [kernel/arch/vectors.S](../../../kernel/arch/vectors.S)
  allocates **272 bytes** and does NOT save SP_EL0.
- `cswitch_to` in [kernel/arch/context_switch.s](../../../kernel/arch/context_switch.s)
  uses **288 bytes** with SP_EL0 at offset 272.

(Chapter 171 later grows the `cswitch_to` frame to 816 bytes
to save FP/SIMD registers q0..q31 + fpsr/fpcr. The fork logic
described here still copies only the 288-byte GPR portion --
the FP/SIMD slots are initialised by `cswitch_to`'s
first-run path, not the fork copy.)

`sys_fork` runs from the SVC path (272-byte frame), but the
child's first scheduled run goes through `cswitch_to`
(expects 288 bytes). So `sys_fork` manually builds the
288-byte variant on the child's kstack: copy gpr[0..30] from
the parent frame, force `gpr[0] = 0` (the child's return
value), then write ELR_EL1, SPSR_EL1, and SP_EL0 at offsets
256, 264, 272.

### The SP_EL0 snapshot must precede kmalloc

SP_EL0 is captured with an inline `mrs` early in `sys_fork`,
*before* any kmalloc. Reason: kmalloc can yield under heap
pressure, and the user-stack pointer we want is the parent's
at the SVC instant -- not whatever value SP_EL0 holds after a
kernel-side context switch round-trip. Reading the value
last would still work in this single-CPU/no-preempt build
(SP_EL0 is restored when the parent thread resumes), but the
intent reads more clearly when the snapshot is the first
thing we do.

### File-descriptor inheritance

- Console / file fds: dup straight across.
- Pipe fds: dup *and* bump `r_refs`/`w_refs` so the child's
  later close doesn't kill a reader the parent still owns.
- Socket fds: **not inherited.** The conn-table model is
  single-owner; a double-close in fork-then-exit would
  corrupt the table. Sockets are inherited only in chapter
  84+ when we add proper refcounting.

### Test

- Source: [userspace/forktest/forktest.c](../../../userspace/forktest/forktest.c)
- Harness: [scripts/test_fork_exec.py](../../../scripts/test_fork_exec.py)
- Three checks, gated on `[forktest] all checks passed`:
  1. **Pure fork** — pids match across the return, parent
     reaps with code 0.
  2. **fork + execv** — child exec's `/bin/hello`, runs to
     completion.
  3. **Heap is copied** — pre-fork malloc + sentinel; child
     overwrites with a different sentinel; parent (post-wait)
     verifies its copy is untouched.


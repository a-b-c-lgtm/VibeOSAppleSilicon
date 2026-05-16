# Chapter 73 — fork on AArch64: the address-space copy

**Status:** Implemented (milestone 65 / 2025-Q4).

This chapter implements `fork()` as an eager full-copy of the
parent's address space. It is deliberately the slow,
unambiguous version; chapter 75 retrofits copy-on-write.

## What this chapter adds

- `SYS_FORK` — child returns 0, parent returns child pid.
- `address_space_clone()` — walks the parent's L1/L2/L3 and
  duplicates every populated user page (heap, stack, anonymous
  brk pages, the OSFS-mapped text/data view).
- A `fork_test` user program that prints from both halves.

## Prerequisites

- Chapter 24 — Per-process address spaces
- Chapter 11 — Threads and the AArch64 context switch
- Chapter 14 — SVC and the syscall ABI

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
  fork. Chapter 75 fixes this.

## What this unlocks

- `exec` (chapter 74) finally has something to replace.
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

`sys_fork` runs from the SVC path (272-byte frame), but the
child's first scheduled run goes through `cswitch_to`
(expects 288 bytes). So we manually build the 288-byte
variant on the child's kstack: copy gpr[0..30] from the
parent frame, force `gpr[0] = 0` (the child's return value),
then write ELR_EL1, SPSR_EL1, and SP_EL0 at offsets 256, 264,
272.

### The SP_EL0 snapshot must precede kmalloc

We captured SP_EL0 with an inline `mrs` early in `sys_fork`,
*before* any kmalloc. Reason: kmalloc can yield under heap
pressure, and we want the user-stack pointer the parent had
at the SVC instant — not whatever value SP_EL0 holds after a
kernel-side context switch round-trip. Reading the value
last would still work in this single-CPU/no-preempt build
(SP_EL0 is restored when the parent thread resumes), but the
intent reads more clearly when the snapshot is the first
thing we do.

### File-descriptor inheritance

- Console / file fds: dup straight across.
- Pipe fds: dup *and* bump `r_refs`/`w_refs` so the child's
  later close doesn't kill a reader the parent still owns.
- Socket fds: **not inherited.** The M64 conn-table model is
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


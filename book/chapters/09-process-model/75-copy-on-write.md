# Chapter 75 — Copy-on-write: making fork cheap

**Status:** Implemented. Eager-copy fork from chapter 73
replaced with a lazy share-on-fork / copy-on-write path; all
chapter-73-78 regression suites still pass and a new
`scripts/test_cow.py` covers the new behaviour.

Eager-copy fork (chapter 73) costs O(allocated pages) per
fork. With a 1 MiB heap and a typical shell that forks once
per command, that is wasted work — most pages are read by
both parent and child and never written. Copy-on-write
defers the copy until the first write, which for `fork +
exec` (the common case) means almost no copying ever happens.

## What this chapter adds

- L3 entries marked read-only + AP-controlled in both
  parent and child after fork.
- A per-physical-page refcount table.
- A new fault handler branch: "write to a refcount-shared,
  write-protected page" → allocate, copy, install.
- Same plumbing reused for shared text segments later.

## Prerequisites

- Chapter 73 — fork
- Chapter 6 — MMU and page tables (AP[2:1] permissions)
- Chapter 5 — Exception vectors, ESR, FAR

## Plan

- Refcount table sizing: one byte per 4 KiB physical page is
  4 MiB for 16 GiB of physical memory — fits in BSS.
- Write-protect on fork: walk both ASs after the L3 copy and
  flip AP to RO; bump refcount on each shared page.
- Fault handler: ESR EC = 0x25 (data abort) + write + DFSC =
  permission fault → "is this a COW page?" → allocate, copy,
  install RW.
- TLB: invalidate the just-touched VA on the current CPU.
  (TLB shootdown across CPUs comes in chapter 87.)
- Stack pages: same treatment, with the same fault path.

## What you'll learn

- Why every modern OS uses COW for fork.
- The cost: one extra page fault per first-write per page,
  worth it when most pages are never written.
- A taste of how mmap shared mappings will work later
  (chapter 89).

## What this unlocks

- Shared text segments (one copy of `/bin/sh` in physical
  memory regardless of how many shells are running).
- Cheap fork-bomb defense (the bomb's pages don't multiply
  until they write).
- The `mmap` chapter (89) reuses every line of this code.

---

## Postscript — what shipped

The implementation tracked the plan above almost verbatim,
but a handful of surprises showed up at integration time
that are worth recording explicitly so chapter 89 (`mmap`)
doesn't relearn them.

### The five moving parts

1. **`kernel/core/pmem_refcount.{h,c}` — per-frame refcount
   table.** A flat `uint16_t` array indexed by
   `(pa - dram_base) / 4096`, allocated out of kheap right
   after `pmem_init` in `kernel/core/main.c`. For 8 GiB
   DRAM that's 4 MiB — small enough not to matter.

2. **`DESC_SW_COW = 1ULL << 55` in
   `kernel/arch/address_space.c`.** AArch64 reserves bits
   55-58 of stage-1 descriptors for software use; we claim
   bit 55 to mean "this RO mapping is COW: on a write
   fault, allocate a private copy." The MMU walks ignore
   the bit; only the OS reads it.

3. **`address_space_clone_cow(src)`.** Replaces the eager
   `address_space_clone` in `sys_fork`. Walks src's L3
   entries; for each writable user page it (a) downgrades
   the source descriptor to RO + DESC_SW_COW, (b) installs
   the same RO+COW descriptor in the child at the same VA,
   (c) bumps the page's refcount. RO pages (program text)
   are shared without the COW marker. After the walk, a
   `tlbi vmalle1` flushes the now-stale RW TLB entries
   from the source.

4. **`address_space_handle_cow_fault(as, fault_va)`.**
   Called from `svc_dispatch` whenever an EL0 data abort
   matches "permission fault on a write." If the page's
   refcount is ≥ 2 we allocate a fresh page, memcpy
   old→new (4 KiB via the boot L1's identity mapping),
   point the descriptor at the new PA with AP=RW + COW
   bit cleared, and dec the old page's refcount. If the
   refcount is already 1 we're the last sharer — just flip
   AP back to RW in place, no copy needed. Either way we
   `tlbi vaae1` the faulting VA so the next access sees
   the new perms.

5. **`address_space_make_writable(as, va)` + a hook in
   `copy_to_user`.** This was the surprise — see the next
   subsection.

### The non-obvious part: AArch64 RO is RO at EL1 too

The four bullets above describe the textbook COW
mechanism. In a single-AS hosted-kernel environment
that's enough. In ours it isn't, because the kernel often
writes user memory through user VAs while running in
EL1 — every `copy_to_user` call, every `deliver_signal`,
every kernel push-onto-user-stack.

AArch64 stage-1 page permissions apply to **both** ELs.
A descriptor with `AP[2:1] = 11` (RO at EL0+EL1) is
read-only from EL1 too. So the moment chapter 75 marked
the parent's still-shared pages RO, the very next
`copy_to_user(...)` into one of those pages took a data
abort at EL1 — and EL1 sync exceptions land in
`panic_entry`, which has no concept of COW.

Symptom in `chldtest`: kernel panic with
`ESR_EL1=0x9600004f` (EC=0x25 — Data Abort from same EL),
`ELR=copy_to_user+0x?`, `FAR` somewhere on the user stack.
The fix is `address_space_make_writable`: a small helper
that takes a user VA, finds its L3 descriptor, and:

- already RW                      → return 0 (no-op)
- RO + DESC_SW_COW                → resolve COW now,
                                    return 0
- RO without DESC_SW_COW          → return -1 (real RO,
                                    e.g. program text)
- not mapped                      → return -1 (EFAULT)

`copy_to_user` walks its destination range page-by-page
through this helper *before* the memcpy, forcing every
COW-shared destination page to be unshared up front. The
fault never happens at EL1 because the descriptor is
already RW by the time the kernel writes.

The lesson generalises: **if the OS ever touches user
memory through a user VA while in EL1 — and POSIX
basically guarantees you will, via `read`, `write`,
`waitpid` exit-code stores, and signal-frame writes —
your COW handler must be reachable proactively, not only
through the data-abort vector.**

### Why the refcount semantics look weird

Refcount 0 means "untracked sole owner." 1 means "one
remaining tracked holder, about to drop." 2+ means "this
many holders." First share goes 0 → 2 (not 0 → 1) because
what was previously "1 implicit holder, untracked" becomes
"2 holders, tracked."

The asymmetry exists because every `pmem_free_page` call
site in the kernel predates COW — they all assumed sole
ownership. Initialising the table to 0 by default and
jumping the share path straight to 2 lets us retrofit COW
without rewriting (or even auditing) every existing pmem
caller. The extra `pmem_dec_and_free` wrapper handles the
"is this our last reference?" decision for them.

### Performance

`scripts/test_cow.py`'s check 4 fork()s a process with an
8 MiB resident heap. Pre-COW (eager memcpy of every page),
that fork took multiple seconds on HVF and could OOM on
small RAM configurations. Post-COW it completes in 0 ms by
the kernel's millisecond timer — the work is just an L3
walk and a `tlbi vmalle1`. fork+exec is now genuinely
cheap, which is the whole point of the chapter.

### Test coverage

`scripts/test_cow.py` runs `userspace/cowtest/cowtest.c`,
which exercises:

1. 4 MiB heap COW (sentinel survives in parent, child's
   write doesn't bleed back).
2. Stack COW (subtle because stack and heap live in
   different L2 slots).
3. Kernel `copy_to_user` into a still-COW user page —
   the regression that caught the AP-applies-to-EL1 bug
   above. The parent fork()s, then immediately
   `waitpid()`s into a stack-local without touching the
   stack first; the kernel must pre-fault the COW page
   before storing the exit code.
4. Large-heap fork latency proxy.

The chapter-73 `forktest`, chapter-77 `sigtest` and
chapter-78 `chldtest` suites all still pass unchanged —
COW is semantically transparent.

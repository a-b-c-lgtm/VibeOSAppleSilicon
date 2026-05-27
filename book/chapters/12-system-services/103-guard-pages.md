# Chapter 103 — Guard pages and a friendlier stack overflow

[Chapter 26's argv/argc work](../05-devices/026-argc-argv.md) closed
with a postscript: *"a future user-mode crash with ESR EC=0x24 and
FAR a few hundred bytes below the bottom of the mapped stack is
almost certainly a stack overflow, and the right fix is a guard
page so the next overflow produces a message that actually says
'user stack overflow.'"* This chapter cashes that cheque.

The whole feature is about 80 lines of kernel C, one software bit
in the page-table descriptor, and one helper in `address_space.c`.
The payoff is the difference between this, which the kernel used
to produce on a recursive runaway:

```
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x0000000096000005
        EC      = 0x0000000000000024
        FAR_EL1 = 0x000000103ffeffb0
        ELR_EL1 = 0x0000001000100034
        SPSR    = 0x00000000a0000000
        thread  = /bin/stackbomb
```

…and this, which it produces now:

```
[svc] user stack overflow in thread "/bin/stackbomb" (pid 0x0000000000000010)
        FAR_EL1  = 0x000000103ffeffb0  (stack floor 0x000000103fff0000, overran by 0x0000000000000050 bytes)
        ELR_EL1  = 0x0000001000100034
        stack    = [0x000000103fff0000, 0x0000001040000000)  0x0000000000000010 pages
        guard    = [0x000000103ffef000, 0x000000103fff0000)  1 page (DESC_SW_GUARD)
        likely cause: unbounded recursion, or one stack frame larger than 64 KiB.
        thread killed.
```

Same ESR, same FAR, same kill — but a reader of the second
message goes straight to "I have unbounded recursion in
`recurse()` at PC `0x100100034`" instead of doing the
forensic dance.

## Prerequisites

* [Chapter 5 — Exception vectors, ESR, FAR](../02-memory/005-exception-vectors.md)
  for the EC / FAR / ISS decoding.
* [Chapter 23 — Per-process address spaces](../05-devices/023-per-process-address-spaces.md)
  for the user-stack VA layout.
* [Chapter 26 — argc/argv](../05-devices/026-argc-argv.md) for the
  original postscript that names this chapter as the fix.
* [Chapter 72 — fork](../09-process-model/072-aarch64-fork-and-as-copy.md)
  and [Chapter 74 — COW fork](../09-process-model/074-copy-on-write.md):
  the guard has to survive both clone paths.
* [Chapter 91 — mmap and the page cache](../11-smp-and-memory/091-mmap-and-page-cache.md)
  for the existing data-abort dispatch in `svc_dispatch`. The
  new guard check fits in as a third sibling next to the
  lazy-mmap and COW handlers.

## The trick: one software bit on an invalid descriptor

AArch64's stage-1 descriptor format reserves bits **\[58:55\]** for
software (ARM ARM D5.4.5). The MMU ignores them on every walk —
they only matter to the OS. We already used two of them:

* `DESC_SW_COW` (bit 55) — chapter 74, "this RO mapping is a COW
  share; on write fault, allocate and copy."
* `DESC_SW_PAGECACHE` (bit 56) — chapter 91, "this page belongs
  to the page cache; on AS teardown call `page_cache_release(pa)`
  not `pmem_dec_and_free(pa)`."

For chapter 103 we claim **bit 57**:

```c
/* kernel/arch/address_space.h */
#define DESC_SW_GUARD       (1ULL << 57)
```

The crucial property is that **software bits survive even when
`DESC_VALID` is clear.** The MMU's interpretation of an invalid
descriptor is *"reserved, treat the entry as a translation
fault"*, and "reserved" here means "don't crash, just ignore
the rest." So a descriptor with `DESC_VALID=0` and
`DESC_SW_GUARD=1` produces a normal translation fault when the
user touches it, and a moment later the fault handler can read
the descriptor back, see the guard bit, and react.

That's the whole communication channel between AS-create time
(when we install the guard) and fault time (when we want to
emit a domain-specific message). No extra table, no extra map,
no extra memory.

## Installing the guard

The user stack lives at
`[USER_STACK_TOP − 16 × 4 KiB, USER_STACK_TOP)` which is
`[0x103FFF0000, 0x1040000000)`. We install one guard page
immediately below:

```c
/* kernel/arch/address_space.h */
#define USER_STACK_GUARD_VA \
    (USER_STACK_TOP - ((uint64_t)(USER_STACK_PAGES + 1)) * 0x1000UL)
```

…which is `0x103FFEF000` for the chapter-101 defaults.

The installer is a one-liner once the L3 page is in hand:

```c
/* kernel/arch/address_space.c */
int address_space_install_guard(struct address_space *as, uint64_t va)
{
    if (!as) return -1;
    if ((va & 0xFFFULL) != 0) return -1;
    uint64_t *l3 = l3_for(as, va);
    if (!l3) return -1;
    l3[L3_INDEX(va)] = DESC_SW_GUARD;   /* invalid + tagged */
    __asm__ volatile("dsb ishst" ::: "memory");
    return 0;
}
```

The descriptor written here has `DESC_VALID` clear (the MMU will
fault on access) but `DESC_SW_GUARD` set (the OS knows why). No
physical page is consumed. The L3 table covering the guard is
the same L3 that already covers the user stack — installing the
guard one page below the stack base hits an existing L3 page,
costing literally zero extra memory.

The install fires from the ELF loader, right after the stack
itself is mapped:

```c
/* kernel/core/elf.c, after the stack-mapping loop */
if (address_space_install_guard(as, USER_STACK_GUARD_VA) != 0) {
    serial_puts("[elf] failed to install user-stack guard\n");
    return -1;
}
```

Every user program gets the guard automatically; nothing in
userspace needs to know it exists.

## Reading the descriptor back

To recognise a guard fault we need to walk to the L3 entry that
covers FAR. There was already a static helper for this
(`l3_entry_lookup`), but its signature was `uint64_t *`, returning
a pointer for callers that want to mutate. The fault handler only
wants to *read* the descriptor — and crucially wants to see
invalid entries too. New public function:

```c
/* kernel/arch/address_space.c */
uint64_t address_space_lookup_pte(const struct address_space *as,
                                  uint64_t va)
{
    if (!as) return 0;
    uint64_t l2i    = L2_INDEX(va);
    uint64_t l2_ent = as->l2_va[l2i];
    if ((l2_ent & DESC_VALID) == 0) return 0;
    if ((l2_ent & DESC_TABLE) == 0) return 0;
    uint64_t l3_pa  = l2_ent & ~0xFFFULL & ((1ULL << 48) - 1);
    return ((const uint64_t *)(uintptr_t)l3_pa)[L3_INDEX(va)];
}
```

Returns the raw descriptor word — software bits included,
`DESC_VALID` or not. A return value of `0` means "no L2/L3
covers this address at all" (true unmapped); a non-zero return
with `DESC_VALID=0` could be a guard, or it could be a
descriptor we haven't introduced yet but might in the future.

## The handler check

The existing data-abort dispatch in `svc_dispatch` already
routed EC=0x24 (data abort from a lower EL) through two
existing handlers:

1. `address_space_handle_mmap_fault` — translation fault that
   lands inside an outstanding mmap region. Lazy fault-in.
2. `address_space_handle_cow_fault` — permission fault that
   lands on a `DESC_SW_COW` page. Unshare-and-copy.

Chapter 103 adds a third check, **before either of them:**

```c
/* kernel/core/syscall.c, top of the EC==0x24 branch */
if (ec == 0x24) {
    struct thread *t = thread_current();
    if (t && t->as) {
        uint64_t pte = address_space_lookup_pte(t->as, far);
        if (pte & DESC_SW_GUARD) {
            report_user_stack_overflow(t, far, frame->elr);
            thread_exit(-1);
            /* not reached */
        }
    }
}
```

Order matters. The guard descriptor presents to the MMU as a
translation fault (no `DESC_VALID`, no PA, DFSC `0x04..0x07`).
That fault class is also what the mmap handler resolves. If we
ran the mmap handler first it would walk the vma list, find no
vma covering `0x103FFEF000`, return `-1`, and we'd fall through
to the generic "non-SVC sync exception" dump. Functionally
correct but cosmetically a mess. Checking the SW bit first is
more direct: a guard fault is always a guard fault.

`thread_exit(-1)` is the same path that the existing badpoke /
badptr fault tests already use. It closes fds, posts SIGCHLD,
unblocks the parent's waitpid, and yields. The kernel keeps
running; the shell that ran `stackbomb` gets the next prompt
back without ever knowing it was a guard fault as opposed to
any other kind.

## The diagnostic itself

```c
static void report_user_stack_overflow(struct thread *t,
                                       uint64_t far, uint64_t elr)
{
    const uint64_t stack_top = USER_STACK_TOP;
    const uint64_t stack_bot = stack_top - USER_STACK_PAGES * 0x1000ULL;
    const uint64_t guard_va  = USER_STACK_GUARD_VA;
    const uint64_t overshoot = (far <= stack_bot) ? (stack_bot - far) : 0;

    serial_puts("\n[svc] user stack overflow in thread \"");
    serial_puts(t ? t->name : "(null)");
    serial_puts("\" (pid "); serial_puthex(t ? (uint64_t)t->id : 0);
    serial_puts(")\n");
    /* …FAR, ELR, stack range, guard range, cause…                */
    serial_puts("        likely cause: unbounded recursion, "
                "or one stack frame larger than 64 KiB.\n");
    serial_puts("        thread killed.\n");
}
```

Two design notes worth dwelling on:

**Bytes-overran, not "depth N".** It's tempting to report depth
in *frames* — "you recursed 217 times". We can't, because we
don't know the per-frame stride: depth = (stack used) / (frame
size), and "frame size" varies per function and per call site.
A function with a 16 KiB local frame jumps the guard in one
call but still gets caught; a tight recursion with a 16-byte
frame overshoots by 16 bytes after 1024 frames. Both are
genuine overflows; both deserve the same message. We report the
byte overshoot (`stack_bot − FAR`) because that's the only
number we know for sure, and it tells the reader something
useful — a 0x10-byte overshoot is "one more call would have
fit"; a 0x800-byte overshoot is "a single fat frame."

**No backtrace.** ELR gives the user PC of the faulting
instruction. We don't have unwinders, DWARF, or per-binary
symbol tables in the kernel — those land in a future "ELF
symbol stub" appendix. For now the user can `aarch64-elf-objdump
-d` the ELF and look up the address by hand. That's still a
one-step fix-the-bug workflow from the message, which is what
we wanted.

## Surviving fork

`address_space_clone` (eager) and `address_space_clone_cow`
(lazy) both walk the source AS's L2 and L3 tables and copy
every valid entry. Pre-chapter-101 both functions started their
inner loop with:

```c
if ((src_ent & DESC_VALID) == 0) continue;
```

…which would silently drop the guard descriptor on the floor.
A forked child would get no guard, and its eventual stack
overflow would fall through to the generic message — the very
thing we're trying to avoid. New shape:

```c
for (int j = 0; j < PTES_PER_TABLE; j++) {
    uint64_t src_ent = src_l3[j];
    if ((src_ent & DESC_VALID) == 0) {
        /* Chapter 103 — preserve guard pages across fork. */
        if (src_ent & DESC_SW_GUARD) {
            uint64_t va = USER_VA_BASE
                        + ((uint64_t)i << L2_SHIFT)
                        + ((uint64_t)j << L3_SHIFT);
            if (address_space_install_guard(dst, va) != 0) {
                address_space_destroy(dst);
                return NULL;
            }
        }
        continue;
    }
    /* …existing valid-entry copy logic… */
}
```

Same five lines added in both clone functions. The COW variant
has an additional wrinkle: it mutates `src` as it goes
(downgrading writable pages to RO + SW_COW), so if guard
re-install fails we have to TLBI before returning — the touched
COW marks are harmless in `src` on their own, but the stale TLB
entries pointing at the now-RO pages are not. The eager clone
path doesn't need that, because it never mutates `src`.

## What this unlocks

The infrastructure is reusable. Any future "VA that should be
present in the layout but must fault when touched" can claim
`DESC_SW_GUARD` and inherit:

* the install helper (`address_space_install_guard`);
* the fault-time recognition (the SW_GUARD check is in the
  generic EC=0x24 dispatch — it doesn't know or care that the
  guard happens to be below the user stack);
* the fork survival.

Three near-term consumers:

* **Heap guard.** A guard page between the heap top and the
  bottom of the mmap region would catch `sbrk`-walking-off-end
  bugs the same way.
* **Per-thread stacks.** When `pthread_create`-style threads
  eventually get their own stacks (today's `SYS_CLONE` gives
  callers an mmap-backed stack but no guard), each new stack
  wants its own guard. The same `install_guard` helper drops
  in unchanged; the report function already names *which*
  thread overflowed via `t->name`.
* **Per-shared-library guard regions.** Once we have dynamic
  loading, every loaded library wants a no-execute/no-write
  guard around its `.bss`. The `DESC_SW_GUARD` bit
  generalises: the report function would gain a second case
  ("invalid + GUARD outside the stack range = a library guard
  fault, here's which library").

A more ambitious extension would be a kernel-side
**fault-region table** — a small array of `(va_lo, va_hi,
kind, name)` records consulted by the report function — so the
fault handler can name *every* intentional guard
(stack/heap/library/TLS) with a single registration call. The
SW bit by itself only says "I'm a guard"; a table would say
"I'm the stack guard for thread `notepad`." This chapter
doesn't add that table yet because we only have one guard.
When we have three, it'll be worth the bookkeeping.

## Why this chapter doesn't add "bump on overflow"

A tempting feature is *automatic stack growth*: detect a guard
fault, allocate a new page, install it where the guard used to
be, install a fresh guard one page lower, eret back into the
faulting instruction. The user gets a stack that "magically"
extends to fit, and crashes only on OOM.

Three reasons it's not in this chapter:

1. **It silences the bug.** The whole win of chapter 103 is
   the *message* — turning a forensic problem into a
   single-line answer. Auto-grow turns the same forensic
   problem back into a silent OOM that fires somewhere
   completely unrelated to the actual recursion. The
   browser stack-bump (`USER_STACK_PAGES` 4 → 16, motivated
   by deeply-nested HN comment threads — see repo memory
   `chapter-44-css-table-layout.md`) would have been visible
   immediately under the chapter-101 regime; under auto-grow
   it would have manifested as "the browser is unusably slow"
   or "OOM at random page loads."

2. **The hard cap is the safety net.** Auto-grow needs a cap
   somewhere — even Linux's `RLIMIT_STACK` exists. Without
   one, a runaway recursion just walks the whole user VA range
   before crashing. With one, auto-grow degrades to "same
   diagnostic, but later and with the kernel having allocated
   N pages of physical memory it can never reclaim." Strict
   mode is auto-grow with the cap set to the original stack
   size — strictly more useful for diagnosis.

3. **It's easy to add later.** The descriptor is already
   tagged; the fault handler already runs; the install helper
   already exists. A future chapter that wants auto-grow needs
   `address_space_install_anon_page(va)` plus
   `address_space_install_guard(va − PAGE)` in the handler
   instead of `thread_exit`. ~20 lines.

If the project ever ships a long-running daemon whose stack
needs vary wildly (a JIT, an interpreter), we'll add it as
opt-in per-AS behaviour. For shell-spawned utilities, strict
is the right default.

## Where this hit us, in retrospect

The browser bumped `USER_STACK_PAGES` from 4 to 16 because
`layout_build_subtree` was recursing through deeply-nested
table elements (HN comment threads, ~50 nesting levels). The
symptom: silent fault dumps. The fix was a stack bump
*because the diagnostic was useless* — we didn't know whether
to fix the recursion, grow the stack, or both. Under
chapter 103, the same bug would have produced

```
[svc] user stack overflow in thread "/bin/browser" (pid …)
        ELR_EL1 = 0x… (decoded after the fact: layout_build_subtree+0x…)
        likely cause: unbounded recursion, …
```

…and we'd have known immediately the issue was depth, not
size. (We'd still probably have bumped the stack as a
stopgap, but the *real* fix — making `layout_build_subtree`
iterative — would have been an obvious follow-up rather than
a discovered-much-later refactor.)

That's the general theme. Guard pages don't catch new bugs;
they catch the same bugs *earlier* and *with their context
intact*. A bug caught at the instruction that overflowed the
stack tells you which call site to fix; a bug caught at
"random EL1 fault, kernel halted" tells you nothing.

## Tests

* `userspace/stackbomb/stackbomb.c` — a deliberately infinite
  recursion with a 256-byte volatile local. Suppresses GCC's
  `-Winfinite-recursion` warning locally (we *want* the kernel
  guard to be the one that stops us, not the compiler):

  ```c
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Winfinite-recursion"
  static void recurse(void) { ... recurse(); ... }
  #pragma GCC diagnostic pop
  ```

* `scripts/test_stackbomb.py` — boots the kernel, runs
  `stackbomb` from the shell, asserts that:

  1. the friendly `[svc] user stack overflow` line appears;
  2. the diagnostic carries FAR / ELR / stack / guard /
     DESC_SW_GUARD fields;
  3. neither the old `non-SVC sync exception` dump nor any
     `PANIC` appears;
  4. `stackbomb` never returns from `recurse()` (would mean
     the guard didn't fire);
  5. the shell prompt comes back afterward — the kernel killed
     the offending thread but stayed up.

The first run produced exactly the right diagnostic and we
caught two test-harness mistakes that are worth remembering:

* `read_until` was returning the moment it saw the trigger
  `[svc] user stack overflow`, before the rest of the
  diagnostic finished printing. **Fix:** wait for the *last*
  line of the diagnostic (`thread killed.`) instead. This is
  the general lesson from the user-memory note on
  event-order-vs-render-order: when polling for a multi-stage
  event, key on the last stage, not the first.

* The shell-prompt-came-back check was `read_until(ser,
  [b"$ "], 10.0, prior=log)`, which matched the *boot-time*
  prompt already sitting in `log` and returned instantly
  without ever waiting for the post-overflow prompt. **Fix:**
  drop `prior=` for that read so we only see fresh bytes.

The regression sweep after the changes (mmap, fork+exec,
threads, COW, plus the new stackbomb test) all still pass.

## Files changed

* [kernel/arch/address_space.h](../../../kernel/arch/address_space.h) —
  `USER_STACK_GUARD_VA`, `DESC_SW_GUARD`, declarations for
  `address_space_install_guard` and `address_space_lookup_pte`.
* [kernel/arch/address_space.c](../../../kernel/arch/address_space.c) —
  definitions of the two new functions; comment block for
  `DESC_SW_GUARD`; one extra `if` branch in each of
  `address_space_clone` and `address_space_clone_cow` to
  preserve guards across fork.
* [kernel/core/elf.c](../../../kernel/core/elf.c) — install the
  guard right after the stack is mapped in `elf_load_into_as`.
* [kernel/core/syscall.c](../../../kernel/core/syscall.c) —
  `report_user_stack_overflow` helper and a SW_GUARD check at
  the top of the EC=0x24 branch of `svc_dispatch`.
* [userspace/stackbomb/stackbomb.c](../../../userspace/stackbomb/stackbomb.c)
  — the deliberate test program.
* [Makefile](../../../Makefile) — STACKBOMB build rules + osfs
  inclusion.
* [scripts/test_stackbomb.py](../../../scripts/test_stackbomb.py)
  — regression harness.

Total: ~80 lines of kernel C, ~140 lines of test harness, one
new userspace binary.

## What you'll learn

* A guard page costs almost nothing — one descriptor word, no
  physical memory — and saves hours of diagnostic time.
* AArch64 stage-1 descriptors have four software bits free for
  OS use, and they survive on invalid entries. That's a free
  back-channel between any two pieces of kernel code that
  share an AS.
* The shape of "install an intentional fault, recognise it at
  fault time, name it" generalises far beyond stack guards.
  Heap guards, library guards, TLS guards, and even
  PROT_NONE-style `mprotect` regions all want the same
  pattern.
* When you add a feature to a polling-style test harness,
  it's easy to introduce two classes of bug: needles that
  match too early (use a *terminal* needle for multi-line
  events) and prior-context that matches the test's own
  setup (drop `prior=` when you specifically need fresh
  bytes). Both bit us once and are documented above so the
  next test author doesn't repeat them.

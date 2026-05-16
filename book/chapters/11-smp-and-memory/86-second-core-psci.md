# Chapter 86 — The second core: PSCI and secondary boot

Every chapter up to this one has lived inside the comfortable
fiction that there is exactly one CPU. The kernel touches global
variables without locking; lists get walked while the list head
is mutated; the serial driver writes to the PL011 transmit
register without telling anybody. None of this is correct in
general — it is correct because there is exactly one CPU, and
that CPU is the same CPU whether it is in user mode, kernel
mode, an IRQ handler, or the timer tick. The kernel never has
to ask "is somebody else looking at this right now?" because
nobody else exists.

This chapter ends that fiction. We wake CPU 1.

The actual scheduling work happens in chapter 89. For now, our
goal is the smallest possible "hello, multi-core" milestone:
the second core boots, sets up its MMU, finds its per-CPU
state, prints one line of serial output, and parks in `WFE`
forever. The real value of the chapter is the *infrastructure*
that boot needs — the PSCI ABI, the per-CPU register, the
spinlock primitive — because every later chapter in Part XI
builds on it.

## Prerequisites

- Chapter 6 — MMU and page tables (the secondary inherits the
  boot CPU's L1).
- Chapter 11 — Threads and the AArch64 context switch (we'll
  reuse the stack-pointer discipline in `secondary_start`).
- Chapter 9 — GIC v3 (we don't program the redistributor for
  CPU 1 until chapter 88, but it's worth glancing at).

## What "PSCI" is and why we need it

The bootloader (or in our case, QEMU) drops every CPU into the
kernel's first instruction with the same set of resources, but
only one CPU is allowed to execute by default. The other CPUs
sit in a low-power state — typically `WFI` inside firmware —
waiting for somebody to call them. The mechanism for "somebody
to call them" is the **Power State Coordination Interface**,
PSCI for short.

PSCI is a tiny ABI with a handful of functions:

| Function       | ID         | Purpose                                |
|----------------|------------|----------------------------------------|
| `PSCI_VERSION` | 0x84000000 | Returns the implemented PSCI version.  |
| `CPU_ON`       | 0xC4000003 | Wakes a target CPU at a given EL1 entry|
| `CPU_OFF`      | 0x84000002 | Puts the calling CPU to sleep.         |
| `SYSTEM_OFF`   | 0x84000008 | Powers the whole machine off.          |

The two interesting facts are:

1. The **conduit** — how you make the call — is either `HVC #0`
   (hypervisor call) or `SMC #0` (secure monitor call). On QEMU
   `virt` running under HVF, the firmware lives in the
   hypervisor, so the conduit is `HVC`. The device tree's
   `/psci` node tells us which one to use.
2. The 32-bit `CPU_ON` ID at the top of the table uses the
   "32-bit fast call" SMCCC encoding; we pass it in `x0` and
   the rest of the arguments in `x1`-`x3`. The return value
   comes back in `x0`. Nothing else is touched.

This means PSCI is one of the very few things in the kernel
where "implement the spec" reduces to "set up four registers
and execute one instruction." See `kernel/arch/psci.c`.

## Detecting the conduit

We could just hardcode `HVC` because we know we're on QEMU,
but the device tree already contains the answer and walking
it is six lines of code. The `/psci` node looks like this in
the dump:

```
psci {
    compatible = "arm,psci-1.0", "arm,psci-0.2", "arm,psci";
    method = "hvc";
    cpu_on = <0xc4000003>;
    cpu_off = <0x84000002>;
    cpu_suspend = <0xc4000001>;
    migrate = <0xc4000005>;
};
```

`fdt_read_psci_method()` (in `kernel/core/fdt.c`) walks
`/psci`, finds the `method` property, and returns either
`PSCI_CONDUIT_HVC` or `PSCI_CONDUIT_SMC`. Then `psci_init()`
caches that choice for the rest of the boot, and `psci_call()`
dispatches to either an inline `hvc #0` or `smc #0` based on
it.

## Per-CPU state via `TPIDR_EL1`

Once we have more than one CPU, the question "which CPU am I"
becomes interesting in a way it never was before. There are
three reasonable answers:

1. Read `MPIDR_EL1` and decode `Aff0`. Cheap, but indirect —
   you get a hardware identifier, not a logical index.
2. Use a lock-free per-CPU table indexed by `MPIDR.Aff0`.
   Works, but every per-CPU access becomes "read MPIDR, mask,
   index, dereference."
3. Reserve one general-purpose register (or a dedicated system
   register) to point at the CPU's own private state.

ARMv8 explicitly designed `TPIDR_EL1` for option 3. It is a
64-bit read/write system register, accessible only from EL1,
and the architecture says nothing about its contents — it is
"thread pointer for EL1 software." On AArch64 Linux it points
to the per-CPU area. We do the same.

`smp_init_with_dtb()` programs `TPIDR_EL1` on the boot CPU
once the `g_cpus[]` table is filled in:

```c
__asm__ volatile("msr tpidr_el1, %0"
                 :: "r"(&g_cpus[boot_idx]) : "memory");
```

…and `secondary_start` programs it for each secondary right
before it tail-calls `secondary_main`. From that point onward,
`cpu_current()` is a single instruction:

```c
static inline struct cpu *cpu_current(void)
{
    struct cpu *p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}
```

Anything that wants per-CPU state — the current thread, a
per-CPU runqueue (chapter 89), a per-CPU IPI mailbox (chapter
88), a printk buffer — hangs off this pointer.

## The secondary boot stub

When `psci_cpu_on(mpidr, entry, ctx)` succeeds, the firmware
schedules `entry` to be called on the target CPU at EL1, with
`x0` set to `ctx`. Everything else is undefined. In particular
the secondary has:

- The MMU **off**.
- The vector table **unset**.
- No usable stack.
- A cold cache and TLB.

So before the secondary can call any C code, it has to repeat
the relevant subset of the EL1 init sequence we did in chapter
1. The new entry in `kernel/arch/boot.s` looks like this in
outline:

```asm
.globl secondary_start
secondary_start:
    /* x0 = struct cpu * passed by psci_cpu_on()
     * (the third argument to PSCI CPU_ON is delivered in x0). */
    msr     tpidr_el1, x0           // remember "who am I"
    ldr     x1, [x0, #CPU_OFF_STACK_TOP]
    mov     sp, x1                  // private boot stack

    /* Mirror CPU 0's MMU configuration: same TTBR0_EL1, MAIR,
     * TCR, SCTLR.  Page tables are shared. */
    bl      mmu_secondary_enable

    /* Same vector table as CPU 0. */
    adr     x2, vector_table
    msr     vbar_el1, x2

    /* Off to C. */
    mov     x0, x0                  // (still self pointer)
    b       secondary_main
```

The MMU bring-up is the touchy bit. The boot CPU built the L1
in chapter 6 and installed it via TTBR0_EL1 and SCTLR_EL1.
Those are CPU-local registers, not memory — every secondary
has its own copy and they're zero until we write them. The
trick is that the *page table itself* is shared memory, so as
long as we point the secondary at the same `l1_pgtable` and
copy the boot CPU's MAIR/TCR/SCTLR values, the secondary
inherits the entire address space the moment it turns its
MMU on.

`mmu_secondary_enable` is in `kernel/arch/mmu.S`. It does
exactly the same `dsb sy / isb / msr sctlr_el1, x` dance as
the boot path; the only thing different is that we read the
TCR/MAIR values from globals that the boot CPU stashed away
during its own bring-up.

## Spinlock 101 (because we now need one)

The first global mutable thing the secondary touches is the
serial port. CPU 1's `secondary_main` calls
`serial_puts("[smp] CPU 1 ready\n")` and the boot CPU is
simultaneously printing "[smp] all CPUs online\n". Without
serialization the two strings interleave per-byte and the log
becomes unreadable.

So this chapter also lands the kernel's first lock primitives,
in `kernel/arch/spinlock.h`. There are two of them.

The flat `spinlock_t` is the canonical AArch64 ticketless
test-and-set. Acquire is a load-acquire (`ldaxr`); release is
a store-release (`stlr`). The pattern:

```c
spin_lock(spinlock_t *l):
1:  ldaxr   w0, [l]
    cbnz    w0, 1b          // somebody owns it; spin
    mov     w1, #1
    stxr    w0, w1, [l]
    cbnz    w0, 1b          // race lost; retry
    // memory barrier baked into ldaxr — we're in.

spin_unlock(spinlock_t *l):
    stlr    wzr, [l]        // store-release
```

`ldaxr` reserves the cache line. `stxr` only succeeds if the
reservation is still held; if another CPU touched the line,
`stxr` returns nonzero and we retry. The read-side fast path
is entirely atomic-free as long as the lock is held — we just
spin in the `cbnz`. ARM's exclusive-monitor hardware is doing
the work for us.

The recursive `reclock_t` adds a CPU-id field and a depth
counter so the same CPU can re-enter the lock without
deadlocking. We need this for the serial driver because
`serial_puthex()` internally calls `serial_putc()` many times
to emit each hex nibble — if `puthex` took the flat lock and
then `putc` tried to take it again, we'd deadlock against
ourselves. The recursive variant lets us wrap the *outer*
operation (`puthex` or `puts`) in one lock-acquire so the
whole printed string is atomic across CPUs.

Both variants disable IRQs while held (`*_irqsave`). That's
the price you pay for ever wanting an IRQ handler to take the
lock — if the outer code held it without masking, the IRQ
could fire mid-critical-section and try to re-acquire,
deadlocking the same CPU against itself. Today no IRQ handler
prints from inside a critical section, but we want the policy
robust against future code.

## Wiring it into the boot

`smp_init_with_dtb()` (in `kernel/arch/cpu.c`) is the entire
choreography:

1. `psci_init(dtb)` — pick HVC vs SMC.
2. `fdt_read_cpus(dtb, mpidrs, …)` — enumerate `/cpus` from
   the device tree. On `-smp 2` this finds two entries with
   MPIDR `0x0` and `0x1`.
3. Populate `g_cpus[i]` with `cpu_id`, `mpidr`, and a
   per-CPU `stack_top` from the linker's
   `secondary_stack_top_<i>` symbols.
4. Find the boot CPU's index by matching its own `MPIDR_EL1`
   against the table.
5. Set `TPIDR_EL1` on the boot CPU; mark it `READY`.
6. For each non-boot CPU: `psci_cpu_on(mpidr, &secondary_start,
   &g_cpus[i])`. Mark `BOOTED` on success; log the rejection
   on failure.
7. Spin-wait for every `BOOTED` CPU to flip its `READY` flag,
   with a timeout that warns but doesn't abort.

The order in `kernel_main` matters: we call `smp_init_with_dtb`
*after* `thread_init` so the per-CPU stacks are already in
`.bss`, but *before* the VFS comes up so the `[smp]` log block
sits cleanly between the memory/heap setup and the device
probe section. Since the secondaries do nothing but park in
WFE today, the timing is otherwise unconstrained.

## Booting it

```
$ make run-graphical                # Makefile passes -smp 2
…
[smp] bringing up additional cores ...
[smp] DTB reports 0x2 cpu(s)
[smp] PSCI CPU_ON cpu=1 mpidr=0x0000000000000001 entry=0x000000004000093c
[smp] CPU 1 ready (mpidr = 0x0000000000000001)
[smp] waiting for secondaries to report ready ...
[smp] all CPUs online
…
```

That's the whole chapter.

## When things don't work

Two failure modes we hit during bring-up. Both kept us
honest.

### `-smp 1` mismatch

Our `assets/virt.dtb` is baked with `-smp 2` so the kernel
always sees two `/cpus` entries — but the existing harness
scripts launch QEMU without an explicit `-smp` argument,
which means `-smp 1`. The kernel dutifully calls
`psci_cpu_on(cpu_id=1, mpidr=0x1, …)` and PSCI returns a
"denied" status because there is no second CPU.

The fix is just to log the denial and continue: a missing
secondary is not a kernel bug, it's a launch configuration
quirk. But the *choice of word* matters — the first version
of `cpu.c` printed `"FAILED rc=…"` and several test scripts
greped the kernel log for the substring `"FAIL"` to detect
userspace-test failures. That made every existing regression
test go red the moment we landed chapter 86. The line now
prints `"denied rc=…"`, mirroring PSCI's own term, and the
suite goes back to green.

The wider lesson: the kernel log is a public API to the test
harness. Choose words that won't collide with overloaded
substrings like `FAIL`, `PANIC`, `FATAL`.

### The serial-lock IRQ trap

Once the second CPU is awake, it can call `serial_puts` from
its own context. If we'd left `serial_putc` lock-free, two
boot lines would interleave per-byte. Easy.

The non-obvious bit was the IRQ policy. The first version of
the locked driver took the lock without masking IRQs. Boot
worked. The first time a virtio-net IRQ fired during the
boot's `[net] self-test:` block, the IRQ handler called
`serial_puts` from *inside* a critical section CPU 0 was
already inside, hit a `reclock_lock` that would have been
satisfied by the recursion fast-path *if the IRQ had been
on the same CPU*, and… also was on the same CPU. So it
worked. But the moment the symmetric scenario flipped — IRQ
fires during a `serial_puthex` on a *different* CPU once
chapter 88 lands — the IRQ handler would spin forever waiting
for the printing CPU to release.

The defense is to mask IRQs while the lock is held. That
means an IRQ that wants to print sits as pending in the GIC
distributor for a few microseconds — the duration of a
`serial_puts` line — instead of being delivered immediately.
For a kernel where IRQ handlers print at most a few characters
on errors, that's invisible. For a kernel that printed at IRQ
rate, it would matter and we'd want a per-CPU lockless ring
buffer.

## What this unlocks

- **Chapter 87** — atomics and locks vocabulary. Now that we
  have a recursive spinlock we can audit every kernel global,
  classify it as "owner-thread-only" or "shared," and lock
  the shared ones.
- **Chapter 88** — IPIs via GIC SGIs. The first thing we'll
  use them for is a "halt all on panic" so a panic on either
  CPU stops the other.
- **Chapter 89** — the SMP scheduler. Either a shared
  runqueue with a per-runqueue spinlock, or per-CPU runqueues
  with work stealing. Until then, every userspace thread
  still runs on CPU 0 and CPU 1 just sits in `WFE`.

## Build & test

```
$ make                      # passes -smp 2 by default
$ python3 scripts/_dbg_smp_boot.py
…
OK   [smp] bringing up additional cores
OK   [smp] DTB reports 2 cpu(s)
OK   [smp] PSCI CPU_ON cpu=1
OK   [smp] CPU 1 ready
OK   [smp] all CPUs online
```

The full single-CPU regression suite still passes — every
existing test launches QEMU without `-smp`, exercises the
"DTB lists 2 CPUs but only 1 exists" denial path, and lands
the same userspace behaviour as before chapter 86.

## Files added or changed

- `kernel/arch/cpu.{h,c}` — `struct cpu`, `g_cpus[]`,
  `smp_init_with_dtb`, `secondary_main`.
- `kernel/arch/psci.{h,c}` — HVC/SMC dispatch, `CPU_ON`
  wrapper.
- `kernel/arch/spinlock.h` — flat `spinlock_t` and recursive
  `reclock_t`, both with `_irqsave` variants.
- `kernel/arch/boot.s` — `secondary_start` trampoline.
- `kernel/core/fdt.{h,c}` — `fdt_read_psci_method` and
  `fdt_read_cpus` walkers.
- `kernel/core/serial.c` — every public function now wraps
  its body in `reclock_lock_irqsave`.
- `kernel/core/main.c` — call `smp_init_with_dtb` after
  `thread_init`.
- `linker/kernel.ld` — three 16 KiB secondary stacks.
- `scripts/build_dtb.sh` — bake DTB with `-smp 2`.
- `Makefile` — `-smp 2` (`QEMU_SMP=2`) on all `qemu` recipes.
- `scripts/_dbg_smp_boot.py` — chapter 86 smoke test.

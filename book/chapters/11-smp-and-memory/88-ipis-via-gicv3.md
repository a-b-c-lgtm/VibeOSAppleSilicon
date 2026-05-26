# Chapter 88 — IPIs via GICv3 SGIs

Chapter 86 brought up CPU 1 and parked it in WFE. Chapter 87
gave both CPUs the atomic vocabulary they need to share data.
Now they need a way to **talk to each other**: "wake up, you
have work" or "stop now, the system is going down."

ARM gives us exactly the right primitive: 16 SGI IDs (Software
Generated Interrupts) per CPU, each fired by writing the
`ICC_SGI1R_EL1` system register on any CPU and delivered via
the GICv3 redistributor as a normal IRQ on the target CPU.

This chapter wires SGIs up end-to-end and proves the round-trip
works with a smoke test: at the end of `smp_init`, CPU 0 sends
`IPI_PING` to every secondary and verifies the per-CPU receive
counter bumps within microseconds.

## Prerequisites

- Chapter 9 — GIC v3 distributor and CPU interface basics.
- Chapter 86 — secondary CPU bring-up via PSCI.
- Chapter 87 — atomics (the per-CPU receive counters).

## What an IPI actually is on ARMv8

On x86 you'd send an IPI by writing the local APIC's ICR with
a destination CPU and a vector number. On ARMv8 the equivalent
is `ICC_SGI1R_EL1`, a 64-bit system register where the bit
fields encode:

```
[63:56] Aff3        (cluster level 3)
[55:48] reserved
[47:44] RS          range selector for >16 CPUs
[43:41] reserved
[40]    IRM         interrupt routing mode (0 = list, 1 = "all but me")
[39:32] Aff2        cluster level 2
[31:28] reserved
[27:24] INTID       the SGI ID — 0..15
[23:16] Aff1        cluster level 1
[15:0]  TargetList  bitmap of Aff0s in this Aff{1,2,3} cluster
```

On QEMU virt with ≤8 CPUs every CPU lives in cluster 0/0/0
and `Aff0 == cpu_id`, so "send IPI vector V to CPU N" is just:

```
write ICC_SGI1R_EL1 = (V << 24) | (1 << N)
```

Broadcasting to "everyone but me" is even simpler — set bit
40 (IRM) and the GIC distributor enumerates the cluster on
your behalf:

```
write ICC_SGI1R_EL1 = (V << 24) | (1 << 40)
```

The receiver's CPU interface raises a normal IRQ. From the
kernel's perspective an SGI is indistinguishable from a PPI
or SPI: `ICC_IAR1_EL1` returns the SGI's INTID (0..15), and
`ICC_EOIR1_EL1` ends it. The only thing that flags it as an
IPI is that intid < 16.

## Per-CPU GIC init

Up to chapter 87 our `gic_init` was hardcoded for "the boot
CPU's redistributor at 0x080A_0000". That's CPU 0's frame.
Each secondary has its own redistributor frame at
`0x080A_0000 + (cpu_id * 128 KiB)`, and **each CPU has to
wake its own** by clearing `GICR_WAKER.ProcessorSleep` from
its own context. (Trying to wake CPU 1's redistributor from
CPU 0 doesn't work — the operation has to come from a CPU
running with that redistributor's MPIDR.)

The refactor splits `gic_init` into two pieces:

- **`distributor_init()`** — quiesce the distributor, mark
  every SPI as group-1, set default priorities, then
  re-enable with `ARE_NS | ENABLE_G1NS`. Called exactly once,
  from CPU 0, before any secondary wakes.

- **`gic_init_per_cpu()`** — wake this CPU's redistributor
  (computed from MPIDR.Aff0), set every SGI/PPI as group-1,
  disable them all by default, set default priorities, then
  enable the CPU interface system regs (`ICC_SRE_EL1`,
  `ICC_PMR_EL1`, `ICC_BPR1_EL1`, `ICC_IGRPEN1_EL1`). Called
  by the boot CPU as part of `gic_init()`, and by each
  secondary at the top of `secondary_main()`.

`gic_enable_irq` was also touched: for IDs < 32 (SGI/PPI) it
now writes the **current** CPU's redistributor instead of a
hardcoded one. So a secondary that calls `gic_enable_irq(0)`
enables IPI_PING in *its own* redistributor; CPU 0 calling
the same function would enable it in CPU 0's redistributor.
Per-CPU SGI/PPI enables are *per-CPU*, by GICv3 design.

## The IPI vocabulary: `kernel/arch/ipi.h`

Two vectors ship in chapter 88:

| ID | Vector      | Sender → Receiver behaviour                  |
|----|-------------|----------------------------------------------|
| 0  | `IPI_PING`  | Receiver bumps a per-CPU atomic counter; used by the smoke test |
| 1  | `IPI_HALT`  | Receiver masks IRQs and enters WFI forever; wired but not yet triggered |

The API:

```c
void ipi_send(uint32_t target_cpu, uint32_t ipi_id);
void ipi_broadcast_others(uint32_t ipi_id);
int  ipi_handle(uint32_t intid);     // called from irq_dispatch
void ipi_smoke_test(void);           // called from smp_init_with_dtb
```

`ipi_handle` is invoked from `irq_dispatch` whenever the
acknowledged intid is < 16. It's a small switch on the
ipi_id, runs the per-vector handler, and returns whether
the caller should reschedule. (Today that's always 0;
chapter 89's `IPI_RESCHED` will be the first to return 1.)

The handlers themselves are deliberately tiny — IRQ context
isn't where you do work:

```c
static void handle_ping(void) {
    uint32_t cpu = cpu_current_id();
    if (cpu < SMP_MAX_CPUS)
        atomic_add_return64(&g_ipi_ping_count[cpu], 1);
}

static void handle_halt(void) {
    serial_puts("[smp] CPU N halted via IPI\n");
    asm("msr daifset, #2");      // mask IRQs
    for (;;) asm("wfi");
}
```

Note `handle_halt` never returns. That's a deliberate
exception to the normal IRQ contract — `irq_dispatch` would
EOI on its way out, but here the EOI is dead code on the
halting CPU. The running-priority on the CPU interface stays
elevated, but no further IRQs will ever be acknowledged on
this CPU anyway.

## Design choice — one SGI per vector, no mailbox

Linux uses a per-CPU `volatile uint32_t pending_ipis` bitmap
where the sender does `atomic_or(&target->pending_ipis, 1<<V)`
before firing a single "wake up" SGI, and the receiver
loop-and-clears the bits. The benefit: two rapid RESCHED IPIs
collapse into one delivery (the second `or` is a no-op on an
already-set bit, no second SGI gets fired).

We don't need that yet:

- 4 CPUs maximum.
- IPI rate today is "approximately zero" (one ping per
  secondary at boot).
- Future RESCHED rate (chapter 89) will be at most a few
  per millisecond.

So chapter 88 ships the simpler scheme: **one SGI ID per
vector**. The receiver just reads `intid` from `ICC_IAR1_EL1`
and dispatches on it. If we ever measure SGI-storm pressure
in chapter 89 we can switch to the mailbox model — the
public `ipi_send`/`ipi_broadcast_others` API stays the same.

## Wake-from-WFI vs WFE

Chapter 86's secondary parked in `wfe`. That was wrong-shaped
for chapter 88: WFE only wakes on an explicit SEV (Send Event)
or on the local event register being set, and the GIC does
**not** automatically `SEV` on IRQ delivery. So a `wfe`
secondary stays asleep when the IPI arrives. (We'd see the
PING counter never bump -- exactly the failure mode the
docs warn about. The fix is to read the docs first.)

The fix in `secondary_main` is two characters:

```c
-    for (;;) __asm__ volatile("wfe");
+    for (;;) __asm__ volatile("wfi");
```

WFI ("Wait For Interrupt") sleeps until any pending IRQ
arrives at the CPU interface, regardless of whether
`daif.I` would currently mask it. Even masked IRQs wake the
CPU; they just don't get delivered until DAIF is cleared.

## The SMP smoke test

Choreography (matches the chapter 87 smoke pattern):

1. **CPU 0**, late in `smp_init_with_dtb`, after every
   secondary is READY and after `smp_smoke_verify` passed,
   calls `ipi_smoke_test()`.

2. For each BOOTED+READY secondary, CPU 0:
   - Snapshots `g_ipi_ping_count[cpu]` (call it `before`).
   - Calls `ipi_send(cpu, IPI_PING)`.
   - Spin-waits up to ~50 ms for `g_ipi_ping_count[cpu] >
     before`.

3. **CPU N** (a secondary), now sitting in `wfi` with IRQs
   unmasked, takes the SGI as a normal IRQ. `irq_dispatch`
   calls `ipi_handle(0)`, which calls `handle_ping`, which
   bumps `g_ipi_ping_count[cpu]` atomically.

4. CPU 0 sees the bump, prints `[smp-ipi] cpu=N OK round-trip`,
   moves on. After all secondaries pass, prints `[smp-ipi]
   all OK`.

The boot log on `-smp 2`:

```
[smp] PSCI CPU_ON cpu=1 mpidr=0x...1 entry=0x...
[smp] waiting for secondaries to report ready ...
[smp] CPU 1 ready (mpidr = 0x...1)
[smp] all CPUs online
[smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
[smp-ipi] cpu=1 OK round-trip
[smp-ipi] all OK
```

Like the chapter 87 smoke, this runs on every boot — and
therefore on every regression test. If the GIC routing breaks
(say, because we mis-compute the redistributor base for a
new CPU), it surfaces as `MISS (no ack within timeout)` on
the boot log instead of as a mysterious lost interrupt
weeks later.

## What we did NOT do this chapter

- **No `IPI_RESCHED`.** It's the natural next vector but
  pointless until chapter 89 builds a runqueue that CPU 1
  can be kicked into.
- **No TLB shootdown.** Pointless until two CPUs share an
  address space (chapter 89 + the eventual mmap chapter).
  When it does land, it'll be `IPI_TLB`: sender fills a
  per-CPU shootdown descriptor, fires the IPI, spin-waits
  for an ack bit. The acknowledgement-handshake is the
  hard part; the IPI mechanism itself is already done.
- **No `panic_halt_others()` call from the panic handler.**
  Today a CPU-0 panic leaves CPU 1 sitting in WFI doing
  nothing harmful — it has no work yet, no in-flight state
  to corrupt. Once chapter 89 has CPU 1 actively running
  threads, the very first thing the panic handler will do
  is `ipi_broadcast_others(IPI_HALT)`. The mechanism is
  ready; the call site is deferred so we don't add an
  untested code path to the most safety-critical function
  in the kernel.
- **No IPI mailbox / coalescing.** As above.
- **No RS / >16 CPU support.** `SMP_MAX_CPUS = 4`; we'd
  rather rebuild than spec for a hypothetical big.LITTLE
  topology.

## When things don't work

**Trap: forgot to wake the secondary's redistributor.**
If `gic_init_per_cpu()` isn't called on CPU N, every IPI
sent to CPU N is silently dropped — the GIC distributor
queues the SGI internally but never gets to deliver it
because the redistributor reports ProcessorSleep. The
symptom is `[smp-ipi] cpu=N MISS`; the fix is to verify
the secondary called `gic_init_per_cpu()` before unmasking
IRQs.

**Trap: WFE instead of WFI on the secondary.**
The secondary boots, enables IRQs, sets READY, and parks
in WFE. CPU 0 sends the PING. The SGI delivers cleanly to
the CPU interface, but WFE doesn't wake on IRQ arrival —
only on SEV — so the secondary keeps sleeping until either
a stray SEV (none ever comes) or a context-switching
attempt clears the wait. Symptom is identical to the
"redistributor asleep" case (`MISS`); diagnosis is to
check whether the IPI handler ever ran (g_ipi_ping_count
stays at 0) versus whether it ran but nobody noticed
(g_ipi_ping_count went up but the spin-wait timed out
anyway — which would only happen if the smoke test itself
was buggy).

**Trap: per-CPU register state on the wrong CPU.**
`gic_enable_irq(IPI_PING)` from CPU 0 enables PING in
**CPU 0's** redistributor, not the secondary's. The "let
me set everything up from CPU 0 and then start CPU 1"
pattern that works for purely shared state (distributor)
does NOT work for per-CPU state (redistributor SGI/PPI
enables, CPU interface regs). Each secondary must enable
its own SGIs from its own context. The split into
`gic_init` (boot CPU full + distributor) versus
`gic_init_per_cpu` (any CPU, redistributor-only) makes
this hard to get wrong: secondaries call the latter, only.

**Trap: barrier ordering on the sender side.**
A store to a shared variable followed by `ipi_send` is a
classic publish: the receiver expects to see the new value
of the variable when it runs the IPI handler. That requires
a `dmb ishst` (or stronger) **between the store and the
SGI register write**, otherwise the GIC may deliver the IPI
before the store reaches global visibility. `ipi_send`
already does this internally (`dsb ishst` before
`msr ICC_SGI1R_EL1`), so callers don't need to add their
own — but keep the pattern in mind when ipi_send grows
caller-side preconditions in chapter 89.

## Files added or changed

- **`kernel/arch/ipi.h`** *(new)* — SGI vector enum, public
  IPI API.
- **`kernel/arch/ipi.c`** *(new)* — `ipi_send`,
  `ipi_broadcast_others`, `ipi_handle`, `ipi_smoke_test`,
  per-CPU receive counters.
- **`kernel/device/gic.h`** — added `gic_init_per_cpu()`;
  rewrote header comment to describe the per-CPU GICR layout.
- **`kernel/device/gic.c`** — split `gic_init` into
  `distributor_init` + `gic_init_per_cpu`; redistributor
  base is computed from MPIDR.Aff0 instead of hardcoded.
  `gic_enable_irq` for IDs < 32 now uses the current CPU's
  redistributor.
- **`kernel/core/irq.c`** — IDs < 16 dispatch through
  `ipi_handle` instead of falling through to the catch-all.
- **`kernel/arch/cpu.c`** — `secondary_main` calls
  `gic_init_per_cpu()`, enables PING + HALT SGIs, unmasks
  IRQs, then `wfi` (was `wfe`). `smp_init_with_dtb` calls
  `ipi_smoke_test()` after `smp_smoke_verify`.
- **`Makefile`** — added `kernel/arch/ipi.c` to `C_SRCS`.
- **`scripts/_dbg_smp_boot.py`** — added two new required
  markers (`[smp-ipi] cpu=1 OK round-trip` and
  `[smp-ipi] all OK`).

## Build & test

```
$ make all
$ python3 scripts/_dbg_smp_boot.py
…
[smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
[smp-ipi] cpu=1 OK round-trip
[smp-ipi] all OK
…
OK   [smp] bringing up additional cores
OK   [smp] DTB reports 2 cpu(s)
OK   [smp] PSCI CPU_ON cpu=1
OK   [smp] CPU 1 ready
OK   [smp] all CPUs online
OK   [smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
OK   [smp-ipi] cpu=1 OK round-trip
OK   [smp-ipi] all OK
```

The single-CPU regression suite (24 tests) still passes —
which is the real test, because each of those tests boots
under `-smp 2` and would catch any regression in the per-CPU
GIC bring-up.

## What this unlocks

- **Chapter 89 — SMP scheduler.** Now that we can wake a
  sleeping CPU, we can finally put threads on it.
  `IPI_RESCHED` becomes the third vector; the runqueue,
  per-CPU `current`, and per-CPU thread tables become real.
- **Future panic-halt-others.** The panic path will
  `ipi_broadcast_others(IPI_HALT)` before printing its
  register dump, so the dump on CPU 0 reflects state from
  a moment when no other CPU is mutating shared kernel
  data. Today this is unnecessary (CPU 1 is in WFI); after
  chapter 89 it becomes essential.
- **Future TLB shootdown.** `IPI_TLB` will be vector 3.
  Sender fills `g_shootdown_va` (a per-CPU descriptor),
  sets the target's pending bit, fires the IPI, spin-waits
  for an ack bit. Receiver issues `tlbi vae1` for the VA,
  sets the ack bit. The mechanism we need exists; only
  the descriptor and ack-bit protocol remains.

# Chapter 10 — The ARM generic timer

> **Milestone in this chapter:** 2 — GIC v3 + generic timer.
> **Code referenced:** [kernel/core/timer.c](../../../kernel/core/timer.c),
> [kernel/core/timer.h](../../../kernel/core/timer.h),
> [kernel/core/main.c](../../../kernel/core/main.c) (init + heartbeat),
> [kernel/core/irq.c](../../../kernel/core/irq.c) (re-arm in dispatch).
>
> **At the end of this chapter** you will have a kernel that takes
> a real interrupt every 100 ms, increments a tick counter, and
> prints a heartbeat every ten ticks. The timer is the only IRQ
> source in milestone 2; chapter 11 reuses it as the preemption
> source for the scheduler.

## Why we get the timer for free

Every AArch64 implementation is required to ship the *generic
timer* — a per-CPU block of registers driven by a system-wide
counter. There are several variants visible to software:

| Timer name           | Where it lives | Who can program it |
|----------------------|----------------|---------------------|
| EL1 physical (CNTP)  | per-CPU        | EL1 (and EL2/EL3 always) |
| EL1 virtual (CNTV)   | per-CPU        | EL1 (always)              |
| Hypervisor (CNTHP)   | per-CPU        | EL2 only                  |
| Secure (CNTPS)       | per-CPU        | EL3 only                  |

In a bare-metal or secure-boot environment EL1 software typically
uses CNTP (the *physical* timer). Under a hypervisor, however,
EL2 owns CNTP — `CNTHCTL_EL2.{EL1PCEN, EL1PCTEN}` gate EL1's
ability to even read or write the CNTP_*_EL0 registers, and
hypervisors usually leave those gates closed.

That is exactly the situation we are in. Apple's Hypervisor
Framework is the EL2 between us and the silicon, and it does not
let us touch CNTP directly. Writing `CNTP_TVAL_EL0` from EL1
under HVF traps to EL2 with a fault that surfaces as
`ESR.EC = 0x00` ("Unknown reason"). Chapter 5 has the panic
template if you want to see what that looks like.

The fix is not to fight the hypervisor — it is to use the
*virtual* timer (CNTV) instead. The virtual timer is precisely
what hypervisors expose to guests; it ticks at the same rate as
CNTP (CNTFRQ_EL0 reports the same value for both), and it is
delivered to the guest as PPI 27 instead of PPI 30. Programming
it from EL1 under HVF "just works".

| Timer | Programming registers       | Counter           | INTID on virt |
|-------|-----------------------------|-------------------|----------------|
| CNTP  | CNTP_TVAL_EL0, CNTP_CTL_EL0 | CNTPCT_EL0        | 30 (untouchable under HVF) |
| CNTV  | CNTV_TVAL_EL0, CNTV_CTL_EL0 | CNTVCT_EL0        | 27 (the one we use) |

The book deliberately uses CNTV throughout because *every* future
chapter assumes we run as a guest under HVF.

## The frequency register

`CNTFRQ_EL0` reports the counter rate in Hertz. Some
implementations leave it RAZ until firmware programs it; Apple
silicon under HVF reports `24_000_000` (24 MHz) for the virtual
timer. TCG with `-cpu cortex-a72` reports `62_500_000` (62.5 MHz).
Both values are sane; both are read at runtime so the same code
works under both accelerators.

```c
uint64_t freq = cntfrq_el0_read();   // Hz
```

To convert milliseconds to ticks while staying in 64-bit integer
arithmetic without losing precision on small intervals, we
re-order the multiplication:

```c
g_interval_ticks = (uint32_t)((freq * interval_ms) / 1000ULL);
```

For 24 MHz × 100 ms / 1000 = 2_400_000 ticks per fire. Plenty of
headroom in 32 bits.

## The two control registers

The per-EL timer block exposes two writable registers and one
read-only counter:

- **`CNTV_TVAL_EL0`** — *time value*, a signed 32-bit down-
  counter. Writing it sets the current value; the hardware
  decrements it once per tick of the counter. When it crosses
  zero (becomes negative), `CNTV_CTL_EL0.ISTATUS` goes high and,
  if the timer is enabled and not masked, the corresponding PPI
  fires.
- **`CNTV_CTL_EL0`** — control bits in the low three bits:

  | Bit | Name    | Meaning                                          |
  |-----|---------|--------------------------------------------------|
  | 0   | ENABLE  | Timer counts when set                            |
  | 1   | IMASK   | Mask the interrupt output (does not stop counting) |
  | 2   | ISTATUS | Read-only: 1 if timer condition met              |

- **`CNTVCT_EL0`** — the 64-bit free-running virtual counter,
  read-only. Useful for delta measurements; we will lean on it
  in chapter 11 for thread runtime accounting.

## `timer_init`

`timer_init` does three things, in order:

1. Read `CNTFRQ_EL0` to discover the counter rate.
2. Compute `g_interval_ticks` for the requested period.
3. Arm the timer by writing `CNTV_TVAL_EL0` and then setting
   `ENABLE = 1` (and `IMASK = 0`) in `CNTV_CTL_EL0`.

```c
void timer_init(uint32_t interval_ms)
{
    uint64_t freq = cntfrq_el0_read();
    g_interval_ticks = (uint32_t)((freq * interval_ms) / 1000ULL);

    cntv_tval_el0_write(g_interval_ticks);
    cntv_ctl_el0_write(1ULL);   // ENABLE=1, IMASK=0
}
```

The `isb` after `cntv_ctl_el0_write` (inside the inline-asm
helper) is required: without it the *next* instruction may not
see the timer enabled, and an early read of CNTV_CTL_EL0 would
race the write.

## The IRQ path end-to-end

Putting chapter 9 and chapter 10 together, the chain that runs
every 100 ms is:

1. The virtual counter advances from `0xN` to `0xN + 1`.
2. The timer's downcounter decrements from `g_interval_ticks` to
   `g_interval_ticks - 1`. This continues for 100 ms of wall time.
3. The downcounter goes negative; `CNTV_CTL_EL0.ISTATUS` rises;
   the timer asserts PPI 27 to the GIC redistributor.
4. The GIC distributor and CPU interface forward it: the IRQ
   becomes pending at the CPU interface, with priority `0x80`
   (set by `gic_set_priority` in `kernel_main`).
5. PSTATE.I is clear (we ran `msr daifclr, #2`), so the CPU takes
   the IRQ exception. It branches through `VBAR_EL1 + 0x280`
   (chapter 5 vector table slot 5).
6. Slot 5 is `b irq_entry` (chapter 9 patched it from `b
   panic_entry`).
7. `irq_entry` runs `save_context`, calls
   `irq_dispatch(struct exception_frame *)`.
8. `irq_dispatch`:
   - reads `ICC_IAR1_EL1` → `intid = 27`
   - the switch matches `TIMER_CNTV_INTID`
   - `timer_rearm()` writes `CNTV_TVAL_EL0` again with
     `g_interval_ticks` so the next fire is exactly 100 ms from
     *now* (drift-free against the counter, only against wall
     clock by the dispatch latency)
   - `timer_tick()` increments `g_ticks`
   - `gic_end_of_irq(27)` writes `ICC_EOIR1_EL1`
9. `irq_entry` runs `restore_context` and `eret`s. PSTATE is
   restored (I bit goes back to 0 since it was clear at the time
   of the exception), and `kernel_main` resumes inside its
   `wfe` instruction.
10. The next `wfe` returns when an event arrives (which any IRQ
    delivery will produce); `kernel_main` checks
    `timer_ticks()` and prints a heartbeat every ten ticks.

That last point is worth dwelling on. `wfe` ("wait for event")
is the AArch64 idiomatic way to halt a CPU until *something*
happens. IRQs implicitly send an event to the executing CPU on
delivery, so `wfe` is the right primitive to pair with our IRQ
loop:

```c
for (;;) {
    __asm__ volatile("wfe");
    uint64_t now = timer_ticks();
    if (now - last_heartbeat >= HEARTBEAT_TICKS) {
        last_heartbeat = now;
        serial_puts("[tick] count = ");
        serial_puthex(now);
        serial_puts("\n");
    }
}
```

Under HVF this pattern uses essentially zero host CPU between
ticks; the host scheduler suspends our vCPU thread while the
guest is in `wfe`, and resumes it only when the timer fires.

## What you should see

```text
$ make run
Running under HVF — Ctrl-A X to quit.

============================================================
osdev aarch64 — milestone 2 (GIC v3 + generic timer)
============================================================
dtb_phys = 0x0000000000000000
initialising GIC v3 ... ok
priming generic timer for 100 ms ticks ... ok
unmasking IRQs in PSTATE ... ok

entering wfe loop; heartbeat every 0x00000000000003e8 ms (Ctrl-A X to quit QEMU)

[tick] count = 0x000000000000000a
[tick] count = 0x0000000000000014
[tick] count = 0x000000000000001e
```

Three heartbeats in three seconds, exactly. `0x3e8 ms = 1000 ms`.
Each heartbeat advances the tick count by `0xa = 10`, which is
ten 100-ms ticks.

If you see the banner and then silence, the most likely culprits
in order:

1. You forgot `gic_enable_irq(TIMER_CNTV_INTID)`. The IRQ is
   pending at the redistributor but not enabled.
2. You forgot `msr daifclr, #2`. The IRQ is delivered to the CPU
   interface but blocked by `PSTATE.I`.
3. You forgot `gic_end_of_irq(intid)` in the dispatcher. The
   first tick fires but the running priority on the CPU
   interface stays elevated, blocking subsequent timer IRQs at
   the same priority.

If you see exactly one heartbeat and then silence, it is almost
always #3.

## What chapter 11 adds

We now have a free-running tick. The next conceptual leap is to
use that tick to switch *between* multiple flows of execution —
threads. Chapter 11 introduces `struct thread`, the AArch64
context-switch routine `cswitch` (it lives in
`kernel/arch/context_switch.S` once we add it), and the
cooperative `yield()` primitive. Chapter 12 makes the timer
preempt those threads automatically.

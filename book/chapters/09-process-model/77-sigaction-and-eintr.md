# Chapter 77 — Catching signals: sigaction, masks, EINTR

> **Milestone in this chapter:** add `sigaction` + the return-to-
> user trampoline so processes can catch signals. Mask handling
> and `EINTR` semantics are deferred to a later chapter.
> **Code referenced:**
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`sys_sigaction`, the saved trap frame, the sigreturn
>   trampoline)
> - [userspace/libc/signal.h](../../../userspace/libc/signal.h)
>
> **At the end of this chapter** you will have userspace able
> to register handlers via `sigaction(2)`, with the kernel
> entering the handler on delivery and `sigreturn` resuming
> the interrupted context. Builds on chapter 76 (signal
> delivery).
also *catchable* — a process can register a handler that
runs on delivery. That requires a tiny return-to-user
trampoline, a saved trap frame, and a decision about what
in-progress syscalls do when interrupted.

## What this chapter adds

- `SYS_SIGACTION(signo, &handler, &old)` — register/replace.
- `SYS_SIGRETURN` — restore the pre-signal trap frame.
- A signal-delivery trampoline injected into the user stack:
  push old frame → call handler → SYS_SIGRETURN.
- Per-thread blocked-signal mask + `SYS_SIGPROCMASK`.
- Slow syscalls (read on a pipe, recv on a socket) return
  -EINTR if a signal interrupts them; the libc wraps the
  classic restart loop.

## Prerequisites

- Chapter 76 — Signals, SIGINT
- Chapter 14 — SVC and the syscall ABI
- Chapter 27 — User stack layout (we extend it for the
  signal frame)

## Plan

- Picking a signal-frame layout: trap-frame replica + signo +
  return address pointing at the SYS_SIGRETURN stub.
- Where the trampoline lives: emit it once into a known
  per-AS page (similar to vDSO).
- Mask discipline: handler runs with its own signal masked
  to prevent recursion.
- The EINTR debate: SA_RESTART vs return-EINTR. We pick
  return-EINTR for simplicity and let the libc retry.

## What you'll learn

- Why `sigreturn` exists as its own syscall (you cannot
  restore the trap frame from C).
- How to safely run user code "as if it had been called
  by user code" from a signal-delivery moment.
- The trap-frame-on-stack pattern, reusable for future
  things like single-step debugging.

## What this unlocks

- A `Ctrl-C handler` in long-running programs (browser fetch
  cancel, notepad save-then-quit).
- `signal(3)`-shaped libc helper.
- The job-control machinery in chapter 79.

## Postscript: how it actually shipped

### Sigframe layout (mirrored across kernel + libc)

(The kernel declares this as `struct sigframe_k`; the libc-side
shape is `struct sigframe`. The 'k' suffix flags the kernel copy.)

```c
struct sigframe {
    uint64_t x[31];     // 248 bytes
    uint64_t sp_el0;    // 256
    uint64_t elr;       // 264
    uint64_t spsr;      // 272
    uint32_t signum;    // 276
    uint32_t pad;       // 280
    uint64_t pad2;      // 288   (round to 16B for AAPCS SP)
};
```

A static_assert in [kernel/core/syscall.c](../../../kernel/core/syscall.c)
locks both `% 16 == 0` and `== 288`. The trap: a 280-byte
frame will trip the alignment assert at build time.
Round-to-16B is non-negotiable on AArch64 -- SP
must be 16-aligned at function entry.

### Per-thread state

`struct thread` gained:

- `sig_handlers[32]` — `0`=SIG_DFL (terminate w/ 128+sig),
  `1`=SIG_IGN (drop), anything else = EL0 user fnptr.
- `sig_restorer` — the trampoline addr the libc passes on
  the first sigaction() call.

Inheritance:
- `fork`: handlers + restorer ARE inherited (POSIX). Pending
  mask is NOT.
- `exec`: all handlers reset to SIG_DFL, restorer cleared,
  pending cleared. Reason: handler addresses point at code
  in the OLD address space which we just freed.

### Kernel delivery

[kernel/core/syscall.c](../../../kernel/core/syscall.c)
`deliver_signal()`:

1. Read SP_EL0 (the user-mode stack live at the moment of
   the SVC).
2. Reserve `sizeof(sigframe)` at the top of that stack,
   16-aligned.
3. Copy `frame->x[0..30]`, `elr`, `spsr`, plus the live
   `sp_el0`, into the sigframe. `copy_to_user` validates the
   address.
4. Patch the trap frame in place: `x[0] = signum`,
   `x[1] = sigframe ptr`, `x[30] = sig_restorer`, `elr =
   handler`. (SPSR stays — already EL0t with IRQs unmasked.)
5. Write SP_EL0 = sigframe address with an inline `msr`.

The SVC eret then drops into the user handler with a clean
register file, the signum as its first argument, and a
fresh stack frame on top of the saved sigframe.

### The trampoline (in crt0.S)

```asm
.global __sigreturn_trampoline
__sigreturn_trampoline:
    mov     x0, sp           // sigframe ptr (SP at handler entry)
    mov     x8, #32          // SYS_SIGRETURN
    svc     #0
```

AAPCS guarantees that when the handler ret's (which jumps
to LR = trampoline), SP must equal whatever it was at
handler entry. That's exactly the sigframe pointer the
kernel set up. So `mov x0, sp` recovers it without any
additional bookkeeping.

### Sigreturn

`sys_sigreturn` reads the sigframe back over the trap frame:
`x[0..30]`, `elr`, `spsr`, and SP_EL0 (via `msr`). The
dispatcher tail's signal-delivery scan is **skipped on a
sigreturn syscall** so the handler's exit doesn't immediately
re-deliver the signal it just acknowledged.

### Trap: handler only fires at a syscall boundary

A pure CPU-bound user loop will never see its signal because
there's no IRQ-return delivery hook yet. The chapter-77
test had to put a `yield()` in its spin loop. This is
worth documenting as a current limitation — fixing it means
adding the same delivery code to the IRQ return path in
vectors.S, deferred for now.

### Test outcome

- Source: [userspace/sigtest/sigtest.c](../../../userspace/sigtest/sigtest.c)
- Harness: [scripts/test_sigaction.py](../../../scripts/test_sigaction.py)
- Three checks all green on first run:
  1. **Catch + sigreturn** — `kill(self, SIGTERM)` invokes
     handler (sig=15), execution resumes past the kill().
  2. **SIG_IGN** — handler does NOT run.
  3. **fork + cross-pid signal** — child installs handler,
     yields; parent sends SIGINT; child handler runs (sig=2),
     child exits 0, parent reaps cleanly.

### What's deferred

- Per-signal mask + sigprocmask().
- SA_RESTART semantics (we currently leave the slow-syscall
  EINTR question unanswered).
- IRQ-return delivery so CPU-bound user code can be
  interrupted.
- SIGCHLD wiring — next chapter.


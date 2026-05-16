# Chapter 76 — Signals, starting with SIGINT

**Status:** Implemented (pre-M65; the plumbing landed alongside
the M58 raw-tty work).

The kernel currently has no way to *interrupt* a running
program. Ctrl-C in the shell prints `^C` and that is it. A
runaway browser fetch, a `cat` on a giant file, an infinite
loop in a script — all of them require killing the QEMU
process from the host. Signals fix this.

## What this chapter adds

- A signal-pending bitmask in `struct thread`.
- `tty_raw_input` recognises Ctrl-C and posts SIGINT to the
  foreground process group.
- A default-handler table: SIGINT, SIGTERM → terminate;
  SIGKILL → terminate (uncatchable); the rest → ignore for now.
- Delivery on syscall return: if a signal is pending and the
  default action is terminate, the syscall trampoline never
  resumes user mode — it calls `thread_exit`.

## Prerequisites

- Chapter 14 — SVC and the syscall ABI
- Chapter 18 — Console keyboard input
- Chapter 11 — Threads

## Plan

- Define `signo` constants; only SIGINT, SIGTERM, SIGKILL
  matter this chapter.
- Posting a signal: `kill(pid, signo)` syscall; OR-into the
  pending mask.
- The "foreground process" concept: the shell tracks who
  it most recently spawned; Ctrl-C posts to that.
- Delivery point: ALWAYS at syscall return (or at IRQ
  return-to-user), never asynchronously inside kernel code.
- Test: a busy-loop user program; Ctrl-C kills it within
  one timer tick.

## What you'll learn

- The fundamental signal-delivery rule: "at well-defined
  points where it's safe to perturb user state."
- Why Linux's signal-restart-or-EINTR debate exists (we'll
  pick a side in chapter 77).

## What this unlocks

- Ctrl-C actually does something.
- `kill <pid>` shell builtin.
- Catchable signals in chapter 77.

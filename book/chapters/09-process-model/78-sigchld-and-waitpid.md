# Chapter 78 — SIGCHLD and waitpid: parent-child plumbing

**Status:** Implemented (chapter-78 / 2025-Q4). Zombie state
and process groups remain deferred (will land with chapter
79's job control).

The reaping path today is `wait()` — block in the kernel
until the most recently spawned child exits. Real Unix
processes get notified asynchronously via SIGCHLD and use
`waitpid()` to pick a specific child or to non-block.

## What this chapter adds

- SIGCHLD posted to the parent on child exit.
- `SYS_WAITPID(pid, &status, options)` — supersedes `wait`.
  - `pid > 0`: that specific pid
  - `pid == -1`: any child
  - `WNOHANG`: return 0 immediately if no exited child
- Zombie state for exited-but-not-reaped children.
- A `pgid` (process group) field per thread, so SIGCHLD
  delivery is well-defined when a parent forks twice.

## Prerequisites

- Chapter 76 — Signals
- Chapter 17 — init/spawn/wait
- Chapter 73 — fork

## Plan

- Thread states extend with THREAD_ZOMBIE.
- Exit path posts SIGCHLD then leaves the slot in zombie
  state until reaped.
- `waitpid` walks the parent's children list, picks one
  matching the criteria, copies status out, frees the slot.
- WNOHANG is just "no match? return 0 instead of block."
- A SIGCHLD-handler-based parent (long-running daemon)
  reaps children as a side effect of normal work.

## What you'll learn

- Why zombies exist (the parent must observe the exit
  status; we cannot free the entry until then).
- The relationship between SIGCHLD and waitpid (the signal
  notifies, waitpid collects).
- Process groups, briefly — full pgrp/session semantics
  come with job control in chapter 79.

## What this unlocks

- The shell's "spawn N pipeline children" pattern can finally
  reap them in arrival order.
- A working `init` that adopts orphans.
- Job control (chapter 79).

## Postscript: how it actually shipped

### The kernel surface

Two additions:

1. `thread_waitpid(target_pid, code_out, options)` in
   [kernel/core/thread.c](../../../kernel/core/thread.c).
   Same scan/reap loop as the old `thread_wait`, but now
   filtered by pid and short-circuits on `WNOHANG` instead
   of yielding. The legacy `thread_wait` is now a one-liner
   wrapper for `thread_waitpid(-1, code_out, 0)`.

2. SIGCHLD post in `thread_exit`, fired against the parent
   *in addition to* the existing wake-the-WAITING-parent
   path. The two are independent: a parent blocked in
   `wait()` is woken by the state transition; a daemon
   parent that never blocks is notified by SIGCHLD.

Userland surface: `SYS_WAITPID = 33`, libc inline
`waitpid(pid, &status, options)`, plus the `WNOHANG = 1`
and `SIGCHLD = 17` macros.

### Trap: SIGCHLD's SIG_DFL must be "ignore"

This was the most important design decision in the chapter.
Chapter 77's dispatcher tail terminates the receiving
thread with code `128 + sig` for any signal whose handler
is SIG_DFL. If we left that as-is, every existing program
in the OS — none of which catch SIGCHLD — would die with
exit code 145 the moment any of its forked children exited.

Fix: special-case `SIGCHLD` in the dispatcher tail to skip
the terminate path entirely. POSIX phrasing: "the default
action for SIGCHLD shall be to ignore the signal." One line
in [kernel/core/syscall.c](../../../kernel/core/syscall.c):

```c
if (h == 0) {                         /* SIG_DFL */
    if (s == SIGCHLD) continue;       /* ignore by default */
    thread_exit(128 + s);
}
```

A program that explicitly installs `SIG_DFL` for SIGCHLD
still gets ignore behaviour. A program that explicitly
installs a handler runs it. A program that explicitly
installs `SIG_IGN` already takes the `continue` path one
branch up.

### Trap: WNOHANG sentinel design

`waitpid` has three return semantics that must not collide:

- `> 0` — reaped pid (always a real pid; pids start at 1).
- `0`   — "no exited child yet, ask again later". Only
           possible with `WNOHANG`.
- `-1`  — no child at all matches the filter.

We only `copy_to_user` the exit code when `tid > 0`, so a
poll never clobbers the caller's status buffer with a
stale value left over from a previous reap.

### Delivery only at syscall return

Same caveat as chapter 77: SIGCHLD fires at the parent's
next syscall return tail, not asynchronously inside CPU-bound
code. The chldtest puts a `yield()` in its spin loops to
force the trap. A truly preemption-aware implementation
would add the same delivery code to the IRQ return path in
vectors.S; deferred.

### Test

Five checks, all green on first run, in
[userspace/chldtest/chldtest.c](../../../userspace/chldtest/chldtest.c):

1. **SIGCHLD on exit** — parent installs handler, forks a
   quick-exit child, yields; handler runs (sig=17), then
   `waitpid(-1)` reaps cleanly.
2. **waitpid by pid** — parent forks a slow child and a
   fast child; reaps fast first by name, then slow with -1.
3. **WNOHANG** — polls return 0 while child is alive, then
   the pid once it exits.
4. **SIG_DFL = ignore** — a freshly exec'd child (no handler
   inherited) spawns a grandchild and survives the
   grandchild's exit. Verified by exit code 0 ≠ 145.
5. **Legacy wait()** — the old SYS_WAIT shape still works
   after the refactor.

Harness: [scripts/test_sigchld.py](../../../scripts/test_sigchld.py).

### What's deferred

- True THREAD_ZOMBIE state. Today the EXITED thread is
  effectively a zombie until reaped, but we don't expose a
  separate state to ps. Mostly cosmetic.
- `pgid` (process groups). Needed for `kill -GROUP` and for
  `setpgid()`. Lands with chapter 79.
- Re-entrant SIGCHLD: handler-during-handler is allowed and
  could surprise. Per-handler signal masks would fix this;
  also deferred to chapter 79.


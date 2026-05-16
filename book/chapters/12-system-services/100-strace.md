# Chapter 100 — strace: a syscall tracer in 200 lines

**Status:** Stub. Tracking milestone 89.

Debugging a misbehaving user program today means adding
printfs and rebuilding. A real OS has `strace`, and ours
can have one too with surprisingly little kernel code.

## What this chapter adds

- A per-thread "trace ring" allocated on demand.
- Two kernel hooks in the SVC entry: pre and post, log
  syscall #, args, return value.
- `SYS_PTRACE_ATTACH(pid)` — turn on tracing for a target.
- `/proc/<pid>/trace` reads from the ring (drains as you
  read, or peek with `cat -f`).
- `/bin/strace prog ...` — fork, attach, exec, stream.

## Prerequisites

- Chapter 14 — SVC and the syscall ABI
- Chapter 99 — procfs (we surface the ring through it)

## Plan

- Minimal pre/post log entry: `(timestamp, syscall_no,
  arg0..arg5, ret)`.
- Decoders for common syscalls (open path, read length,
  etc) live in user-space `/bin/strace`, not the kernel.
- A flag to fail-on-syscall (poor-man's failpoint testing).

## What you'll learn

- How tracing is fundamentally a small kernel hook plus a
  large user-space decoder.
- Why strace is "free" performance-wise when not attached
  (one branch per syscall).

## What this unlocks

- Bug reports with traces attached.
- The "find the syscall that returns the error" workflow.

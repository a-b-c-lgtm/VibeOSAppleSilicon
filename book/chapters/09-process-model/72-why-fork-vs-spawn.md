# Chapter 72 — Why fork (and not just spawn)

**Status:** Stub. Tracking milestone 65.

The kernel has run with `spawn(path, args)` since chapter 17.
That has been enough for the shell, the launcher, every GUI
app, and even the browser. But it is not Unix's process model.
This chapter is the why-this-exists prelude before we add
`fork` and `exec` over the next several chapters.

## What this chapter adds

Argument and design. No code lands until chapter 73.

## Prerequisites

- Chapter 17 — `init`, `spawn`, `wait`
- Chapter 24 — Per-process address spaces
- Chapter 25 — Hardening the kernel/user boundary

## Plan

- The "spawn vs fork" debate: posix_spawn, vfork, Windows
  CreateProcess.
- What spawn cannot do: redirect-then-exec, signal-mask
  inheritance, fd manipulation between fork and exec, debugger
  fork-following, daemons that double-fork.
- What fork costs: address-space copy, return-twice ABI,
  copy-on-write bookkeeping if we want it cheap.
- The plan: chapter 73 lands fork (eager copy), 74 lands exec,
  75 makes the pair cheap with COW.

## What you'll learn

- The POSIX argument for return-twice as the right primitive.
- Why every textbook teaches fork first even though spawn is
  simpler.
- The setup for the next two chapters' implementations.

## Out of scope

- Implementation. This is a design chapter only.

# Chapter 79 — Job control in the shell

**Status:** Stub. Tracking milestone 72.

With signals (76, 77), waitpid (78), and fork+exec (73, 74)
we now have everything a real shell needs to manage jobs:
background pipelines, suspending, resuming, and listing them.

## What this chapter adds

- `&` at end of command: spawn the pipeline, do not wait.
- Ctrl-Z (SIGTSTP): stop the foreground pipeline.
- `jobs`: list all jobs (running/stopped) with status.
- `fg [%n]` and `bg [%n]`: resume.
- The shell forwards SIGINT/SIGTSTP only to the foreground
  pipeline's process group; background jobs are insulated.

## Prerequisites

- Chapters 76–78 — full signal stack and waitpid
- Chapter 40 — Shell pipelines

## Plan

- The shell tracks a `jobs[]` array: pgid, status, last
  command line.
- Each pipeline runs in its own process group; the shell
  is in its own group.
- The TTY's "controlling pgid" decides who Ctrl-C goes to;
  the shell flips it on `fg`/`bg`.
- SIGTSTP delivers SIGSTOP to the pgid; SIGCONT resumes.
- Status reporting via SIGCHLD with WUNTRACED — a stop is
  also a state change worth noticing.

## What you'll learn

- The Unix "controlling tty" abstraction in miniature.
- Why job control needs a pgrp, not just per-process
  signal masks.
- The race between "shell about to wait on fg job" and
  "fg job stops or exits" — solved via signal masking.

## What this unlocks

- `make &` while you keep working.
- A real shell experience that matches readers' bash
  intuition.
- Future: proper sessions and `setsid`, terminal resize
  forwarding (SIGWINCH).

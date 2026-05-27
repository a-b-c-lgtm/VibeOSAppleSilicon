# Chapter 78 — Job control in the shell

> **Milestone in this chapter:** plan-only sketch of the
> chapter-79 job-control work — the real implementation lands
> in chapter 79 (`gui_term` switching to real fork+exec).
> **Code referenced:**
> - [userspace/sh/](../../../userspace/sh/) (background `&`,
>   Ctrl-Z, `fg`, `bg`, `jobs`)
>
> **At the end of this chapter** you will have the shape the
> shell's job-control surface needs: `&` to background a
> pipeline, Ctrl-Z (SIGTSTP) to stop the foreground job, and
> `fg` / `bg` / `jobs` to navigate. Prerequisites: chapters
> 76–78 (signals + waitpid), chapter 72 + 74 (fork + exec).
> **No code lands in this chapter** — see chapter 79 for
> the implementation.

With signals (76, 77), waitpid (78), and fork+exec (73, 74)

## What this chapter adds

- `&` at end of command: spawn the pipeline, do not wait.
- Ctrl-Z (SIGTSTP): stop the foreground pipeline.
- `jobs`: list all jobs (running/stopped) with status.
- `fg [%n]` and `bg [%n]`: resume.
- The shell forwards SIGINT/SIGTSTP only to the foreground
  pipeline's process group; background jobs are insulated.

## Prerequisites

- Chapters 75–77 — full signal stack and waitpid
- Chapter 39 — Shell pipelines

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

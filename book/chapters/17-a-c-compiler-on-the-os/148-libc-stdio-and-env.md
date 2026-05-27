# Chapter 148 — A POSIX-ish libc, part 1: stdio, errno, env

> **Milestone in this chapter:** section overview for libc part 1.
> The implementation lands across the four follow-up chapters
> 149–152.
> **Code referenced (delivered across the substeps):**
> - [userspace/libc/errno.h](../../../userspace/libc/errno.h),
>   [userspace/crt/crt0.S](../../../userspace/crt/crt0.S)
> - [userspace/libc/stdio.h](../../../userspace/libc/stdio.h)
> - [userspace/libc/env.h](../../../userspace/libc/env.h)
>
> **At the end of this chapter** you will know the four substeps that
> together grow the freestanding libc into a POSIX-ish surface big
> enough for a self-hosting compiler: an `errno` foundation, a
> `FILE *` stdio with the standard buffering modes, an `environ[]`
> arena, and the migration of every existing syscall wrapper to the
> "return -1, set errno" convention. **No code lands in this
> chapter.** It is the index for the four that follow.

## Substeps

- [Chapter 149](149-errno.md) — `errno` foundation: global slot
  defined in `crt0.S`, populated by `__svc_check` inside the
  `_svc{0..6}` helpers. Backward-compat: the wrappers still return
  negative kernel errno. **Done.**
- [Chapter 150](150-stdio.md) — `FILE *` with `_IOFBF` /
  `_IOLBF` / `_IONBF` buffering, `fopen` / `fread` / `fwrite` /
  `fclose` / `fprintf` / `fseek` / `ftell` / `fflush`. Backed by
  a new `SYS_LSEEK = 101`. **Done.**
- [Chapter 151](151-env-arena.md) — `environ[]` with an owning
  string arena, `setenv` (3-arg overwrite flag) / `unsetenv` /
  `putenv` / `clearenv`. **Done.**
- [Chapter 152](152-errno-convention.md) — flip the syscall
  wrappers from "return `-errno`" to "return `-1` + set `errno`"
  (the POSIX convention). Migrate the ~50
  `printf("...errno=%d", -fd)` sites to read `errno` instead. Add
  `strerror`. Port `cat` / `wc` / `head` / `tail` to `FILE *`.
  **Done.**

## Why this chapter exists

The libc today (`userspace/libc/`) is the freestanding subset the
book has needed so far: a `printf` family that writes straight to
fd 1, `malloc` / `free`, the syscall wrappers, a few string
helpers. It works for everything in `userspace/` because every
existing app was written *to that surface*.

A real compiler (the Part XVIII GCC port, or any other future
backend) is not. It was written to ANSI C plus POSIX, and they
assume:

- `FILE *` exists, with `fopen` / `fread` / `fwrite` / `fclose`,
  `fprintf` / `fscanf`, `setvbuf`, `feof`, `ferror`, `fgetc` /
  `fputc`, `fgets` / `fputs`, `ungetc`, `fflush`, `ftell` /
  `fseek`, line-buffered stdout, fully-buffered files.
- A real `errno` — a per-thread `int` set by every failing libc
  call, returned alongside a `-1` from the syscall wrappers.
  Today the wrappers return negative kernel error codes directly;
  that confuses every standard algorithm that wants `errno != 0`.
- A working `getenv` / `setenv` / `unsetenv` /
  `putenv`, backed by a mutable per-process `environ`.
  Chapter 32 added enough env support for the shell;
  this chapter promotes it to a full table any process
  can mutate.

## What this chapter adds

- `userspace/libc/stdio.h` and `stdio.c`: real buffered
  `FILE *` on top of the existing fd syscalls. ~600
  lines including the printf/scanf reuse.
- `userspace/libc/errno.h` + a thread-local `errno`
  slot in the TLS area added in chapter 92.
- Syscall wrapper convention change: every wrapper now
  returns `-1` on failure and sets `errno`, instead of
  returning a negative errno directly. Internal kernel
  callers stay as before.
- `userspace/libc/env.h`: an `environ[]` that owns its
  strings, with `setenv` / `unsetenv` / `putenv` and a
  small string arena so we don't leak on overwrite.
- New regression: `scripts/test_libc_stdio.py` opens
  `/mnt/poem.txt`, copies it through `FILE *` into
  `/tmp/poem.out`, checksums both, asserts equality.

## Prerequisites

- Chapter 32 — environment variables (the basis we
  extend).
- Chapter 40 — writable tmpfs (so the round-trip
  regression has a target).
- Chapter 92 — userspace threads + TLS (so `errno`
  can be thread-local).

## Plan

1. Add the TLS slot for `errno`. Walk every syscall
   wrapper in `userspace/libc/syscall.h` and convert it
   to the `-1 + errno` convention. Add an internal
   helper (`__set_errno_from_kernel`) so each wrapper
   stays one line.
2. Write `FILE`. Buffer is 4 KiB. `_IONBF` for stderr,
   `_IOLBF` for stdout when isatty, `_IOFBF` otherwise.
   `fflush` walks the open-FILE list (kept in a static
   array so we can flush them all on `exit`).
3. Replace the existing `printf` plumbing's "write to
   fd 1" with "write through `stdout`". The existing
   `printf.h` formatter logic doesn't change; only its
   sink changes.
4. Rewrite the env table. The old version stored
   pointers into `argv` memory; the new one owns its
   strings via a 16 KiB arena.

## What you'll learn

- Why `errno` has to be thread-local, and what changes
  the day you ship userspace threads.
- The three stdio buffering modes and why a real compiler's build
  output looks correct only when stderr is unbuffered.
- The historical accident where `printf` became the
  point on which every other C program depends — and
  why our existing tiny `printf` carries us most of
  the way.

## What this unlocks

- Every chapter from 117 onwards.
- `cat`, `wc`, `head`, `tail` get rewritten on top of
  `FILE *` and lose ~50 lines of fd-handling each.
- Any new tool added in this section can be written in
  idiomatic C, not in our local freestanding dialect.

## Applied to

- **Existing app rewrites:** `cat`, `wc`, `head`,
  `tail`, `notepad`'s file load/save path (move from
  raw syscalls to `fopen` / `fread`). Remove
  ~200 lines of duplicated read-into-buffer code
  across `userspace/`.
- **New apps:** none yet — the toolchain itself is the
  thing this unblocks (chapters 157, 124).
- **New tests:** `scripts/test_libc_stdio.py`,
  `scripts/test_libc_errno.py`,
  `scripts/test_libc_env.py`.

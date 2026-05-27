# Chapter 73 — exec: tearing down and rebuilding an AS in place

> **Milestone in this chapter:** 65 — add `SYS_EXEC` so a forked
> child can replace itself with a different program.
> **Code referenced:**
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_EXEC`)
> - [kernel/core/elf.c](../../../kernel/core/elf.c)
>
> **At the end of this chapter** you will have `SYS_EXEC(path,
> argv)` destroying the current AS, loading a fresh ELF, and
> returning to userspace at the new entry point — the second
> half of the classic Unix fork + exec pair that lets the
> shell run external commands.

`fork()` made a clone. `exec()` replaces it with a different
program in the same pid. Together they finally let the shell
do what every Unix shell does: fork, set up redirections in
the child, then exec the new program.

## What this chapter adds

- `SYS_EXEC(path, argv)` — destroys the current AS, loads the
  ELF at `path`, lays out a fresh argv on the new stack page,
  and `eret`s into the new entry point.
- The shell now uses `fork + exec` instead of `spawn`.
- A `redirect_test` program that demonstrates "child sets up
  fd 1 → file before exec" — the canonical pattern.

## Prerequisites

- Chapter 72 — fork
- Chapter 14 — ELF loading and the first user program

## Plan

- Reuse `elf_load_user` to build the new AS, then atomically
  swap it in and free the old one.
- Argv handling: the kernel must copy argv strings out of the
  *old* AS before the swap, since the new AS won't have them
  mapped.
- Failure handling: if the ELF won't load, exec returns -ENOENT
  (or similar) and the old AS is preserved — exec is "all or
  nothing."
- File-descriptor disposition: stdin/stdout/stderr survive,
  the rest survive by default (no FD_CLOEXEC yet — that's a
  one-line addition later).

## What you'll learn

- Why exec returning at all means it failed.
- The "argv must outlive the AS that owned it" subtlety.
- Why the old `spawn(path, args)` was secretly a fork-followed-
  by-immediate-exec inside the kernel.

## What this unlocks

- A real shell pipeline implementation (chapter 78 needs it).
- `exec`-style helpers in the libc (`execv`, `execvp`).
- `system()` — fork + exec + waitpid.

## Postscript: how it actually shipped

### Same thread, new program

`sys_exec` does **not** spawn a new thread. It mutates the
current one: same `struct thread`, same kstack, new AS, new
ELF, new entry point, new SP_EL0. The kernel-side
bookkeeping is therefore very small; the work is all in the
order of operations.

See [kernel/core/syscall.c](../../../kernel/core/syscall.c)
`sys_exec` and `copy_argv_from_user`.

### Argv must outlive the old AS

We stage path + argv into kernel memory **before** any AS
work:

- `path` → `copy_string_from_user` into a stack buffer.
- `argv` → `copy_argv_from_user` into a **static**
  `argv_storage[16][96]`. We use static, not stack, because
  16 × 96 = 1.5 KiB is too much to wedge into a 16 KiB
  kstack alongside everything else. Static is safe under
  this build's single-CPU + no-in-syscall-preemption
  invariant.

Once both are staged, the parent argv pages can disappear and
the new program will still find its arguments.

### Activate-then-destroy AS swap

The load-bearing line is the order:

1. `address_space_create` a new AS, `elf_load_user` into it.
2. **Activate** the new AS (writes TTBR0_EL1, TLB invalidate).
3. `thread->as = new_as`.
4. `address_space_destroy(old_as)` — this walks the *old*
   L2/L3 to free pmem pages.

Step 4 works while step 2 is in effect because the old AS's
physical pages are still live DRAM, and the kernel reaches
them through the boot L1 identity map (slots 2..N), not via
any per-process slot. This is the same invariant that lets
`address_space_clone` memcpy across AS in chapter 72.

### Frame patching for the eret

The SVC handler will eret using the current frame on the way
back. We patch it in place:

- `frame->x[0..30] = 0` — start the new program with a clean
  register file.
- `frame->elr = img.entry_va` — eret jumps here.
- `frame->spsr = 0x340` — EL0t, IRQs unmasked.

### SP_EL0 set directly

`restore_context` in vectors.S (272-byte frame) does NOT
restore SP_EL0. So we set SP_EL0 directly with an inline
`msr sp_el0, %0` to point at the freshly minted user stack
in the new AS. The eret then drops into `_start` with the
right stack already in place.

### Display name

`thread_rename(t, path)` so `ps`, panics, and exit logs show
the new program name. We also rebuild `t->args` from
`argv[1..]` joined by spaces.

### File-descriptor disposition

No `O_CLOEXEC` yet — every inherited fd survives exec. The
test case `[forktest] check 2 (fork+exec) ok` confirms
`/bin/hello`'s `printf` still reaches the console fd that
forktest opened.

### Test outcome

Verified by `[forktest] check 2 (fork+exec) ok` — child
execs `/bin/hello`, hello prints its banner and pid, exits
cleanly, parent reaps without spurious wakeups.


# Chapter 17 — `init`, `spawn`, `wait`: the simplest process model that works

> **Where the code lives.**
> Per-thread parent + exit code: [kernel/core/thread.h](../../../kernel/core/thread.h) (struct thread)
> Wait/exit/lookup: [kernel/core/thread.c](../../../kernel/core/thread.c)
> New syscalls: [kernel/core/syscall.c](../../../kernel/core/syscall.c) (`SYS_SPAWN`, `SYS_WAIT`)
> User wrappers: [userspace/libc/syscall.h](../../../userspace/libc/syscall.h) (`spawn`, `wait`)
> The first user program: [userspace/init/init.c](../../../userspace/init/init.c)
> Per-thread `SP_EL0` save/restore: [kernel/arch/context_switch.s](../../../kernel/arch/context_switch.s)

[Chapter 16](16-files-and-vfs.md) ended with two user programs (`hello`
and `cat`) that the kernel handed to the scheduler one at a time
from `userspace_demo`. That worked because the boot thread did
the orchestration in C: load ELF, spawn user thread, yield until
it exits, repeat. Workable for two programs; obviously not the
shape of a real OS.

This chapter takes the next step. The kernel still loads exactly
*one* user program directly — `/bin/init`. From then on, every
program gets launched by another program, by way of a syscall.
Two new syscalls are enough to make that work:

- `SYS_SPAWN(const char *path)` — load `path` from ramfs and
  start it as a new user thread; returns its tid.
- `SYS_WAIT(int *code_out)` — block until any child exits, then
  reap it and return its tid (and exit code, via `*code_out`).

Plus an extension to the existing `SYS_EXIT(code)`: the child's
exit code now lives on its `struct thread` until the parent's
`SYS_WAIT` collects it.

After this chapter, `init` runs `/bin/hello` and `/bin/cat` by
name, in sequence. The scheduler doesn't need to know what user
programs exist; the boot thread doesn't need to enumerate them.

## A short detour: why not `fork` and `execve`?

Most OS books at this point implement POSIX `fork` and `execve`.
We're not going to. Here's why.

`fork` duplicates the calling process: same code, same data, same
open files, same registers — except the child sees `fork()`
return 0 while the parent sees the child's pid. Implementing
`fork` *correctly* needs **per-process address spaces**: the
child has to reuse the same virtual addresses as the parent
(for code, stack, heap) but with its *own* physical pages, so
each can mutate its memory without disturbing the other.

Today, milestone 9, our kernel still uses **one global page
table** with permissive `AP=01` mappings on the user-region L1
blocks — every user thread sees every byte of physical RAM at
its own physical address. There's no notion of "the same VA
maps to a different page in the child". `fork` *cannot* exist
yet without first introducing per-process L1 page tables, ASIDs,
TTBR0 switching on context switch, and finite-VA user mappings
(L2/L3 walks instead of 1 GiB blocks).

That's a milestone of its own. Doing it right is at least 500
lines of code, several deep MMU debugging sessions, and a
chapter. Doing it *wrong* yields silent memory corruption
between processes that takes weeks to chase down.

The alternative — `spawn(path)` plus `wait(&code)` — sidesteps
the entire problem. The new program gets its own physical pages
(via `pmem_alloc_page`), its own kernel-side stack, and its own
fd table. The parent and child share exactly the same global
address space; they happen to land on different physical pages
because `pmem` is monotonic. No two processes' user code or
stack ever overlap.

This isn't a placeholder hack — `spawn` is a respectable design
choice. Plan 9 used `rfork` and a richer `exec`; it never had a
vanilla `fork`. `posix_spawn` exists in POSIX precisely because
spawn-style is more efficient than fork+exec when you don't need
the inherited execution-state semantics. Many embedded RTOSes
expose only spawn. This kernel will eventually grow per-process
address spaces, but on the day we do, `fork` and `execve` will
slot in alongside `spawn`, not replace it.

## A surprise lesson: SP_EL0 isn't auto-saved

Before we get to the new syscalls, there's a one-line hardware
detail that took half a debugging session to find. It's a good
example of the kind of "obvious in hindsight" trap aarch64 sets
for OS authors.

The story: `init` spawns `hello`, then calls `wait(&code)`. The
SVC handler calls `thread_wait`, which marks `init` as
`THREAD_WAITING` and yields. Hello runs, exits. Hello's
`thread_exit` wakes `init` and yields back. Init resumes inside
its SVC handler, the kernel writes `0` (hello's exit code) to
`init`'s user pointer, the SVC returns via `eret`, and init
reads from the same address... and gets garbage.

Specifically, init's `code` lived at user VA `0x22fffafdc`. The
kernel wrote `0` to that address; we verified it with a
read-back. The user, microseconds later, read `0x386393A0` from
the same address. Same VA, same PA, same cache, same CPU.

The cause: **`SP_EL0` is banked, not auto-saved across context
switches.**

Here's what happened in detail. When `init` is running at EL0
and takes its SVC, the architecture stops using `SP_EL0` and
switches to `SP_EL1` (the kernel stack); `SP_EL0` is preserved,
because no one writes to it. Good so far.

The kernel's SVC handler eventually calls `cswitch_to(init,
hello)`. We save the *kernel*-side context of `init` and switch
to hello's kernel stack. But we never save `init`'s `SP_EL0`.

Then hello's `user_trampoline` runs and explicitly writes
`SP_EL0 = hello.user_sp_top`. Hello executes at EL0 with that
SP. Hello calls `exit`, takes an SVC, and the kernel eventually
calls `cswitch_to(hello, init)` — switching back. We restore
`init`'s kernel context. We `eret` from `cswitch_to`'s caller
(yield), unwinding back into `init`'s SVC handler. Eventually,
the SVC handler `eret`s back to EL0, and the architecture
restores `SP_EL0` as the active stack pointer.

But `SP_EL0` was *never restored*. It still holds hello's value
(or worse, whatever hello's user code last computed for it). So
init resumes at EL0 with **hello's stack pointer**. The
`ldr w9, [sp, #28]` in init reads from hello's old stack page,
not init's.

The fix is one paragraph of asm: save `SP_EL0` when entering
`cswitch_to`, restore it at the end. Because we don't know at
switch time whether the outgoing/incoming thread was at EL0,
the safe thing is to do this unconditionally — it costs two
instructions per switch.

```asm
cswitch_to:
    sub     sp, sp, #288                 // was 272; +16 for SP_EL0
    ; ... save x0..x30, ELR, SPSR ...
    mrs     x16, sp_el0                  // CAPTURE SP_EL0
    stp     x16, xzr, [sp, #272]
    ; ... swap stacks ...
    ldr     x16, [sp, #272]              // RESTORE SP_EL0
    msr     sp_el0, x16
    ; ... restore x0..x30 ...
    eret
```

The synthesised initial frame for new threads also grew by 16
bytes; the slot is zeroed, which is fine because
`user_trampoline` writes `SP_EL0` itself on the first eret to
EL0.

Lesson for future EL0/EL1 work: `SP_EL0` is *separate state*
from the GPRs; the architecture banks it across exception levels
but does not bank it across context switches. Anything that
spans both — e.g. our SVC handler that yields — has to manage
it explicitly. Same goes for `TPIDRRO_EL0` (TLS pointer), `FPSR`,
`FPCR`, and the SIMD register file when we eventually add SIMD
support. Default state is "not preserved unless you preserve it".

## The fd table grows up: parent-tracking on threads

`struct thread` gains two new fields:

```c
struct thread {
    /* ... */
    int               parent_id;     /* -1 if no parent           */
    int               exit_code;     /* valid once state == EXITED */
    enum thread_state state;
    struct thread    *next;          /* runqueue link              */
    struct thread    *all_next;      /* link in g_all_head list    */
    struct fd_entry   fds[FD_TABLE_SIZE];
};
```

`parent_id` is set at thread creation to `current()->id`. So:

- The boot thread has `parent_id = -1` (no parent).
- `init`'s parent is the boot thread.
- Every program `init` spawns has `parent_id == init.id`.

`exit_code` is filled in by `thread_exit(int code)` and read by
the parent's `thread_wait`.

A new state, `THREAD_WAITING`, indicates the thread is blocked
in `wait()`. It's not on the runqueue and won't be picked by
`yield`. When a child calls `thread_exit`, it scans up to its
parent, and if the parent is `WAITING`, transitions it back to
`READY` and pushes it onto the runqueue.

We also introduce a global "all threads" singly-linked list
(`g_all_head`) so `thread_wait` can scan for children without
relying on the runqueue (children might be in any state).

The reaping rule changes too. Previously, an `EXITED` thread
was freed on the next `yield()`. Now, `EXITED` threads stay in
the all-list until their parent calls `wait`. The kernel-side
stack is freed eagerly (the moment the thread yields away for
the last time, via `g_stack_to_free`), but the `struct thread`
itself lingers as a zombie until `wait` collects it. This is
literally how Unix has worked for fifty years.

## `thread_wait`

```c
int thread_wait(int *code_out)
{
    int my_id = g_current->id;
    for (;;) {
        int has_child = 0;
        struct thread *exited = NULL;

        for (struct thread *t = g_all_head; t; t = t->all_next) {
            if (t->parent_id != my_id) continue;
            has_child = 1;
            if (t->state == THREAD_EXITED) { exited = t; break; }
        }

        if (!has_child) return -1;        /* ECHILD-equivalent */

        if (exited) {
            int child_id = exited->id;
            int code     = exited->exit_code;
            all_remove(exited);
            if (exited->stack_base) kfree(exited->stack_base);
            kfree(exited);
            g_thread_count--;
            if (code_out) *code_out = code;
            return child_id;
        }

        /* Has children, none exited yet — block. */
        g_current->state = THREAD_WAITING;
        yield();
        /* When woken (by a child's thread_exit), re-scan. */
    }
}
```

Two things are worth highlighting. First, the scan is O(N) over
all live threads — fine for a hobby kernel where N is single
digits, terrible at scale. The standard fix is a per-process
"first child / next sibling" linked list and a wait-queue;
chapter 22 (when we get to multi-tasked workloads in earnest)
will revisit.

Second, the *block then re-scan* loop is the textbook condition-
variable pattern: state-protected predicate, sleep when false,
re-check on wake. We don't have proper CVs yet — `THREAD_WAITING`
plus a single eager wake from `thread_exit` is a degenerate
implementation — but the loop structure is right.

## `SYS_SPAWN` is just `elf_load_user` + `user_thread_create`

```c
static long sys_spawn(long name_ptr)
{
    const char *path = (const char *)(uintptr_t)name_ptr;

    const uint8_t *data; size_t size;
    int rc = vfs_lookup(path, &data, &size);
    if (rc < 0) return rc;

    struct user_image img;
    if (elf_load_user(data, size, &img) != 0)
        return -EINVAL_VFS;

    struct thread *t = user_thread_create(img.entry_va, img.stack_top_va, path);
    if (!t) return -ENOMEM_VFS;
    return (long)t->id;
}
```

The new program is found by name in ramfs (which now also
contains `/bin/hello`, `/bin/cat`, `/bin/init` — the user
binaries are first-class citizens of the file system),
parsed as ELF by the loader from chapter 15, and handed to
the thread system. `user_thread_create` sets the new thread's
`parent_id` to `current()->id` automatically, so the child
knows who to wake.

There is one TODO comment in `sys_spawn`: the user pointer
to the path is passed verbatim. A real OS would
`copy_from_user` it into a kernel buffer with a length cap,
to defend against runaway strings, page faults reading the
buffer, and TOCTOU between path validation and use. We can
get away with the shortcut today because there's no per-process
isolation — every user pointer is also a valid kernel pointer
in the global address space.

## `init` itself

```c
#include "../libc/syscall.h"

static void run(const char *path)
{
    write(1, "[init] spawn ", 13);
    write(1, path, strlen(path));
    int tid = spawn(path);
    if (tid < 0) {
        write(1, " FAILED errno=", 14);
        putd(-tid);
        write(1, "\n", 1);
        return;
    }
    write(1, " -> tid=", 8);
    putd(tid);
    write(1, "\n", 1);

    int code = 0;
    int reaped = wait(&code);
    write(1, "[init] reaped tid=", 18);
    putd(reaped);
    write(1, " code=", 6);
    putd(code);
    write(1, "\n", 1);
}

int main(void)
{
    puts("[init] starting (pid 1)");
    run("/bin/hello");
    run("/bin/cat");

    int code = 0;
    int rc   = wait(&code);
    write(1, "[init] final wait returned ", 27);
    putd(rc);
    write(1, "\n", 1);

    puts("[init] all programs finished, exiting 0");
    return 0;
}
```

That's the whole program. It's the moral equivalent of:

```sh
#!/bin/sh
/bin/hello && /bin/cat
echo "all done"
```

The boot thread's role is now:

1. Look up `/bin/init` in the ramfs.
2. Call `elf_load_user` on it.
3. Create the user thread via `user_thread_create`.
4. `thread_wait` until init exits.

Everything else flows out of `init`'s decisions. A future shell
chapter only needs to add a read-from-keyboard primitive and a
parser; the launching primitive is already here.

## Verification

Expected serial output (with the kernel chatter trimmed):

```
[user] loading /bin/init (0x1468 bytes)
[user] entry = 0x22fffb000, sp = 0x22fffb000
[user] spawned init pid 0x3
[init] starting (pid 1)
[init] spawn /bin/hello -> tid=4
hello from EL0!
pid=0x00000004
after yield, still alive
[sys_exit] thread '/bin/hello' exited with code 0x0
[init] reaped tid=4 code=0
[init] spawn /bin/cat -> tid=5
[cat] opening /motd ...
... motd contents ...
[cat] /motd done.
[cat] open(/nope) returned -2
[sys_exit] thread '/bin/cat' exited with code 0x0
[init] reaped tid=5 code=0
[init] final wait returned -1
[init] all programs finished, exiting 0
[sys_exit] thread 'init' exited with code 0x0
[user] init (pid 0x3) exited code = 0x0
```

That trace exercises every code path added in this chapter:

- The boot thread's `vfs_lookup` finds `/bin/init`.
- `user_thread_create` for init sets its `parent_id = boot.id`.
- Init spawns hello via SYS_SPAWN — kernel loads hello via
  `vfs_lookup + elf_load_user`, creates a user thread with
  `parent_id = init.id`.
- Init's SYS_WAIT blocks (init transitions to `THREAD_WAITING`)
  until hello exits. Hello's SYS_EXIT pokes init back to READY.
- The reaped exit code (0) makes it through SP_EL0 save/restore
  (the lesson above) intact.
- The cycle repeats for cat.
- Init's third `wait` returns -1 because there are no more
  children.
- Init exits 0; boot's `thread_wait` reaps it.

If any link were broken — wait blocking, exit waking, code
plumbing, address-space isolation between sequential user
threads — the trace would stop or print the wrong value at a
predictable point.

## What we *didn't* build

In keeping with the "smallest thing that works" pattern:

- **No fork/execve.** Justified above: needs per-process page
  tables, which is its own milestone. `posix_spawn`-style
  semantics are what we've shipped instead.
- **No process groups, sessions, or signals.** A child can be
  reaped only by its direct parent.
- **No environment, no argv.** `spawn(path)` takes a path and
  nothing else; the new program runs with `argc=0`. Adding a
  string array is mechanical when we want it.
- **No `waitpid(specific_tid)`.** `wait()` reaps any child. A
  multi-child shell would need targeted wait, which is a
  ten-line change to the scan loop in `thread_wait`.
- **No `copy_from_user`/`copy_to_user`.** User pointers pass
  through to the kernel verbatim. The TODO comments mark the
  spots that have to grow validation when address spaces split.
- **No reparenting on parent exit.** If `init` exits while one
  of its children is still alive, that child becomes
  unreapable. (For our demo `init` waits for everything before
  exiting, so the issue doesn't arise; but a real OS reparents
  orphans to PID 1 — yes, init itself.)

## Summary

- The kernel now launches exactly one user program directly:
  `/bin/init`. Everything else is launched by another program
  via `SYS_SPAWN`.
- `SYS_WAIT` blocks the caller until any child exits and
  returns its tid + exit code. `THREAD_WAITING` is the new
  scheduler state.
- The exit-code plumbing required a one-line architectural fix:
  `SP_EL0` must be saved and restored across `cswitch_to`. The
  hardware banks it across exception levels, not across context
  switches; a kernel that does both has to manage it.
- A spawn+wait model is a defensible, complete process API for
  a hobby kernel without per-process address spaces. Real
  `fork` + `execve` will land alongside it once the MMU
  chapter does its work — they don't replace it.

Next: a real shell that reads commands from the console and
spawns programs. That requires a working keyboard, which is the
next chapter.

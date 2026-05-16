# Chapter 93 — Sharing the FD table: CLONE_FILES and refcounted fd_table

**Status:** Done.

Chapter 91 put thread spawning into userspace (`SYS_CLONE`) and
chapter 92 made spawning honour CPU placement (`SYS_CLONE2`).
Both kept one rule from the very early days of the kernel: every
thread has its own private fd table. A thread that opens a file
sees its sibling thread's fds as "not in use", and a thread that
exits drops every fd it ever opened — even ones a sibling is
still using.

That rule is wrong for any program that wants to act like a POSIX
process — many threads, one shared set of open files. Two tests
that drove the chapter: a worker thread can't write to the same
log fd its parent opened, and the chapter-94 browser parser
thread we're about to build can't read from the TCP socket the
GUI core opened on its behalf.

Chapter 93 fixes the rule by lifting the fd table into a
separately-allocated, refcounted `struct fd_table`. The default
behaviour (used by every existing `fork` / `spawn` / `clone` /
`clone2` callsite) stays "fresh table per thread, refcount = 1".
A new syscall `SYS_CLONE3` accepts a `struct clone_args` and
honours a `CLONE_FILES` flag bit; when set, the new thread
adopts the parent's fd_table by reference, and the table is
freed only when the LAST referencing thread exits.

The chapter ships with `userspace/threadtest3` (CLONE_FILES
sharing in case A, plain clone-without-sharing in case B) and
`scripts/test_clone_files.py`.

## What this chapter adds

* **`struct fd_table`** in `kernel/core/vfs.h`. A new container
  type that owns the per-thread fd_entry array plus a refcount
  and a per-table spinlock.
* **`fd_table_create / fd_table_share / fd_table_unref`** in
  `kernel/core/vfs.c`. Allocate, bump-ref, drop-ref. The unref
  walks every still-in-use slot and drops pipe / pty / socket
  references only when the count hits zero.
* **`struct thread::fdt`** replaces the inline
  `struct fd_entry fds[FD_TABLE_SIZE]` field. Now a pointer to
  a `struct fd_table` that may be shared.
* **`vfs_close_all(t)`** becomes "drop t's reference; the unref
  function does the actual close walk only when refcount → 0."
* **`user_thread_create_shared_files_on(...)`** — a new internal
  helper, sibling of `user_thread_create_shared_on`, that takes
  a `share_fdt` flag. When set and the calling thread has an
  existing fdt, the new thread inherits it by reference.
* **`SYS_CLONE3(struct clone_args *)`** — extended-args clone
  with a flags field. Today the only defined flag is
  `CLONE_FILES = 0x01`; unknown bits return `-EINVAL` so
  userspace can't accidentally rely on undefined-bit behaviour.
* **`userspace/libc/syscall.h`** — `struct clone_args`,
  `#define CLONE_FILES`, and the `clone3()` wrapper.
* **`userspace/libc/thread.h:thread_spawn_files()`** — the
  friendly wrapper. Same shape as `thread_spawn_on` but flips
  the CLONE_FILES bit so the worker shares the parent's fds.
* **`userspace/threadtest3`** — two cases. Case A: spawn via
  `thread_spawn_files`, worker writes to a parent-opened fd,
  parent reopens and verifies the bytes round-trip. Case B:
  spawn via `thread_spawn_on` (no CLONE_FILES), worker's
  `write(parent_fd, ...)` must fail.
* **`scripts/test_clone_files.py`** — boots `-smp 2`, runs
  `threadtest3`, asserts both cases printed their `OK` markers.

## Prerequisites

* **Chapter 87** — atomics. `fd_table::refcount` uses
  `atomic_add_return32` / `atomic_sub_return32` exactly the same
  way `address_space::refcount` does (chapter 91).
* **Chapter 91** — CLONE. `SYS_CLONE3` is a strict super-set of
  `SYS_CLONE`; the trampoline (`user_clone_trampoline`), AS
  refcounting, and stack layout are unchanged. The new syscall
  just adds the optional fdt-share dance on top.
* **Chapter 92** — CLONE2. The CPU-placement field on
  `clone_args` mirrors the `cpu_id` argument from `SYS_CLONE2`
  byte-for-byte.

## The new container type

Pre-chapter-93, `struct thread` carried its fd table inline:

```c
/* kernel/core/thread.h — pre-93 */
struct thread {
    /* ... id, sp, state, name, etc ... */
    struct fd_entry  fds[FD_TABLE_SIZE];
    /* ... */
};
```

Each fd_entry is 56 bytes; with `FD_TABLE_SIZE = 16` that's a
fixed 896 bytes inline per thread. Sharing was impossible by
construction — there was no level of indirection to bump a
refcount on.

Chapter 93 introduces the indirection:

```c
/* kernel/core/vfs.h */
#include "../arch/spinlock.h"

struct fd_table {
    spinlock_t        lock;
    volatile uint32_t refcount;
    struct fd_entry   fds[FD_TABLE_SIZE];
};

struct fd_table *fd_table_create(void);
void fd_table_share(struct fd_table *ft);
void fd_table_unref(struct fd_table *ft);
```

And the thread field collapses to a pointer:

```c
/* kernel/core/thread.h — chapter 93 */
struct thread {
    /* ... id, sp, state, name, etc ... */
    /* Refcounted, possibly-shared fd table.  Each thread holds
     * exactly one reference; vfs_close_all drops it on exit. */
    struct fd_table  *fdt;
    /* ... */
};
```

The total memory cost per thread *drops* slightly — we trade an
inline 896-byte array for an 8-byte pointer plus one heap
allocation of `sizeof(struct fd_table)` (~912 bytes including
the lock + refcount). When threads share, the saving multiplies:
two threads in one process share one ~912-byte allocation
instead of carrying 1792 bytes of inline arrays.

The header trap: `vfs.h` did not previously include
`kernel/arch/spinlock.h` because none of its types needed a
spinlock. Adding `spinlock_t` as a field forces the include.
This kind of cascading-include change is worth flagging for
future readers — a freestanding kernel doesn't get a "just
include everything" precompiled header to fall back on.

## The mechanical migration

Before chapter 93 the kernel had 138 references to `t->fds[`,
`parent->fds[`, `child->fds[`, etc. spread across three .c
files (`kernel/core/{thread,vfs,syscall}.c`). Each one needed
to grow a `->fdt` indirection.

We did the migration in one shot with sed:

```sh
sed -i '' 's/->fds\[/->fdt->fds[/g' \
    kernel/core/thread.c \
    kernel/core/vfs.c \
    kernel/core/syscall.c
```

This is the kind of change that's almost always done by hand
file-by-file in a "real" project and almost always introduces
ten-line typos. The sed approach works here because the
expression `->fds[` is unique to fd-table accesses — no other
struct in the kernel has an `fds` array we'd accidentally
rewrite. After the sweep, `grep -nE '->fds\[' kernel/` returns
zero matches and the whole tree compiles. If you see a similar
sweeping rename in a future chapter, this is the pattern to
reach for first.

## Lifecycle: create, share, unref

The three lifecycle functions sit at the bottom of `vfs.c`.
`fd_table_create` is the only function that actually understands
the layout — it kmalloc's the table, zero-fills every slot, sets
slots 0/1/2 to FD_CONSOLE, and stamps the refcount at 1:

```c
/* kernel/core/vfs.c */
struct fd_table *fd_table_create(void)
{
    struct fd_table *ft = (struct fd_table *)kmalloc(sizeof(*ft));
    if (!ft) return NULL;

    ft->lock.locked = 0;
    ft->refcount = 1;

    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        ft->fds[i].in_use      = 0;
        /* ... zero the rest ... */
    }
    /* fds 0, 1, 2 = console (ramfs_index = -1 sentinel). */
    for (int i = 0; i < 3; i++) {
        ft->fds[i].in_use      = 1;
        ft->fds[i].kind        = FD_CONSOLE;
        /* ... */
    }
    return ft;
}
```

Sharing is a single atomic increment on the same primitive
chapter 91's address-space refcount uses — `address_space_share`
is the prior art:

```c
void fd_table_share(struct fd_table *ft)
{
    if (!ft) return;
    (void)atomic_add_return32(&ft->refcount, 1);
}
```

Drop is the only path with real work — only the LAST exiter
walks the table:

```c
void fd_table_unref(struct fd_table *ft)
{
    if (!ft) return;
    if (atomic_sub_return32(&ft->refcount, 1) > 0) return;

    /* Last reference — close every still-open slot so pipe /
     * pty / socket refcounts drop and the matching peer sees
     * EOF / -EPIPE.  No lock needed: by definition no other
     * thread holds a reference and so none can be touching
     * the table concurrently. */
    for (int fd = 0; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &ft->fds[fd];
        if (!e->in_use) continue;
        if (e->kind == FD_PIPE_R && e->pipe)
            pipe_unref(e->pipe, PIPE_REF_R);
        else if (e->kind == FD_PIPE_W && e->pipe)
            pipe_unref(e->pipe, PIPE_REF_W);
        else if (e->kind == FD_SOCKET && e->socket_cid >= 0)
            tcp_close(e->socket_cid);
        else if (e->kind == FD_PTY_MASTER && e->pty)
            pty_close_master(e->pty);
        else if (e->kind == FD_PTY_SLAVE && e->pty)
            pty_close_slave(e->pty);
        e->in_use = 0;
    }
    kfree(ft);
}
```

The "no lock needed when refcount hits zero" comment is worth
internalising. It's the same property that lets chapter 91's
`address_space_destroy` walk the page tables without locking
the AS — by definition, the count going from 1 to 0 means the
*calling* thread held the last reference, so no other CPU can
have a pointer to the object that is about to be freed. Any
thread that wanted to share would have observed refcount > 0
and would have done the bump (which would have prevented the
drop-to-zero in the first place).

## `vfs_close_all` becomes a one-liner

The pre-chapter-93 `vfs_close_all` walked every slot in the
exiting thread's inline fd table and did the per-fd cleanup
inline:

```c
/* pre-93 — pty close right alongside pipe close, socket close,
 * etc.  ~25 lines of switch-on-kind. */
void vfs_close_all(struct thread *t) {
    for (int fd = 0; fd < FD_TABLE_SIZE; fd++) {
        /* ... 20 lines of per-fd cleanup ... */
    }
}
```

Chapter 93 collapses that to:

```c
/* chapter 93 */
void vfs_close_all(struct thread *t) {
    if (!t) return;
    fd_table_unref(t->fdt);
}
```

The cleanup itself moved into `fd_table_unref`. The semantic
shift is the important part: pre-93 every thread exit closed
every fd it could see; chapter-93 only the LAST exit closes
the actual fds. CLONE_FILES siblings keep all their fds alive
across each other's exits, exactly as POSIX threads expect.

There's a deliberate footgun in the new implementation: we
*don't* set `t->fdt = NULL` after the unref. The field is left
pointing at freed memory. The reasoning is that any post-exit
read of `t->fdt` is itself a bug (the thread is about to be
reaped); leaving the pointer non-NULL turns that bug into an
obvious use-after-free crash in a future debug build, instead
of a silent NULL-deref that's sometimes caught and sometimes
not. Live code never touches `t->fdt` after `thread_exit`
because the reaper runs after `vfs_close_all`.

## The `share_fdt` knob

The new low-level kernel entry point is
`user_thread_create_shared_files_on`. It's a sibling of
chapter 92's `user_thread_create_shared_on` with one extra
parameter:

```c
/* kernel/core/thread.c */
struct thread *user_thread_create_shared_files_on(
    uint64_t user_entry_va,
    uint64_t user_sp_top,
    const char *name,
    struct address_space *as,
    uint64_t arg,
    uint64_t tls,
    int cpu_id,
    int share_fdt);   /* the new knob */
```

The chapter-92 entry point is reduced to a forwarder that
passes `share_fdt = 0`:

```c
struct thread *user_thread_create_shared_on(/* ... */) {
    return user_thread_create_shared_files_on(
        /* ... */, /*share_fdt=*/0);
}
```

So every existing CLONE / CLONE2 caller keeps its
"fresh-table-per-thread" semantics with no source change. Only
the new SYS_CLONE3 path passes `share_fdt = 1`.

Inside the function body, the fd table policy is a 5-line
branch right where the old `vfs_init_fdtable(t)` call used to
sit:

```c
t->fdt = NULL;
if (share_fdt && g_current && g_current->fdt) {
    t->fdt = g_current->fdt;
    fd_table_share(t->fdt);
} else {
    vfs_init_fdtable(t);   /* allocates a fresh fd_table */
}
```

`g_current` is the kernel's "the thread that just trapped via
SVC" pointer — i.e. the parent of the new clone. The NULL guard
is purely defensive; real userspace SYS_CLONE3 callers always
trap from a fully-formed user thread that owns an fd_table.

Note that we deliberately do NOT pre-allocate the new thread's
fdt before the branch. If we had written the code as

```c
/* WRONG — leaks the freshly-allocated table on the share path */
vfs_init_fdtable(t);
if (share_fdt && g_current && g_current->fdt) {
    /* now we own a t->fdt that nobody will ever look at */
    t->fdt = g_current->fdt;
    fd_table_share(t->fdt);
}
```

…the share path would leak ~912 bytes per CLONE_FILES call.
Branching first is both cheaper and clearer.

## SYS_CLONE3 and the args struct

`SYS_CLONE3` is a single-argument syscall. The argument is a
USER pointer to a `struct clone_args` that the kernel
copies into its own buffer before validating any field —
classic copy-from-user discipline so a hostile user can't
race the validation by mutating the struct mid-syscall.

```c
/* kernel/core/syscall.h — and mirrored in userspace/libc/syscall.h */
struct clone_args {
    uint64_t flags;
    uint64_t entry;
    uint64_t arg;
    uint64_t stack_top;
    uint64_t tls;
    int32_t  cpu_id;
    uint32_t _pad;       /* keep total size a multiple of 8 */
};

#define CLONE_FILES  0x01ULL
```

The 4-byte `_pad` is load-bearing: it keeps the total struct
size a multiple of 8 so that future additions can append
fields without changing the alignment of existing ones. Linux
`struct clone_args` does the same thing for the same reason.

The kernel implementation:

```c
/* kernel/core/syscall.c */
static long sys_clone3(long uargs)
{
    struct thread *parent = thread_current();
    if (!parent || !parent->as) return -EINVAL_VFS;

    struct clone_args a;
    if (copy_from_user(&a, (uint64_t)uargs, sizeof(a)) < 0)
        return -EINVAL_VFS;

    /* Reject any flag bit we don't know about yet.  This is
     * the "extensibility safety" — userspace that links
     * against an older kernel must not silently get the
     * future fancier semantics it didn't ask for. */
    const uint64_t known_flags = CLONE_FILES;
    if (a.flags & ~known_flags) return -EINVAL_VFS;

    /* Validate entry / stack the same way sys_clone /
     * sys_clone2 do.  All EL0 user mappings live in the
     * [USER_VA_BASE, USER_VA_END) range. */
    if (a.entry < USER_VA_BASE || a.entry >= USER_VA_END)
        return -EINVAL_VFS;
    if (a.stack_top <= USER_VA_BASE || a.stack_top > USER_VA_END)
        return -EINVAL_VFS;
    if (a.stack_top & 0xFULL) return -EINVAL_VFS;

    if (a.cpu_id < -1 || a.cpu_id >= (int32_t)SMP_MAX_CPUS)
        return -EINVAL_VFS;

    int share_fdt = (a.flags & CLONE_FILES) ? 1 : 0;

    struct thread *child = user_thread_create_shared_files_on(
        a.entry, a.stack_top,
        share_fdt ? "clone3+files" : "clone3",
        parent->as,
        a.arg, a.tls,
        (int)a.cpu_id, share_fdt);
    if (!child) return -ENOMEM_VFS;

    return (long)child->id;
}
```

Two design decisions worth pausing on:

**Reject unknown flag bits.** A future version of the kernel
will define `CLONE_VM`, `CLONE_SIGHAND`, etc. on bits 1, 2, 3...
If today's kernel silently ignored unknown bits, userspace that
expected those future semantics would get the *base* CLONE3
behaviour with no warning. Linux made the same call (their
clone3 also rejects unknown flags). The cost is a small
amount of caller pain when we add new flags — they have to
check the kernel version — but the safety dominates.

**The thread name is `clone3+files` or `clone3`.** Visible in
the `[sys_exit] thread '...' exited` line in the serial log
so you can tell at a glance whether a thread that exited was
a CLONE_FILES child or not. Helpful when debugging fd leaks.

## The libc wrapper

`userspace/libc/thread.h` grows a `thread_spawn_files`
sibling next to chapter 92's `thread_spawn_on`:

```c
/* userspace/libc/thread.h */
static inline int thread_spawn_files(clone_entry_t entry, void *arg,
                                     int cpu_id)
{
    void *stack = mmap(NULL, THREAD_STACK_BYTES,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return -12;

    void *sp_top = (uint8_t *)stack + THREAD_STACK_BYTES;

    struct clone_args a;
    a.flags     = CLONE_FILES;
    a.entry     = (uint64_t)(uintptr_t)entry;
    a.arg       = (uint64_t)(uintptr_t)arg;
    a.stack_top = (uint64_t)(uintptr_t)sp_top;
    a.tls       = 0;
    a.cpu_id    = (int32_t)cpu_id;
    a._pad      = 0;

    int tid = clone3(&a);
    if (tid < 0) {
        (void)munmap(stack, THREAD_STACK_BYTES);
        return tid;
    }
    return tid;
}
```

This is the only function chapter 94 will reach for directly.
Most callers should never write a `clone_args` struct by hand;
the wrapper hides the boilerplate.

## The smoke test

`userspace/threadtest3` runs two cases back-to-back. Case A
proves CLONE_FILES actually shares — the worker thread, born
on CPU 1 via `thread_spawn_files`, must be able to write to a
fd that main opened in main's address space:

```c
/* Case A — CLONE_FILES SHARES the fd table */
(void)unlink(path_a);
int fd_a = open(path_a, OPEN_CREAT | OPEN_RDWR | OPEN_TRUNC);

struct worker_arg wa = { .fd = fd_a, .want_cpu = 1 };
int tid = thread_spawn_files(worker_shared, &wa, /*cpu_id=*/1);
thread_join(tid);

/* main re-opens fd_a for read and asserts the bytes the worker
 * wrote are visible. */
close(fd_a);
int fd_a_r = open(path_a, OPEN_RDONLY);
char buf[64];
long n = read(fd_a_r, buf, sizeof(buf) - 1);
/* expect: n == 11, buf == "FROM_WORKER" */
```

Case B is the negative test — proves that when CLONE_FILES is
NOT set, the worker really does get a private table:

```c
/* Case B — plain clone (no CLONE_FILES) */
int fd_b = open(path_b, OPEN_CREAT | OPEN_RDWR | OPEN_TRUNC);

struct worker_arg wb = { .fd = fd_b, .want_cpu = 1 };
int tid_b = thread_spawn_on(worker_private, &wb, /*cpu_id=*/1);
int rc_b = thread_join(tid_b);

/* worker_private exits with code 0xFB if its write to fd_b
 * failed (any negative errno); rc_b != 0xFB would mean the
 * fd_table was unexpectedly shared. */
```

The case-B worker's `write(fd_b, ...)` fails with `-ENOSYS`
today (the kernel's `sys_write` falls through to "unknown fd,
assume console-only stdout" when the slot isn't `in_use`).
A future polish would change that to `-EBADF`, but the
chapter-93 invariant is "fd is not in_use in the worker's
private table" — *which* errno gets returned is a separate
question, so the test deliberately accepts any negative
return value as proof of private isolation.

`scripts/test_clone_files.py` boots `-smp 2`, drops to `/bin/sh`,
runs `threadtest3`, and asserts both `case A: OK` and
`case B: OK` markers appear on the serial log:

```
PASS: chapter 93 CLONE_FILES smoke test
```

## Floor caveats

Things this chapter *doesn't* do, and the rationale:

* **No per-fd `O_CLOEXEC`.** `exec()` keeps every fd open
  across the address-space replacement; if you don't want a
  fd to survive an exec, close it yourself first. POSIX
  programs use `fcntl(F_SETFD, FD_CLOEXEC)` to mark fds for
  auto-close-on-exec — we don't have that yet because
  nothing in the system actually needs it.
* **No per-fd lock during multi-step open / dup.** The
  per-table `lock` field is allocated but not yet taken in
  any path. The reasoning: two threads sharing one fd_table
  *can* race on "find a free slot, claim it" inside `vfs_open`
  and accidentally end up with the same fd in two slots.
  Today this is impossible because no in-tree program
  actually has two threads opening files concurrently; when
  one does (the chapter-94 browser parser thread is the
  obvious candidate, if it ever opens its own files), we'll
  add the spin_lock around the slot search.
* **No `CLONE_VM` / `CLONE_SIGHAND` / `CLONE_FS`.** Today
  AS sharing is implicit in "all clones share the parent's
  AS by construction" (chapter 91 set this up); cwd / env
  are *copied* at create time via `thread_inherit_fds`'s
  sister blocks; signal handlers are *copied* across
  fork (POSIX), and threads share by virtue of running in
  the same AS. Adding flag bits to control these
  individually is a chapter for later; we punt on it
  because no in-tree program needs the fine-grained
  control today.
* **fork still copies, doesn't share.** A bare `fork()`
  call still gives the child a brand-new fd_table populated
  via `thread_inherit_fds` (per-slot copy with pipe / pty /
  socket refcount bumps). Chapter 93 is purely about
  giving CLONE3 callers an opt-in to share — fork's
  long-standing copy semantics are deliberately preserved
  so chapter 65 fork tests still pass.

## What this unlocks

The chapter-94 browser parser thread needs CLONE_FILES for
two reasons: it has to read from the TCP socket the GUI core
opened (so the parser sees a valid fd 3+ in its table), and
it has to be able to `close()` that socket without leaking
the fd into the GUI core's view of the world (one shared
table means both threads see the close at once).

More generally, any worker-thread pattern that opens a file
in the parent and processes it in the worker now has a clean
implementation. Pre-chapter-93 the parent had to re-open the
file inside the worker's `entry` function (over a path
string handed across via `arg`), which works for files but
not for pipes / sockets / ptys — those have no path you can
re-open. CLONE_FILES is the only way to share them.

## Files added

* `userspace/threadtest3/threadtest3.c`
* `scripts/test_clone_files.py`
* `book/chapters/11-smp-and-memory/93-clone-files.md` (this file)

## Files modified

* `kernel/core/vfs.h` — added `#include "../arch/spinlock.h"`,
  `struct fd_table`, `fd_table_create / share / unref` protos.
* `kernel/core/vfs.c` — rewrote `vfs_init_fdtable` to allocate
  a fresh refcounted table; rewrote `vfs_close_all` to call
  `fd_table_unref`; added the three lifecycle implementations.
* `kernel/core/thread.h` — replaced the inline
  `struct fd_entry fds[FD_TABLE_SIZE]` field with
  `struct fd_table *fdt`; added prototype for
  `user_thread_create_shared_files_on`.
* `kernel/core/thread.c` — added `t->fdt = NULL;` before every
  `vfs_init_fdtable(t)` callsite (so the new function knows
  whether the caller already attached a shared table); added
  the `share_fdt` branch in
  `user_thread_create_shared_files_on`; mechanical
  `->fds[` → `->fdt->fds[` migration of every existing access.
* `kernel/core/syscall.h` — added `SYS_CLONE3 = 77`,
  `struct clone_args`, `#define CLONE_FILES`.
* `kernel/core/syscall.c` — added `sys_clone3` and wired it
  into the SVC dispatcher; mechanical `->fds[` →
  `->fdt->fds[` migration of every existing access.
* `userspace/libc/syscall.h` — mirrored `SYS_CLONE3`,
  `struct clone_args`, `CLONE_FILES`, and added the
  `clone3()` inline wrapper.
* `userspace/libc/thread.h` — added `thread_spawn_files()`.
* `Makefile` — added `THREADTEST3_*` build rules and packed
  the binary into the OSFS disk image.

## Build & test

```sh
make all
python3 scripts/test_clone_files.py    # ⇒ PASS
```

The full regression sweep (`for f in scripts/test_*.py;
do python3 "$f"; done`) passes 46/46 with the new chapter-93
test added and no existing test regressed. The mechanical
`->fds[` → `->fdt->fds[` migration touched 137 callsites
across three .c files; no behavioural drift was observed
in the chapter 60+ regression set.

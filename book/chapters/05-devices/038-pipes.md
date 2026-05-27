# Chapter 38 — Kernel pipes: ring buffers, blocking I/O, and `dup2`

> *Code:* `kernel/core/pipe.{h,c}`, additions to `kernel/core/vfs.{h,c}`,
> `kernel/core/syscall.{h,c}`, `kernel/core/thread.{h,c}`, and the new
> userspace exerciser `userspace/pipetest/pipetest.c`.
>
> *Milestone covered:* 30 (kernel pipe primitives + `dup2` + `THREAD_BLOCKED`).
>
> *Why this chapter is in the order it is:* the previous chapter
> introduced `THREAD_SLEEPING` so a thread could deschedule itself on a
> wall-clock deadline.  Pipes need the same shape — a thread that
> deschedules itself, parked on something, until somebody else wakes it
> — but with a different wakeup condition.  This chapter generalises
> "block until the timer says go" into "block until another thread says
> go," then uses that primitive to build the first pieces of the
> producer/consumer plumbing the shell will need for `cmd1 | cmd2`.
> The shell pipe parser itself comes in the next chapter; here we
> focus exclusively on making the kernel correct.

## What we're building

A POSIX-shaped pipe: a one-direction byte conduit with a fixed-size
in-kernel buffer.  Two file descriptors point at the same underlying
object; one is the read end and the other is the write end.  Bytes
written to the write end are buffered; reads from the read end drain
the buffer, blocking when there's nothing to read.  When the last
writer closes, subsequent reads return `0` (EOF).  When the last
reader closes, subsequent writes return `-EPIPE`.

By the end of the chapter, `pipetest` (a single user binary) will
prove all of the above end to end.

## A second blocking state

The previous milestone added `THREAD_SLEEPING`, but its shape was
specialised for the timer: each thread got a `wake_at_ms` field and
the scheduler walked the all-thread list on every yield, marking
ready any sleeper whose deadline had passed.  That works because the
clock is a single monotonic resource — there's exactly one event
("the current time") and one source of truth.

Pipes are different.  A thread parked on a pipe doesn't know when it
will be woken — the wake comes from somebody else, namely the writer
that just put bytes into the ring or the closer that just dropped
the last writer reference.  So we add a second state, distinct from
`THREAD_SLEEPING` so the timer-driven walk doesn't accidentally
ready it:

```c
enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_WAITING,
    THREAD_SLEEPING,
    THREAD_BLOCKED,    /* blocked on a void *token (a pipe ptr today) */
    THREAD_EXITED,
};
```

Alongside the new state we add a single field, `void *blocked_on`,
that names whatever the thread is parked on.  For pipes the token is
the `struct pipe *` itself, but the field is deliberately opaque so
the same machinery works later for sockets, signals, condition
variables, or any other one-shot wakeup source.

## Two thin helpers replace the timer-walk pattern

We expose two functions in `thread.h`:

- `thread_block_on(void *token)` — the parker.  Sets state to
  `THREAD_BLOCKED`, records the token, and yields.  When it returns,
  some other thread has called `thread_wake_blocked(token)`; the
  caller is contractually obliged to re-check the condition (this
  matches POSIX condition-variable semantics).
- `thread_wake_blocked(void *token)` — the unparker.  Walks the
  all-thread list, finds every thread with `state == BLOCKED &&
  blocked_on == token`, sets them to `READY`, pushes them onto the
  runqueue, and clears `blocked_on`.

The walk in `thread_wake_blocked` is the obvious O(n) thing — fine
for a system with single-digit threads and a single CPU, painful
later.  When pipe traffic gets heavy enough to feel it (chapter 49,
maybe?) the answer is to add a per-token wait list rather than
walking everything.  Don't optimise yet.

## The pipe object

A pipe is a 4 KiB ring buffer plus three counters:

```c
#define PIPE_BUF_SIZE 4096u

struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;       /* next byte to read */
    uint32_t tail;       /* next byte to write */
    uint32_t count;      /* bytes currently in the ring */
    int      r_refs;     /* number of FD_PIPE_R fds pointing here */
    int      w_refs;     /* number of FD_PIPE_W fds pointing here */
};
```

Two implementation choices worth calling out:

**`count` instead of "empty == full" sentinel.**  The textbook
SPSC ring tracks emptiness via `head == tail` and fullness by
arranging that `tail + 1 == head`, sacrificing one slot.  We don't
need that trick because we have a real `count`, so the buffer
genuinely holds `PIPE_BUF_SIZE` bytes without losing a slot.

**Refcounts in the object, not external.**  Each `FD_PIPE_R`
descriptor adds one to `r_refs`; each `FD_PIPE_W` adds one to
`w_refs`.  When a side hits zero, *that side*'s threads (the ones
blocked the wrong way around — readers waiting for bytes when no
writers exist, writers waiting for space when no readers exist) get
woken so they can notice the change and bail.  When both hit zero,
`kfree`.  This is the same discipline VFS uses; nothing exotic.

## Reading and writing

`pipe_read` is the canonical condition-loop:

```c
while (p->count == 0) {
    if (p->w_refs == 0) return 0;   /* EOF — no more writers */
    thread_block_on(p);
}
/* Drain up to min(len, count) bytes... */
```

The loop is unconditional: even if `thread_block_on` returns and
the buffer happens to be empty *again* (perhaps another reader got
there first), we re-check.  Spurious wakeups are allowed.  EOF is
not a special wakeup — it's just "writers are gone, nothing's
coming, return zero."

`pipe_write` is the symmetric loop, with one extra wrinkle: it
loops *across* the user's request, possibly blocking multiple times
if the user hands us more than 4 KiB.  Each iteration:

1. If `r_refs == 0`, no point continuing — return `-EPIPE` (or
   the count we already wrote).
2. If full, `thread_block_on(p)` and try again.
3. Otherwise copy as much as the ring will accept, advance, wake
   any blocked reader.

Notice the wake on the *write* path: every time bytes land in the
ring we call `thread_wake_blocked(p)`.  That's the producer-side
poke that tells a parked reader "go look again."  The cost is one
walk of the thread list per write call; we'll regret that in a
benchmark someday, but that day is not today.

## The fd table grows a `kind`

Up to now the fd-table entry distinguished file objects by side
channels: `osfs_size != 0` meant "OSFS-backed," `ramfs_index >= 0`
meant "ramfs-backed," and `ramfs_index == -1 && osfs_size == 0`
meant "console" (and only fds 0/1/2 were allowed to be console).
Pipes don't fit that scheme without further hacks, so we promote
the implicit discriminator into an explicit one:

```c
enum fd_kind {
    FD_CONSOLE = 0,
    FD_FILE,        /* osfs OR ramfs (distinguished by osfs_size != 0) */
    FD_PIPE_R,
    FD_PIPE_W,
};
```

`vfs_read`, `vfs_close`, and `sys_write` now dispatch on `kind`
before falling through to the legacy paths.  Read/write/close on
any pipe fd routes through `pipe_*`; close also calls `pipe_unref`
to drop the right refcount.

## `dup2`

The minimum viable `dup2(oldfd, newfd)` is short:

1. Validate both fds.
2. If `newfd` was open, close it (drops its pipe refcount if
   relevant).
3. Memcpy the source slot into the destination slot.
4. If the kind is a pipe, bump that pipe's matching refcount.

That's enough for what comes next.  The shell will need `dup2` to
graft the read end of a pipe onto a child's `stdin` and the write
end onto a child's `stdout` before `exec`-ing the child, which is
the standard recipe for pipelines.  We deliberately don't implement
the `O_CLOEXEC` flag yet, because we don't have `exec` either — both
arrive in the next milestone.

## The IRQ-mask gotcha, again

Recall from chapter 37 that SVC handlers run with all DAIF bits set
by the architecture, so the timer can't fire and `timer_ticks()`
can't advance from inside a syscall.  Sleep had to unmask `DAIF.I`
across `thread_sleep_ms` to break the deadlock.

Pipes have a similar but slightly different problem: a blocked
reader doesn't depend on the timer to wake — it depends on a
*writer* running and calling `thread_wake_blocked`.  But the writer
is also a user thread, and to wake the reader the writer has to
*also* yield (so the reader gets scheduled).  In a single-CPU world
that's exactly what happens automatically: `pipe_write` calls
`thread_wake_blocked` (which pushes the reader onto the runqueue),
and on the next yield (whether forced by the writer's own
`pipe_write` looping back to block, or by its eventual `close`, or
just by the periodic timer interrupt that fires *between* syscalls)
the reader runs.

So today, with a single-process exerciser doing all the writes and
reads sequentially, IRQ masking inside SVC isn't a problem — the
reader never blocks because the writer ran first and there are
already bytes in the ring.  The interesting case is when reader
and writer are *different threads*, which we'll only see once the
shell can spawn pipelines.  We'll come back to this in the next
chapter and decide whether to unmask `DAIF.I` around blocking
pipe ops the way we did for sleep.

## Verification: `pipetest`

Rather than wait for the shell pipe parser to validate the kernel
side, we write a single-process user binary that exercises every
edge of the new code and prints `[PASS]/[FAIL]` lines.  It runs
13 checks across three scenarios:

1. **Basic round-trip** — open a pipe, write 11 bytes, read them
   back, verify they match.
2. **Close-while-pending** — write a byte, close the writer, prove
   that the reader can still drain the byte before seeing EOF on
   the *next* read.  This exercises the "no writers but bytes
   remain" path in `pipe_read`.
3. **`dup2` keeps the pipe alive** — open a pipe, `dup2(wfd, 5)`,
   close the original `wfd`.  Prove that writes through fd 5 still
   reach the reader, and that closing fd 5 (the last writer) is
   what finally produces EOF.  This exercises refcount bookkeeping
   in `dup2` and `pipe_unref`.

Sample output:

```
/$ pipetest
[PASS] pipe() returned 0
[PASS] pipe() gave two distinct fds >= 3
[PASS] write 11 bytes to pipe wfd
[PASS] read returned 11 bytes
[PASS] read bytes match what was written
[PASS] second write of 1 byte
[PASS] after close(wfd), still drain pending byte
[PASS] read on drained+closed pipe returns 0 (EOF)
[PASS] second pipe() returned 0
[PASS] dup2(wfd, 5) returned 5
[PASS] write to duplicated wfd works after closing original
[PASS] read from rfd after dup'd write returns those bytes
[PASS] after closing duplicated writer too, reader sees EOF
[pipetest] all checks passed
```

All thirteen pass.  The kernel piping layer is correct; the only
case left untested is the genuinely-blocking one (reader and writer
in different threads), and that has to wait until chapter 39 when
the shell can spawn pipelines.

## What this unlocks

Pipes are the first general-purpose blocking I/O primitive in the
system.  Once the shell can drive them they immediately unlock:

- `cmd1 | cmd2` — the headline use case.  Producer side connects
  its `stdout` to the pipe's write end; consumer side connects its
  `stdin` to the read end.  The shell waits for both children.
- Multi-stage pipelines `a | b | c` — a sequence of pipes, each
  child set up the same way.  The shell wires up `n-1` pipes for
  `n` stages.
- A foundation for `select(2)`-style multiplexing, when we want
  to read from many sources at once without spawning a thread per
  source.

Beyond pipes, the same `THREAD_BLOCKED` + `blocked_on` machinery is
the prototype for everything that wants to park a thread on a
condition: socket recv (chapter 38 of the next book), waiting for a
keypress with a deadline, a semaphore, a futex.  We'll lift the
walk into a per-token wait list once we have more than one client.

## What's missing

- **No process-pair test yet.**  Until the shell has `|` parsing,
  the only exercise of `pipe_read`'s blocking branch is artificial.
  We'll close that loop in the next chapter.
- **No `O_NONBLOCK`.**  All reads and writes block.  This is fine
  for the shell, painful for an event loop.  Add a flag word to
  `fd_entry` when something needs it.
- **No `pipe2(flags)`.**  Linux's `pipe2` lets you set `O_CLOEXEC`
  and `O_NONBLOCK` at creation time; we'll grow into that.
- **Walk is O(n).**  Rewrite `thread_wake_blocked` to consume a
  per-pipe wait list when contention shows up.
- **`dup2` doesn't `O_CLOEXEC`-skip on `exec`.**  We don't have
  `exec` yet; once we do, child inheritance becomes a question.
- **`sys_write` to console is still `serial_putc` per byte.**  Not a
  bug, but worth noticing — when a pipeline pushes a megabyte of
  text through a final stage, that's a megabyte of MMIO writes.
  Buffer it later.

## What changed in each file

- `kernel/core/thread.h` — added `THREAD_BLOCKED` to the state
  enum, `void *blocked_on` to the thread struct, prototypes for
  `thread_block_on` / `thread_wake_blocked`.
- `kernel/core/thread.c` — implementations of the two new helpers;
  `blocked_on = NULL` initialised at all three thread-creation
  sites (boot thread, `thread_create`, `user_thread_create`).
- `kernel/core/pipe.h` — new file; pipe ABI.
- `kernel/core/pipe.c` — new file; `pipe_alloc`, `pipe_unref`,
  `pipe_read`, `pipe_write`.  ~110 lines.
- `kernel/core/vfs.h` — added `enum fd_kind` (CONSOLE/FILE/PIPE_R
  /PIPE_W) and the `kind` and `pipe` fields on `struct fd_entry`.
- `kernel/core/vfs.c` — `vfs_init_fdtable` and every `vfs_open` /
  `vfs_open_into` site now set `kind` + `pipe`; `vfs_read`
  dispatches `FD_PIPE_R` to `pipe_read` and rejects `FD_PIPE_W`;
  `vfs_close` calls `pipe_unref` for pipe fds before clearing.
- `kernel/core/syscall.h` — `SYS_PIPE = 22`, `SYS_DUP2 = 23`.
- `kernel/core/syscall.c` — `sys_write` now dispatches pipe-write
  fds through `pipe_write`; new `sys_pipe` allocates a pipe and
  installs the two ends; new `sys_dup2` with refcount management;
  dispatch cases added.
- `userspace/libc/syscall.h` — enum entries plus `pipe()` and
  `dup2()` wrappers.
- `userspace/pipetest/pipetest.c` — new self-test binary.
- `Makefile` — `kernel/core/pipe.c` added to `C_SRCS`; the usual
  `OBJS / ELF / STRIPPED / OSFS_BIN_FILES` block plus the
  `mkosfs.py` invocation extended for `pipetest`.

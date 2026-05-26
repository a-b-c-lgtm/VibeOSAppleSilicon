# Chapter 40 — Shell pipelines: `cat | grep | wc`

> *Code:* additions to `userspace/sh/sh.c`, `kernel/core/syscall.{h,c}`,
> `userspace/libc/syscall.h`, plus a small but important fix to
> `kernel/core/thread.c` (closing fds on exit).
>
> *Milestone covered:* 31 (the pipe story is finally complete).
>
> *Why this chapter is the next one:* the previous chapter built the
> kernel pipe primitive plus `dup2` and a single-process exerciser
> (`pipetest`) that proved the ring buffer, refcounting, and the
> `THREAD_BLOCKED` machinery were correct.  But none of that
> machinery actually *blocks* anyone in `pipetest` — the writes all
> precede the reads, so the buffer is never empty when the consumer
> looks.  This chapter is what finally exercises the blocking path:
> a producer in one process, a consumer in another, with the kernel
> in between scheduling between them.

## What we're building

A POSIX-shaped pipeline parser inside the shell, plus a single new
syscall that lets the shell spawn a child with two of its own fds
mapped onto the child's stdin and stdout.  By the end of this
chapter:

```
$ cat /mnt/poem.txt | wc
14 83 569
$ cat /mnt/poem.txt | grep the | wc
5 44 244
$ cat /mnt/poem.txt | head -3
If you can read this through the kernel's OSFS-1 mount,
the chain works:

```

…all work, with each stage running as its own process and the
kernel correctly blocking the consumer when the buffer is empty
and the producer when it's full.

## The new syscall: `spawn_pipe`

`SYS_SPAWN_REDIR` (chapter 37) was the natural place to grow,
but it took a *path string* for stdin and resolved it inside the
kernel via `vfs_open_into`.  Pipes don't have paths — they're
anonymous objects that already exist in the parent's fd table —
so trying to fit them into `SYS_SPAWN_REDIR` would have meant
overloading the path argument with magic strings ("pipe:3"?
"<fd:3>"?) or a flag bit somewhere.  Both are uglier than just
adding a fourth syscall.  The interface:

```c
int spawn_pipe(const char *path, const char *args,
               int stdin_fd, int stdout_fd);
```

Both fd arguments are interpreted in the *parent's* fd table.
A value of `-1` means "leave the default console in that slot."
Internally the kernel builds the same address-space + ELF-load +
thread-create dance as `sys_spawn`, then calls a small helper
just before the child becomes runnable:

```c
static int dup_parent_fd_into_child(struct thread *parent, int pfd,
                                    struct thread *child, int cfd)
{
    if (pfd < 0 || pfd >= FD_TABLE_SIZE) return -EBADF;
    struct fd_entry *src = &parent->fds[pfd];
    if (!src->in_use) return -EBADF;
    child->fds[cfd] = *src;            /* shallow copy of the entry  */
    child->fds[cfd].in_use = 1;
    /* Bump the underlying object's matching refcount. */
    if (child->fds[cfd].kind == FD_PIPE_R && child->fds[cfd].pipe)
        child->fds[cfd].pipe->r_refs++;
    else if (child->fds[cfd].kind == FD_PIPE_W && child->fds[cfd].pipe)
        child->fds[cfd].pipe->w_refs++;
    return 0;
}
```

The shallow copy is the same trick `dup2` uses; the refcount bump
is what keeps the pipe alive once the parent closes its own copy.

## The shell: parsing `|`

Inside the main loop, after expand_vars and the `time` prefix
handling but *before* the existing `<` handler, we scan `cmd` for
`|`.  If absent, the rest of the loop runs unchanged — single-
command path with optional `<`.  If present, we take the
pipeline branch.

The branch is straightforward:

1. **Split.**  Replace each `|` with a NUL in place, building an
   array of segment pointers.  Hard cap of 8 segments — if the
   user wants more they're doing something weird.
2. **Trim.**  Skip leading/trailing whitespace on each segment.
   Reject empty segments (`||`, leading `|`, trailing `|`).
3. **Allocate.**  Call `pipe()` `n-1` times.  If any fail, close
   what we got and bail.
4. **Spawn.**  For each segment, split off the first whitespace-
   separated token as the path, the rest as the args.  The fd
   wiring rule:

   - First segment: stdin = `-1` (default console),
     stdout = `pipes[0][1]` (write end of the first pipe).
   - Middle segments: stdin = `pipes[i-1][0]` (read end of the
     pipe coming in), stdout = `pipes[i][1]` (write end of the
     pipe going out).
   - Last segment: stdin = `pipes[n-2][0]`,
     stdout = `-1` (default console).

5. **Close all pipe fds in the shell.**  This is the step that
   turns a hang into a working pipeline.  More on it in a moment.
6. **Wait.**  Call `wait()` once per spawned child.  The shell
   doesn't track which tid was the rightmost stage, so we
   record the last reaped exit code as `$?` — close enough to
   POSIX for now.

## The hang that taught us about fd inheritance

First end-to-end test:

```
$ cat /mnt/poem.txt | wc
[sys_exit] thread '/bin/cat' exited with code 0x0
… nothing more ever prints.
```

`cat` finished its work, wrote everything into the pipe's write
end, and exited.  But `wc` never printed anything.  When QEMU was
killed and we looked at the trace, `wc` was still alive, blocked
in `pipe_read`, waiting for EOF that never came.

Why?  The pipe object's `w_refs` was supposed to drop to zero when
the last writer closed, and `pipe_read` was supposed to notice and
return 0.  But `cat` had crashed out with a `thread_exit(0)` call
that never closed any of its fds.  So the kernel's view was: "Cat
is dead, but cat's old fd 1 (the pipe write end) is still 'in
use' from the kernel's perspective, so `w_refs` is still 1."  The
pipe stayed open from the writer side forever.

The fix is one new line in `thread_exit`:

```c
void thread_exit(int code)
{
    g_current->exit_code = code;
    g_current->state     = THREAD_EXITED;

    /* Close every fd before we tear down — pipe refcounts must
     * drop so the other side of any pipe sees EOF / -EPIPE. */
    vfs_close_all(g_current);
    ...
```

…and a small new helper in vfs.c:

```c
void vfs_close_all(struct thread *t)
{
    for (int fd = 0; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &t->fds[fd];
        if (!e->in_use) continue;
        if (e->kind == FD_PIPE_R && e->pipe) pipe_unref(e->pipe, PIPE_REF_R);
        else if (e->kind == FD_PIPE_W && e->pipe) pipe_unref(e->pipe, PIPE_REF_W);
        /* clear slot... */
    }
}
```

This is one of those bugs that's obvious in hindsight (POSIX has
mandated "close all fds on exit" since 1988) but easy to skip in a
hobby kernel where for many milestones nothing was reference-
counted.  Pipes are the first object in this kernel where the
"who's still holding a reference?" question has a non-trivial
answer.

## The other hang: shell holds the pipe open

Even with `vfs_close_all` in place, there's a second way to hang
the pipeline.  Recall step 5 above: "Close all pipe fds in the
shell."  Why?

Trace through what the shell does for `cat | wc`:

1. Shell calls `pipe(&fds[3], &fds[4])`.  In the shell's table:
   `fds[3]` = pipe read end, `fds[4]` = pipe write end.  Pipe
   refcounts: `r_refs = 1, w_refs = 1` (one each, both held by
   the shell).
2. Shell calls `spawn_pipe("/bin/cat", "/mnt/poem.txt", -1, 4)`.
   The kernel duplicates the shell's fd 4 into cat's fd 1,
   bumping `w_refs` to 2.  Cat's stdout is the pipe.
3. Shell calls `spawn_pipe("/bin/wc", "", 3, -1)`.  Kernel
   duplicates the shell's fd 3 into wc's fd 0, bumping
   `r_refs` to 2.  Wc's stdin is the pipe.
4. Shell waits.

If we forget step 5, `r_refs = 2` and `w_refs = 2`.  Cat finishes,
calls `vfs_close_all`, drops `w_refs` to 1.  But the shell still
holds the other writer.  `wc` keeps reading until the pipe
empties, then blocks waiting for the next byte — which never
comes, because the only "writer" left is the shell, which is in
`wait()` and never going to write anything.

The fix is the canonical one: after spawning every pipeline child,
the shell closes its own copies of all the pipe fds.  Now each
end of the pipe is held by exactly one process — the producer
holds the writer, the consumer holds the reader — and the
refcount semantics produce the right behaviour: cat finishes,
`w_refs` drops to 0, wc's read returns 0, wc exits.

This is the same dance every Unix shell has done since 1973 and
why every Unix shell tutorial spends two pages on it.  We deserve
the same two paragraphs.

## DAIF, again — but it doesn't bite this time

Chapter 38 introduced `daifclr`/`daifset` around `thread_sleep_ms`
because SVC handlers run with `DAIF.I = 1` (the architecture
auto-masks IRQs on exception entry), so the timer can't fire from
inside a syscall.  The same logic ought to apply to a blocked
pipe consumer: enter `sys_read` with IRQs masked, block, never
get scheduled because the timer can't preempt anyone.

Why doesn't it bite?  Look at `cswitch_to` in
[`kernel/arch/context_switch.s`](../../../kernel/arch/context_switch.s):
when a thread voluntarily yields, the saved SPSR is hardcoded to
`0x345` — `D=1, A=1, I=0, F=1`.  When that thread is later
resumed via `eret`, PSTATE is loaded from SPSR_EL1, so `DAIF.I`
becomes 0 even though it was 1 on the way in.  In effect, every
voluntary cswitch has the side effect of unmasking IRQs on the
next time the saved thread runs.

The producer side is even simpler: if the producer doesn't block,
it returns from its SVC, the architectural `eret` restores the
user's SPSR (which had I=0 because user threads run with IRQs
unmasked), and the timer can preempt.  Only if the producer
blocks (pipe full, no space) do we enter the same case as the
consumer, which is also fine because of `cswitch_to`'s SPSR
override.

The interesting question is whether *another* unrelated thread
could be scheduled with `I=1` somehow, but our thread-create paths
all set up entry SPSRs with `I=0`, and `cswitch_to` always saves
`I=0`, so we're consistent.  This is the kind of invariant that
would need a written-down assertion if we ever grew SMP or signals
or multiple priority levels — for the current single-CPU
cooperative+preemptive mix, it just works.

## Verification

A focused suite covering each important shape:

```
$ cat /mnt/poem.txt | wc
14 83 569

$ cat /mnt/poem.txt | grep the | wc
5 44 244

$ cat /mnt/poem.txt | head -3
If you can read this through the kernel's OSFS-1 mount,
the chain works:

$ echo hello world | wc
1 2 12
```

Each row exercises a different shape:

- **2-stage**: simplest case; one pipe, two children.
- **3-stage**: tests that intermediate stages correctly inherit
  *both* an inbound pipe-read fd and an outbound pipe-write fd,
  and that closing the right fds in the shell still produces
  EOF in the right order.
- **head -3 ends early**: head closes stdin and exits as soon as
  it has 3 lines, which means cat's pipe write should fail with
  `-EPIPE` on the next attempt.  Cat doesn't currently check the
  return value of write, so it just keeps going until the file
  ends — but the kernel side correctly returns `-EPIPE` and
  doesn't crash.  (When we add SIGPIPE later this becomes a real
  termination signal; for now, cat finishes, writes are silently
  dropped, and head's output is correct.)
- **echo | wc**: tests that a "small" producer (one line, exits
  after 12 bytes) correctly produces EOF for the consumer to see.

## What this unlocks

Pipelines are foundational.  They turn the shell from a one-shot
command launcher into a programmable text processor.  Every Unix
small tool (sort, uniq, awk, sed, jq, …) only makes sense in the
context of pipelines — they're the assembly instructions of the
shell.  Now that we have them:

- Future tool combinations like `ls | wc -l`, `cat /mnt/log |
  grep ERROR | head` Just Work.
- The networking layer (chapter 39 in the next book) can use the
  same `THREAD_BLOCKED + blocked_on` machinery for socket recv —
  pipes were really just a warm-up for the same pattern with a
  different wakeup source.
- The `<` redirection from chapter 37 and the `|` pipeline
  parser here are the foundation for `>` (output redirection)
  whenever we add a writable filesystem.

## What's missing

- **No `<` mixed with `|`.**  The pipeline branch runs *before*
  the `<` parser, so `cat < /mnt/poem.txt | wc` doesn't currently
  work — the `<` is interpreted as part of `cat`'s args.  A real
  shell parses `<` and `|` together; we'll get there.
- **No `>` redirection at all.**  Blocked on writable storage.
- **No SIGPIPE.**  A producer whose consumer has gone away gets
  `-EPIPE` on its next write, but we have no signal-delivery
  mechanism, so the producer must check the return value
  manually.  `cat`/`echo`/etc. don't.  Symptom is invisible (the
  bytes are silently lost) until we have signals.
- **`wait()` doesn't track per-child status.**  We use the
  exit code of the last-reaped child as the pipeline's `$?`,
  but POSIX says it should be the rightmost stage's status.
  Order of completion isn't deterministic, so this is wrong
  in general but right often enough.  Fix is to make
  `wait()` take a tid and wait for *that* child specifically.
- **Pipeline depth capped at 8.**  Compile-time constant
  `MAX_SEGMENTS = 8` in sh.c.  Bump it if you ever find a real
  use case for ≥ 9 stages.
- **Pipe parser is not quote-aware.**  `echo 'a | b'` will be
  interpreted as a pipeline.  Same caveat as `<`.  Fix is to
  defer the `|` scan until after a single quote-aware tokeniser
  pass; that's a fair amount of work and we'll bundle it with
  `<`'s same fix.
- **Pipe buffer is 4 KiB.**  Linux defaults to 64 KiB, FreeBSD
  to 16 KiB.  4 KiB just means more context switches when
  pumping large amounts of data; correctness is fine.

## What changed in each file

- `kernel/core/syscall.h` — `SYS_SPAWN_PIPE = 24`.
- `kernel/core/syscall.c` — `dup_parent_fd_into_child` helper
  plus `sys_spawn_pipe` (~80 LOC body sharing structure with
  `sys_spawn_redir`); dispatch case.
- `kernel/core/thread.c` — `vfs_close_all(g_current)` call
  added to the top of `thread_exit`.
- `kernel/core/vfs.h` — prototype for `vfs_close_all`.
- `kernel/core/vfs.c` — `vfs_close_all` implementation that
  walks all 16 fd slots and drops pipe refcounts before
  clearing.
- `kernel/core/main.c` — banner bumped to milestone 31.
- `userspace/libc/syscall.h` — `SYS_SPAWN_PIPE` enum entry +
  `spawn_pipe(path, args, stdin_fd, stdout_fd)` wrapper using
  `_svc4`.
- `userspace/sh/sh.c` — pipeline branch (~120 LOC) inserted
  between `time` handling and the `<` parser; help text mentions
  pipelines.

# Chapter 114f — Step 6: per-request timeouts and deadlock detection

Chapter 114e ended with a warning. Userfs is now load-bearing
on the boot path, and a wedged daemon — one that takes its
request fd, never reads it, and never writes a reply — would
freeze every `cat /proc/...`, every `ps`, every shell that
tabs into a userfs mount, until reboot.

This chapter closes that loop. The kernel side of
`userfs_call` learns a per-request deadline. If a request
spends more than 5 seconds in flight (header write + payload
write + header read + payload read combined), the call
returns `-ETIMEDOUT_VFS` (=110), the channel is marked dead,
every subsequent request short-circuits to `-EIO`, and the
mount stays in the mount table as a tombstone instead of
being mistakenly re-used.

The deadlock half — a daemon recursing into its own mount —
was already covered by the `owner_pid` check that landed in
[chapter 114a](114a-kernel-userfs-module.md). This chapter
documents that mechanism alongside the timeout work so
"step 6" is one place in the book, not two.

## What got added

| Path | Change |
| --- | --- |
| `kernel/core/vfs.h` | `#define ETIMEDOUT_VFS 110` (matches Linux's `<errno.h>`) |
| `kernel/core/thread.h` / `.c` | new `thread_block_on_until(token, deadline_ms)`; yield-time walker now wakes `THREAD_BLOCKED` threads past their deadline as well as `THREAD_SLEEPING` ones |
| `kernel/core/pipe.h` / `.c` | new `pipe_read_until` / `pipe_write_until`; the old `pipe_read` / `pipe_write` become one-line wrappers passing deadline 0 |
| `kernel/core/userfs.c` | `userfs_call` snapshots a deadline at entry and threads it through all four pipe ops; timeout propagates as `-ETIMEDOUT_VFS` |
| `userspace/hangfs/hangfs.c` | new 3-line daemon that mounts `/hang` and sleeps forever without ever servicing its pipe (test fixture) |
| `scripts/test_userfs_timeout.py` | new regression: opens `/hang/anything`, asserts errno=110 within 5–12 s, asserts shell stays alive, asserts a second open short-circuits |
| `Makefile`, `book/INDEX.md` | wire hangfs into the disk image and the chapter index |

Net new kernel: ~70 lines. Net new userspace: ~30 lines
(daemon) + ~200 lines (test). The kernel-side timeout
mechanism is so small because it reuses two existing
primitives — `wake_at_ms` (the field that drives
`thread_sleep_ms`) and `thread_wake_blocked` (the path that
already gets called when a pipe gets data) — instead of
introducing a new scheduler state or a per-request watchdog
thread.

## The deadline-aware block primitive

The fundamental thing the scheduler did not have was a way
to wake a `THREAD_BLOCKED` thread because *time passed*. We
had two separate sleep flavours:

- `thread_block_on(token)` — sets `state=BLOCKED`,
  `blocked_on=token`, yields. Woken by
  `thread_wake_blocked(token)`. **No timeout.**
- `thread_sleep_ms(ms)` — sets `state=SLEEPING`,
  `wake_at_ms = now + ms`, yields. The yield-time walker
  re-readies sleepers when the wall clock passes their
  deadline.

`thread_block_on_until` unifies them by reusing the
`wake_at_ms` field on a `THREAD_BLOCKED` thread:

```c
void thread_block_on_until(void *token, uint64_t deadline_ms)
{
    g_current->blocked_on = token;
    g_current->wake_at_ms = deadline_ms;
    g_current->state      = THREAD_BLOCKED;
    yield();
    g_current->wake_at_ms = 0;   /* leave no stale deadline */
}
```

The yield-time walker grew one extra `else if` branch:

```c
if (t->state == THREAD_SLEEPING && now >= t->wake_at_ms) {
    /* unchanged sleeper path */
} else if (t->state == THREAD_BLOCKED &&
           t->wake_at_ms != 0 && now >= t->wake_at_ms) {
    t->blocked_on = NULL;
    t->state      = THREAD_READY;
    runq_push_to(t);
}
```

Three properties fall out of this design that we lean on:

1. **A deadline-driven wake looks exactly like a normal
   wake.** The caller sees `state=RUNNING`, `blocked_on=NULL`,
   and has to re-check its condition. There is no separate
   `-ETIMEDOUT` return path from the block primitive itself —
   that's the caller's job, by comparing the wall clock to
   the deadline it saved before blocking.

2. **`wake_at_ms == 0` means "no deadline."** The plain
   `thread_block_on` doesn't touch the field, so its callers
   keep the prior behaviour. The walker's `wake_at_ms != 0`
   guard means it only ever times out threads that *opted
   in* via the new entry point.

3. **No new scheduler state.** Both `THREAD_SLEEPING` and
   `THREAD_BLOCKED` continue to mean exactly what they
   meant. The walker's branch is a one-line addition, not a
   new state machine.

The price is that a `THREAD_BLOCKED` thread parked via the
deadline-aware variant now lives on both wake paths: it can
be woken by `thread_wake_blocked(token)` (the resource
became available) *or* by the walker (the wall clock passed
the deadline). Either path clears `blocked_on` and
re-readies the thread, so the caller must re-check both its
resource condition AND the wall clock to know which
happened — exactly the loop already used by
`pipe_read_until` below.

## Deadline-aware pipe ops

`pipe_read_until` and `pipe_write_until` are the wrappers
that connect `userfs_call` to the deadline mechanism. The
read variant is the interesting one:

```c
long pipe_read_until(struct pipe *p, void *buf, size_t len,
                     uint64_t deadline_ms)
{
    while (p->count == 0) {
        if (p->w_refs == 0) return 0;   /* EOF */
        if (deadline_ms != 0) {
            uint64_t now = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
            if (now >= deadline_ms) return -ETIMEDOUT_VFS;
        }
        if (deadline_ms != 0)
            thread_block_on_until(p, deadline_ms);
        else
            thread_block_on(p);
        if (p->count > 0) continue;     /* prefer data over signal */
        struct thread *me = thread_current();
        if (me && me->sig_pending) return -EINTR_PIPE;
        /* loop: re-check w_refs, count, and the deadline */
    }
    /* drain ... */
}
```

Three things to note:

- **The deadline check comes BEFORE we block** as well as
  after. If a previous call already burned the budget on a
  slow write, the read won't even park the thread once —
  it returns `-ETIMEDOUT_VFS` immediately.

- **The prefer-data-over-signal rule from
  [the pipe_read fix](../../../memories/repo/pipe-read-prefer-data-over-signal.md)
  is preserved.** A signal landing while data is also waiting
  still drains the data first. That fix predates this
  chapter and remains the contract.

- **`deadline_ms == 0` means "no deadline"** — `pipe_read`
  becomes a one-line wrapper passing 0. Nothing outside
  userfs needed to change.

`pipe_write_until` follows the same shape; the only twist
is that a partial write that hits the deadline reports the
bytes already moved instead of `-ETIMEDOUT_VFS`, matching
POSIX's "short writes are OK" rule.

## Threading the deadline through `userfs_call`

`userfs_call` snapshots the absolute deadline once at the
top of the function and passes the same value to all four
pipe ops:

```c
uint64_t deadline = timer_ticks() * (uint64_t)TICK_INTERVAL_MS
                  + (uint64_t)USERFS_CALL_DEADLINE_MS;  /* 5000 */

long w = pipe_write_until(c->req_pipe, &req, sizeof req, deadline);
if (w != (long)sizeof req) {
    c->alive = 0;
    c->in_flight = 0;
    thread_wake_blocked(&c->in_flight);
    return (w == -ETIMEDOUT_VFS) ? -ETIMEDOUT_VFS : -EIO;
}
/* ...same shape for payload write, header read, payload read... */
```

The single-snapshot matters. A naive design that gave each
of the four ops its own 5-second budget would let a
half-wedged daemon stall a single client for up to 20 s in
the worst case. With one shared deadline, the entire round
trip is bounded by 5 s regardless of where in the protocol
the daemon stalls.

On timeout we do two things:

1. **Set `c->alive = 0`.** Every subsequent call against
   the same channel short-circuits at the top of
   `userfs_call` with `-EIO`. The mount stays in the mount
   table (so `ls /` still lists it and the daemon can be
   killed and respawned by `init`'s supervisor), but no
   pipe operation will ever block on it again.

2. **Wake the in-flight slot.** Any other thread parked in
   `userfs_call`'s contention loop sees the wake, re-checks
   `c->alive`, and exits with `-EIO`. The serialisation
   lock doesn't become a graveyard.

The errno-routing ternary
(`(w == -ETIMEDOUT_VFS) ? -ETIMEDOUT_VFS : -EIO`) means the
*first* caller to see the timeout learns it was a timeout
(useful for logging and tests); every subsequent caller
sees `-EIO` because the channel is dead. That's the right
shape — only the first failure carries the diagnostic
information; later failures are derivative.

## The deadlock detector (already shipped in 114a)

`userfs_call` rejects re-entry from the daemon thread that
owns the channel:

```c
struct thread *me = thread_current();
if (me && c->owner_pid > 0 && me->id == c->owner_pid)
    return -EDEADLK;
```

`owner_pid` is the pid that called `sys_mount` to install
the channel (snapshotted in `sys_mount`, see
[chapter 114b step 3](114b-sys-mount-umount.md)). If procd
ever tried to `open("/proc/<self>/status")` from inside one
of its own callbacks, the call would deadlock against
`in_flight`. Catching the recursion at entry turns it into
a clean errno before we even take the serialisation lock.

What this catches:
- **Direct recursion.** procd → procd. clipboardd →
  clipboardd. The common case.

What this does NOT catch:
- **Transitive recursion.** procd opens `/clipboard/foo`
  while clipboardd is inside a callback that wants to open
  `/proc/<procd-pid>/status`. Each `userfs_call` sees a
  different owner_pid and lets the request through; both
  threads then sit on opposite in_flight slots and deadlock.

The timeout half of this chapter is the safety net for the
transitive case. After 5 s, both calls time out, both
channels go dead, and `init`'s supervisor respawns both
daemons. The user sees a 5 s pause and a `cat` that exits
with `errno=110`, which is a far better failure mode than a
silent freeze. A proper transitive-deadlock detector (a
wait-for graph keyed on `owner_pid`) is a future chapter —
we have not seen this happen in any real test, only in
adversarial constructed scenarios.

## The test fixture

`userspace/hangfs/hangfs.c` is the smallest possible test
client. It does NOT call `userfs_serve`. It mounts `/hang`
directly via `mount_kernel(...)` and then sleeps forever:

```c
int fds[2];
long r = mount_kernel("/hang", fds, 0);
if (r < 0) { printf("hangfs: mount_kernel -> %ld\n", r); return 1; }
printf("hangfs: mounted /hang (mount_id=%ld), going to sleep\n", r);
for (;;) sleep_ms(1000);
```

That's the entire program. The request pipe fills up with
incoming p9 headers and nobody ever drains it; the reply
pipe never gets a single byte written. Any client that
opens a file under `/hang/` sits in `userfs_call`'s reply
read for exactly 5 seconds and then returns
`-ETIMEDOUT_VFS`.

We deliberately did NOT route this through `libfs`. The
serve loop in `libfs` *does* drain the request pipe (that's
the whole point of `libfs`), so a libfs-based hangfs would
need a per-handler trick to never reply. Doing the test
fixture at the raw `mount_kernel` level makes the failure
shape crystal clear and dodges a chapter's worth of "we
introduced a `SIMULATE_HANG` flag" exposition.

## What the test asserts

`scripts/test_userfs_timeout.py` boots the OS, spawns
`/bin/hangfs &`, and then makes six assertions in order:

1. **hangfs reports its mount line.** `hangfs: mounted
   /hang` shows up on serial, proving `sys_mount` accepted
   the registration.

2. **`cat /hang/anything` returns `errno=110`.** This is
   the headline: the deadline fires and the kernel returns
   `-ETIMEDOUT_VFS`. cat's open-failure printer formats it
   as `errno=110`.

3. **The elapsed time is between 4.5 s and 12 s.** The
   kernel deadline is 5 s; the wide upper bound is a
   robustness margin for slow CI hosts. In the local run
   on a 2-CPU HVF VM, the actual elapsed is 5.08 s.

4. **The shell is still responsive after the timeout.**
   A `/bin/echo alive-after-timeout` round-trips
   successfully. cat exited non-zero; the shell did not.

5. **A second open against /hang short-circuits.** Now
   that the channel is dead, the second
   `cat /hang/another` returns `errno=5` (not 110) and
   does so in under 4 s — no second 5-second wait.

6. **The second errno is NOT 110.** This proves the
   timeout fires exactly once per channel; subsequent
   failures correctly report `-EIO`.

Local result: **6 PASS / 0 FAIL**, total wall time ~14 s
including boot.

## What this unlocks

Per the apps-must-use-features discipline:

- **Existing app(s) modified to use the feature**: none.
  The timeout is entirely below the syscall ABI; userspace
  callers see the same `open`/`read`/`write` interface,
  just with a new errno (`110`) they may receive from a
  userfs path. Apps that already check `if (fd < 0)` will
  treat the timeout the same as any other open failure,
  which is the correct behaviour for v1.

- **New app(s) added**: `userspace/hangfs/hangfs.c` — a
  test fixture, not a tool a user would run, but kept as a
  real `/bin/hangfs` binary so the failure mode is
  reproducible without any kernel-internal toggle.

- **Existing test scripts upgraded**: none — every prior
  userfs test (test_userfs_echo, test_clipboard,
  test_procfs, test_strace, test_mounts, test_mount_ro)
  passes unchanged. The timeout never fires on a
  well-behaved daemon (procd's slowest responder, the
  strace renderer, runs in tens of ms).

- **New test scripts added**:
  `scripts/test_userfs_timeout.py` — the six-assertion
  regression described above.

## Why not a watchdog thread

The design rejected was: spawn one kernel thread per
`userfs_channel` that sleeps for 5 s, then if the channel
is still `in_flight` and the request tag hasn't changed,
close the rsp_pipe to unblock the calling thread with EOF.

Three reasons it lost:

1. **Per-channel thread overhead.** Every mount would carry
   a kernel thread for the life of the mount. Today we have
   3 userfs mounts (`/clipboard`, `/proc`, `/echo` when
   echofs is spawned); a future system with dozens of
   mounts would carry dozens of mostly-idle watchdogs.

2. **Wake-by-EOF is ambiguous.** A pipe wake on EOF (writer
   closed) means "the channel is permanently broken." A
   pipe wake on data means "the daemon replied." Mixing
   timeout-driven EOF into that vocabulary would force
   every pipe consumer (not just `userfs_call`) to
   distinguish "the writer closed" from "the watchdog
   closed it on my behalf." The deadline-aware block keeps
   the timeout signal local to the call that set it.

3. **The deadline-aware block costs one field check per
   yield-walker iteration.** That walk already happens
   unconditionally; we added one branch to its loop body.
   The marginal cost is unmeasurable.

## Side-effects for the next chapter

One. The chapter-114 plan listed step 6 as the last
implementation step, but in writing this chapter we
discovered that the deadline machinery has a natural reuse
target outside userfs: every blocking pipe wait in the
codebase (the shell's `read` loop, gui_term's input pump,
the IPC bus's `srv_accept`). None of those currently have a
way to time out. The deadline-aware block + pipe primitives
are the building blocks for adding `poll`/`select` with a
timeout in a future chapter; we are NOT going to do that
work here, but it is now a one-day change instead of a
one-week change.

The transitive-deadlock detector is also a future-chapter
candidate. The 5 s timeout makes the transitive case
self-resolving, so the urgency is gone. We'll revisit it
when (if) a real workload hits the deadlock and the user
sees the 5 s pause as a bug rather than a safety net.

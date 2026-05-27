# Chapter 102 — strace via /proc/&lt;pid&gt;/trace

> **Milestone in this chapter:** 100 — per-thread syscall ring +
> `/bin/strace`.
> **Code referenced:**
> - [kernel/core/strace.c](../../../kernel/core/strace.c)
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_TRACE_ME` and the trace branch in `svc_dispatch`)
> - [userspace/strace/strace.c](../../../userspace/strace/strace.c)
>
> **At the end of this chapter** you will have a 64-entry syscall
> ring buffer per traced thread, a `/proc/<pid>/trace` leaf that
> drains it on read, a one-argument `SYS_TRACE_ME` self-opt-in, and
> a 150-line `/bin/strace` wrapper that fork+exec's a child and
> streams its syscalls to stdout.

Chapter 101 surfaced live process state through a read-only
pseudo-filesystem at `/proc`. The same shape — *the kernel renders
text into a per-open snapshot, userspace just reads it* — extends
naturally into the next observability tool every Unix has:
`strace`. A traced program's syscalls appear as a textual log under
`/proc/<pid>/trace`, and a tiny `/bin/strace` wrapper turns the
polling into a streaming view.

This chapter is short on new mechanism and long on *reusing*
mechanism. The deltas are:

1. A 64-entry ring buffer per traced thread.
2. One branch in `svc_dispatch` to record entries.
3. A new procfs leaf, `/proc/<pid>/trace`, that drains the ring on
   read.
4. A self-trace syscall, `SYS_TRACE_ME`, with no arguments.
5. A 150-line `/bin/strace` that forks → `trace_me()` → `execv()`
   → polls procfs.

Total kernel diff: ~280 lines plus seven single-line edits to six
existing creation/teardown sites in `thread.c`.

```sh
$ strace /bin/echo hello
hello
21.60 execv(0x103ffffff0, 0x103fffffc0) = 0
21.60 write(1, 0x103ffffeb8, 5) = 5
21.60 write(1, 0x103ffffeb8, 1) = 1
21.60 exit(0) = ?
strace: + exited with code 0
```

That's the user-facing target. The rest of this chapter is
about why each design choice points where it does.

## What this chapter adds

- `kernel/core/strace.h` — public API: `strace_enable`,
  `strace_release`, `strace_enter`, `strace_render_and_drain`.
- `kernel/core/strace.c` — the ring, the formatter, and the
  syscall metadata table (66 entries; one per known SVC).
- A `struct strace_ring *strace` field on `struct thread`,
  initialised to `NULL` at every creation site, freed at
  every reap site, *not* inherited across fork.
- `SYS_TRACE_ME = 80` in `kernel/core/syscall.h` and the
  matching enum entry plus a one-line wrapper in
  `userspace/libc/syscall.h`.
- A `case SYS_TRACE_ME` in `svc_dispatch`, plus an
  `_tr = strace_enter(...)` reservation block before the
  `switch` and a `_tr->ret = ret; _tr->completed = 1`
  back-fill after it.
- A `"trace"` entry in `PROCFS_PID_LEAVES[]` and a
  `thread_strace_render_pid()` helper in `thread.c` that
  takes `g_all_lock` so the target thread cannot exit while
  we render.
- `userspace/strace/strace.c` — `/bin/strace`, the polling
  driver.
- `scripts/test_strace.py` — boot, drop into the shell,
  run `strace /bin/echo hello`, assert a recognisable
  syscall name and the `exited with code` line.

## Prerequisites

- Chapter 13 — SVC and the syscall ABI. The dispatcher in
  `svc_dispatch` is where we inject the tracer hook.
- Chapter 16 — `init`, `spawn`, `wait`. We need fork+exec
  semantics for `/bin/strace` to attach to a child.
- Chapter 101 — procfs. Chapter 102 borrows its
  *render-once-on-open, slice-on-read* pattern wholesale,
  plus its lock discipline and its FD_PROCFS no-inherit
  rule.

## Design decisions

### Per-thread ring, allocated on demand

The tracer state is `struct strace_ring`: 64 entries
(`STRACE_RING_CAP`) of about 80 bytes each, plus three
`uint32_t` head/tail/lost counters — under 5 KiB total.
Storing it inline in `struct thread` would bloat every
thread's record by 5 KiB whether the thread is traced or
not, so we hang it off a pointer (`struct strace_ring
*strace`) that is `NULL` until `SYS_TRACE_ME` runs:

```c
int strace_enable(struct thread *t)
{
    if (!t) return -1;
    if (t->strace) return 0;             /* idempotent */
    struct strace_ring *r = kmalloc(sizeof(*r));
    if (!r) return -1;
    /* explicit field init — see "memset trap" below */
    r->head = r->tail = r->lost = 0;
    for (int i = 0; i < STRACE_RING_CAP; i++) {
        r->entries[i].completed = 0;
        r->entries[i].syscall_no = 0;
        r->entries[i].ret = 0;
        r->entries[i].ts_ms = 0;
        for (int j = 0; j < 6; j++)
            r->entries[i].args[j] = 0;
    }
    t->strace = r;
    return 0;
}
```

The hot path is one branch:

```c
if (t->strace) {
    _tr = strace_enter(t, num, a0, a1, a2, a3, a4, a5);
}
```

`strace_enter` is a no-op the moment the test fails. For
every untraced thread (the common case) the cost of the
tracer is one already-cached pointer load and a predictable
not-taken branch.

Why 64 entries? Because `cat /proc/<pid>/trace` polls every
20 ms (see `pump_trace_once` below) and even a chatty
program rarely makes more than a few dozen syscalls in 20 ms
on this kernel. If it does, the ring overwrites and we
prepend `(N entries lost)\n` to the next render so the user
knows.

### Lock-free single-writer, drain-on-read

The tracer ring has exactly one writer — the traced thread
itself, mid-SVC. By the time `svc_dispatch` calls
`strace_enter`, the SVC has already raised EL1 and disabled
preemption for this thread on this CPU. The thread cannot
re-enter `strace_enter` until its current SVC returns, so
the head bump:

```c
uint32_t idx = r->head & (STRACE_RING_CAP - 1);
struct strace_entry *e = &r->entries[idx];
if (r->head - r->tail >= STRACE_RING_CAP) {
    r->lost++;
    r->tail++;
}
/* fill e->args, e->syscall_no, e->ts_ms */
r->head++;
return e;
```

…needs no lock. Concurrent readers (a `cat /proc/<pid>/trace`
running on another CPU) only ever advance `tail`, so the
worst case is a torn render where one entry slips between
"snapshot head" and "slot is fully written" — completely
harmless, because we mark each slot `completed = 0` until
the dispatcher's tail sets `completed = 1`, and
`render_entry` prints `= ?` for un-completed entries.

The reader, in contrast, is *not* under the same locking
guarantees: a `cat` could be on either CPU. We protect it by
holding `g_all_lock` for the duration of the formatter
(in `thread_strace_render_pid`), which prevents the target
thread from being freed mid-render. The formatter itself
takes no other locks and runs on a stack-bounded 8 KiB
output buffer (the procfs file cap), so the window is
bounded and small.

### Drain on read, not peek

Each `read(fd, ...)` of a freshly opened
`/proc/<pid>/trace` returns whatever is in the ring at *open
time* — and advances `tail` to that point. Subsequent reads
on the same fd return the empty tail of the snapshot
buffer. This matches procfs's snapshot-on-open semantics
(chapter 101) exactly: open, render, slice; `tail` is the
"how far did we render" cursor.

Why drain instead of peek? Because `/bin/strace` is the
typical caller, and it wants exactly-once delivery: a
syscall that's been printed once should not be printed
again on the next poll. The kernel ring is the source of
truth; the userspace tool is stateless. If you want a peek
without drain you can `cat` it, then re-`cat` it; the
second cat will get whatever entries piled up in the ~20 ms
since the first one finished. That's strictly better than
exposing a `peek` flag.

### Overwrite on full, count the loss

When a traced program syscall-bombs faster than the reader
drains, the producer overwrites the oldest entry and bumps
`lost`. The renderer prepends `(N entries lost)\n` to the
next output so the user knows the trace is incomplete:

```c
if (lost) {
    sf_puts(out, cap, &pos, "(");
    sf_putu(out, cap, &pos, (uint64_t)lost);
    sf_puts(out, cap, &pos, " entries lost)\n");
    r->lost = 0;
}
```

The alternative — block the traced thread when the ring
fills — would couple the tracer's correctness to whether
anyone is reading the trace. A program that runs `strace`
once and exits the reader would then deadlock the next time
the traced thread tried to syscall.

### Snapshot-at-open over the writer side too

Inside `strace_render_and_drain`, we sample `r->head`
*once* and walk to that point. Anything the traced thread
writes after we sampled stays in the ring for the next
render, even if the writer races us:

```c
uint32_t head = r->head;       /* sample once */
uint32_t tail = r->tail;
while (tail != head) { ... tail++; }
r->tail = tail;                /* drain only what we rendered */
```

If the writer adds 5 entries after our sample but before we
finish the formatter, those 5 entries survive into the next
`cat`. If we instead re-read `r->head` inside the loop, we'd
risk an infinite render against a syscall-spamming child.

### Render in the kernel, not the user

The kernel renders entries to text; userspace just relays
bytes to stdout. This is the same trade-off chapter 101 made
for `/proc/uptime` and friends, and the reasons carry over:

- The kernel knows the syscall numbers, names, and
  arities. We have a `SYSCALL_META[]` table (66 entries,
  hand-maintained) that maps `SYS_OPEN → ("open", 2)` and
  similar.
- The kernel can format pointer-typed arguments as
  `0x%lx` without ever dereferencing them. Doing decoded
  string dumps (the `-s` flag of real `strace`) would
  require kernel-side `copy_from_user` of an unknown user
  pointer — possible, but a security-and-privacy review's
  worth of work that we don't need today.
- Userspace `/bin/strace` is just a fork + execv + poll
  loop. No parser, no formatter, no per-syscall knowledge.

Decoders and pretty-printers can come later, in a future
`strace -s` flag, layered on top of the kernel's
already-decoded line.

### `SYS_TRACE_ME`, not `ptrace_attach`

POSIX `strace` uses `ptrace(PTRACE_ATTACH, pid, ...)` to
let one process trace another. We instead provide
`SYS_TRACE_ME`: a thread can opt into being traced, but
nothing else can attach to it. The `/bin/strace` driver is
forced into a fork-then-self-trace pattern:

```c
int kid = fork();
if (kid == 0) {
    trace_me();
    execv(argv[1], &argv[1]);
}
```

That's enough for the typical `strace prog ...` use case.
Attaching to a long-running process by pid (`strace -p
12`) requires either:

- A capability check (we have no concept of permissions
  yet — every user is root), or
- A way for the target thread to consent (e.g. an
  `accept_trace_request()` syscall it must have called).

Both are interesting follow-ups, neither is necessary for
shipping the basic tool. The opt-in model also avoids
exposing a way for a malicious binary to read another
process's syscall stream.

### Tracing follows exec, *not* fork

When the traced thread `execv()`s, the kernel keeps the
ring: `sys_exec` swaps the address space and renames the
thread, but it doesn't touch `t->strace`. So
`/bin/strace prog` continues capturing every syscall the
new program issues. Real `strace` does the same.

When the traced thread `fork()`s, the child does *not*
inherit the ring. `thread_fork_user` sets `t->strace =
NULL` explicitly:

```c
/* Chapter 102 — tracing is NOT inherited.  POSIX strace
 * follows exec but not fork by default; we match that. */
t->strace = NULL;
```

POSIX `strace` requires `-f` to follow forks; ours doesn't
implement `-f` at all. Users who want to trace both sides
of a fork can either:

- Wrap each side in its own `strace` invocation, or
- Add `-f` later (the kernel hook is already in the right
  place; you just need to copy the ring on `fork` and then
  attach a per-trace stdout multiplexer in `/bin/strace`).

### The implicit-memset trap

`struct strace_ring` is just over 2 KiB. Initialising it
with `= {0}` makes GCC emit a call to `memset`, which
freestanding code doesn't have. (This is the same
zero-init-becomes-memset trap chapter 27 first ran into
when building printf.) We dodge it the way the user
libc's `printf` did: explicit field-by-field
initialisation in `strace_enable`.

### The /bin/strace banner-suppression dance

The very first time `/bin/strace` polls
`/proc/<child>/trace`, the child has not yet been scheduled
— it's only just been forked. The kernel renders the
literal string `(not traced)\n` (13 bytes) so that
`cat /proc/<some-pid>/trace` for any thread always returns
something readable instead of EOF.

But for `/bin/strace` running in a tight `pump_trace_once`
loop, that `(not traced)\n` would flood stderr until the
child finally calls `trace_me()`. So the tool detects a
single-read 13-byte payload exactly equal to `(not
traced)\n` and silently drops it, returning 0 (which makes
the caller's `sleep_ms(20)` kick in instead of busy-looping):

```c
static const char BANNER[] = "(not traced)\n";
long got = read(fd, buf, sizeof(buf));
if (got == BANNER_LEN
    && memcmp(buf, BANNER, BANNER_LEN) == 0) {
    while (read(fd, buf, sizeof(buf)) > 0) {}
    close(fd);
    return 0;
}
```

This is the only place either side of the kernel boundary
"knows" about the banner string. The kernel keeps emitting
it; the tool keeps swallowing it. Sometimes good layering
*is* a string match.

## Walkthrough

### kernel/core/strace.h — public API

The header declares the four kernel-internal entry points
and the on-disk shape of one entry:

```c
struct strace_entry {
    uint64_t ts_ms;       /* monotonic ms at SVC entry */
    uint64_t args[6];     /* x0..x5 from the SVC frame */
    int64_t  ret;         /* x0 after dispatch        */
    uint32_t syscall_no;  /* x8 (== num) */
    uint32_t completed;   /* 0 until dispatcher tail   */
};

struct strace_ring {
    struct strace_entry entries[STRACE_RING_CAP];
    uint32_t head;         /* next slot to write */
    uint32_t tail;         /* next slot to read  */
    uint32_t lost;
};

int  strace_enable(struct thread *t);
void strace_release(struct thread *t);
struct strace_entry *strace_enter(struct thread *t,
                                  uint32_t syscall_no,
                                  uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5);
long strace_render_and_drain(struct thread *t,
                             char *out, size_t cap);
```

`STRACE_RING_CAP` is 64 (a power of two so `head &
(CAP-1)` indexes the slot array without a divide).

### kernel/core/strace.c — formatter

The `SYSCALL_META[]` table is the only piece of strace
that needs touching whenever you add a new syscall:

```c
static const struct {
    const char *name;
    int         arity;
} SYSCALL_META[] = {
    [SYS_WRITE]  = { "write",   3 },
    [SYS_EXIT]   = { "exit",    1 },
    [SYS_GETPID] = { "getpid",  0 },
    /* ... 60+ more ... */
    [SYS_BEEP]      = { "beep",     2 },
    [SYS_TRACE_ME]  = { "trace_me", 0 },
};
```

`meta_for(no)` does a bounds check and returns the slot, or
`NULL` if the number is unknown. Unknown syscalls render as
`syscall_<n>(arg, arg, arg, arg, arg, arg) = -38` (six
args) so a forgotten table update never silently hides a
syscall — it just looks ugly until you add the row.

The hand-rolled `sf_*` formatters (`sf_putc`, `sf_puts`,
`sf_putu`, `sf_puti`, `sf_puthex`, `sf_putarg`,
`sf_put_secs_cs`) are deliberately a copy of procfs.c's
`pf_*` family rather than a shared header. The amount of
shared code (about 60 lines) wasn't worth a new header
file. If a third pseudo-fs ever wants the same primitives,
we'll factor.

### kernel/core/syscall.c — the dispatcher hook

The hook is two blocks of ~5 lines bracketing the
`switch (num)` already in `svc_dispatch`:

```c
struct strace_entry *_tr = NULL;
{
    struct thread *_tt = thread_current();
    if (_tt && _tt->strace) {
        _tr = strace_enter(_tt, (uint32_t)num,
                           a0, a1, a2, a3, a4, a5);
    }
}

long ret;
switch (num) {
    /* ... cases ... */
    case SYS_TRACE_ME:
        ret = strace_enable(thread_current()) == 0
              ? 0 : -ENOMEM_VFS;
        break;
}

frame->x[0] = (uint64_t)ret;

if (_tr) {
    _tr->ret = (int64_t)ret;
    _tr->completed = 1;
}
```

That's the entire kernel-side cost of tracing. The
existing signal-delivery tail and any other bookkeeping
stay exactly where they were; the back-fill happens after
`frame->x[0]` is written, so the trace records the value
the user-mode program will actually see in `x0`.

### kernel/core/thread.c — six init sites, two free sites

`struct thread::strace` is touched in eight places:

- Six creation paths in `thread.c`:
  `thread_create` (kernel thread), `thread_create_on`
  (kernel thread on specific CPU), the idle thread init,
  `user_thread_create`, `thread_create_shared_on`, and
  `thread_fork_user`. Each sets `t->strace = NULL`. The
  `thread_fork_user` site has the load-bearing comment
  `/* Chapter 102 — tracing is NOT inherited */` so future
  developers don't accidentally copy the parent's ring.
- Two reap paths: `thread_waitpid` (parent reaps a child
  via `wait()`) and the orphan-reaper loop in
  `thread_exit`. Each calls `strace_release(exited)` next
  to the `kfree(exited->stack_base)` it already does.

This is the same eight-edit pattern chapter 101 introduced
for `FD_PROCFS`: every new per-thread resource must be
opt-in zeroed at every creation path and freed at every
reap path. Skipping one site usually doesn't crash
immediately — it leaks memory until the thread happens to
be killed by a path that *does* free it. The rule is now
"every new field gets traced through `grep '^struct
thread {' -A100`".

The reason `strace_release` is a separate function instead
of inlining the `kfree(t->strace); t->strace = NULL;`
pair is that the reap loops in `thread_waitpid` and
`thread_exit` already touch the thread struct under
delicate locking; a function call gives us a single place
to add future bookkeeping (e.g. a debug counter) without
having to remember every reap site.

### kernel/core/thread.c — `thread_strace_render_pid`

The procfs leaf needs to find the target thread by pid
*and* hold a lock across the formatter so the thread can't
exit and be freed mid-render. `thread.c` already has
`g_all_lock` for exactly this purpose; we expose a new
helper that walks the global list and calls
`strace_render_and_drain` while holding the lock:

```c
long thread_strace_render_pid(int pid, char *out, size_t cap)
{
    long n = -1;
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    for (struct thread *t = g_all_head; t; t = t->all_next) {
        if (t->id == pid) {
            n = strace_render_and_drain(t, out, cap);
            break;
        }
    }
    spin_unlock(&g_all_lock);
    irq_restore(f);
    return n;
}
```

Holding `g_all_lock` across an 8 KiB-bounded formatter
that takes no other locks is fine. It blocks
`thread_create` and `thread_exit` for the duration, but
those are themselves fast (microseconds), and the
formatter is called only from a userspace `read()` —
never from an IRQ.

### kernel/core/procfs.c — one new leaf

Two edits to `procfs.c`:

```c
static const char *const PROCFS_PID_LEAVES[] = {
    "status",
    "cmdline",
    "trace",            /* chapter 102 */
};

/* in procfs_render(), after the cmdline branch: */
if (str_eq(leaf, "trace"))
    return thread_strace_render_pid(pid, out, cap);
```

The `PROCFS_PID_LEAVES[]` change makes
`ls /proc/<pid>` enumerate `trace` alongside `status` and
`cmdline`, so the file is discoverable through the
existing directory-listing path. The dispatch line wires
the leaf to its renderer.

### userspace/libc/syscall.h — one wrapper

```c
static inline int trace_me(void)
{
    return (int)_svc0(SYS_TRACE_ME);
}
```

That's it. Userspace gets one new function; the rest of
the syscall ABI is untouched.

### userspace/strace/strace.c — fork, attach, poll

The driver is 150 lines of straight-line C, no parser, no
allocator. The interesting parts are:

```c
int kid = fork();
if (kid == 0) {
    (void)trace_me();      /* ignore failure; exec anyway */
    execv(argv[1], &argv[1]);
    exit(127);
}
for (;;) {
    long n = pump_trace_once(kid);
    if (n < 0) break;       /* trace file gone — child reaped */
    int code = 0;
    int reaped = waitpid(kid, &code, WNOHANG);
    if (reaped == kid) {
        (void)pump_trace_once(kid);   /* final flush */
        return code;
    }
    if (n == 0) sleep_ms(20);
}
```

The `n == 0 → sleep_ms(20)` keeps us off the CPU when the
child is idle (e.g. blocked in `read`). Without it, the
parent would spin at the cost of one syscall per ring
poll, which would itself dwarf the traced program's
syscall count.

Note the explicit final `pump_trace_once(kid)` *after*
`waitpid` returns. The traced thread's ring is freed inside
`thread_waitpid` via `strace_release`, but the kernel
hasn't yet rendered the last few entries (`exit`,
typically) when we polled before the wait. Calling pump
once more lets us catch them — and then the next pump call
fails with `open(..., O_RDONLY) = -ENOENT_VFS` because the
pid is gone.

### scripts/test_strace.py — end-to-end smoke

Mirrors `test_procfs.py` shape: boot, wait for shell
prompt, run `strace /bin/echo hello`, then assert on the
captured serial output:

```python
recognised = ("write(", "open(", "execv(", "close(", "read(",
              "fork(", "exit(", "getpid(", "waitpid(", "brk(")
if not any(name in out for name in recognised):
    return fail("strace output had no recognisable syscall line", out)

if "strace: + exited with code" not in out:
    return fail("strace did not announce child exit", out)
```

We also exercise the unattached path by walking
`ls /proc` for pid leaves and reading
`/proc/<pid>/trace` until one returns `(not traced)`. That
banner is what every untraced thread serves; finding it
proves the kernel renderer's `t->strace == NULL` path
works.

## A bug we found

The first end-to-end run of `strace /bin/echo hello`
showed output like this:

```
strace /bin/echo hello
(not traced)
(not traced)
... 40 more ...
hello
21.60 execv(...) = 0
21.60 write(1, ..., 5) = 5
21.60 exit(0) = ?
strace: + exited with code 0
```

…but `strace /bin/browser --gui` didn't show the spam. Two
useful questions popped out:

1. Why does `echo` produce so many `(not traced)` lines but
   `browser` doesn't?
2. Why is the final trace correct, but framed by junk?

The answer to (1) is timing. The `/bin/strace` parent
opens `/proc/<child>/trace` immediately after `fork()`
returns, and on a 2-CPU SMP kernel the child may not have
been scheduled yet. The kernel renders the literal
`(not traced)\n` banner in that window — chapter 101 chose
that string so any `cat /proc/<pid>/trace` on an unattached
thread always returns something readable.

For `cat /proc/X/trace` from the shell, that banner is
exactly what you want. For a tracer's tight poll loop, it's
flooding the output. The `browser` case looked clean
because the child gets scheduled *much* faster relative to
the parent's first poll, so the window of `(not traced)`
spam is shorter.

The fix is the banner suppression in
`pump_trace_once` — see *The /bin/strace banner-suppression
dance* above. The kernel-side rule is unchanged
(unattached threads emit the banner), and the consumer-side
rule is "if the only content this poll is the banner, drop
it."

The bug surfaces only when the GUI test runs side-by-side
with the serial test, which is a useful pattern: if a tool
behaves differently on a fast program vs a slow one, the
difference is almost always a timing-window assumption you
didn't realise you'd made. The diagnosis script
[`scripts/_dbg_strace_echo.py`](../../../scripts/_dbg_strace_echo.py)
is preserved so you can rerun the capture if the issue ever
surfaces again.

## Lessons

**Every new per-thread resource is an eight-edit
checklist.** Six creation sites, two reap sites, plus a
fork-inherit decision. Skipping one creation site leaks
memory; skipping one reap site double-frees on the next
thread reuse; skipping the fork decision either leaks (if
you don't free) or double-frees (if you copy the pointer
naively). The cost of being explicit is small; the cost
of forgetting is hours of debugging. Chapter 101 paid this
cost for `FD_PROCFS`; chapter 102 paid it for
`struct strace_ring *`. The next per-thread thing will
pay it for whatever it is.

**Snapshot-on-open generalises.** Chapter 101 introduced
the pattern for procfs files. Chapter 102 reuses it for
the trace ring without rethinking it. The same property —
that the kernel renders into a stable buffer at a single
well-defined moment, and userspace just reads that
buffer — gives us trivial concurrency: there's no
"render in the middle of a syscall" race because the
syscall is either visible at open time or it isn't.

**Lock-free single-writer is genuinely free.** When you
have hardware that makes one writer impossible to preempt
(here: in-flight SVC) and readers that only modify a
distinct field (here: `tail`), no atomics, no fences, no
locks are required for the writer's hot path. The reader
takes a real lock (`g_all_lock`) but only because it
needs to keep the *thread struct itself* alive across the
formatter — not the ring contents.

**A tracer that needs a flag for "should I print this"
isn't a tracer.** We render every syscall the same way
every time. No filtering, no decoding, no formatting
options. If you want to grep, pipe through grep. The
kernel's job is to expose the data; the unix shell's job
is to slice it.

**The banner you ship for `cat` is the banner that floods
your tracer.** When a kernel renders something that's
useful for one tool (`cat`), check what it does to
*every* tool that touches the same file. Either the tool
or the kernel has to suppress it — and it's almost always
the tool, because the tool is the one that knows what
context it's polling in.

**When two programs disagree about output, look at how
fast they start.** A traced `echo` is done in ~30 µs; a
traced `browser --gui` opens windows, listens for events,
and survives the parent's first `pump_trace_once` poll.
The faster the program, the more `pump` calls happen
*before* the program issues its first traced syscall.
Bugs that only show up on the fast program are usually
about scheduling races between the *user* of the data and
the *producer* of the data.

## What this unlocks

- Bug reports with traces attached. "Here's the
  `strace`" is now a sentence that means something on
  this kernel.
- The "find the syscall that returns the error"
  workflow. You can finally answer "did my open fail
  before or after my read?" without instrumenting the
  binary.
- A future `-f` (follow-fork) is one ring copy in
  `thread_fork_user` away.
- A future `-p <pid>` (attach to a running thread) needs
  the consent dance described under "SYS_TRACE_ME, not
  ptrace_attach" — but the rendering and ring machinery
  is already in place.

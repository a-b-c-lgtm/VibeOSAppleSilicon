# Chapter 114e — Step 5: porting `procfs` to userspace

Chapter 114d ported `clipboardd` to userfs and showed that a
shoebox-sized userspace daemon could replace a custom IPC
protocol with "everything is a file." That was the warm-up
on purpose: the chapter-108 clipboard was 300 lines of state
that fit on a single screen.

This chapter does the same surgery on a heavier patient:
the chapter-99 `procfs` driver. Procfs is roughly 750 lines
of kernel C that walked thread tables, render-formatted
human-readable text into per-fd staging buffers, and bolted
itself into the VFS through a special-case `FD_PROCFS` kind
in `kernel/core/vfs.h`. By the end of this chapter all of
that lives in `userspace/procd/procd.c` and reaches the
kernel through three new syscalls.

The interesting bits are not the port itself (that's
mechanical once 114c has landed). The interesting bits are
the bugs that the port surfaced — one in `libfs` (a wrong
listdir type convention), one in `kernel/core/pipe.c` (a
signal-vs-data race that has been latent since chapter 78)
— and the shape of the kernel/user ABI that comes out
the other end.

## What got deleted

Three files and a chunk of cross-cutting VFS plumbing:

| Path | Size | Fate |
| --- | --- | --- |
| `kernel/core/procfs.c` | ~620 lines | **deleted** |
| `kernel/core/procfs.h` | ~112 lines | **deleted** |
| `userspace/procd/procd.c` | — | new, 580 lines |

Plus per-touch-site cleanups:

- `kernel/core/vfs.h`: `FD_PROCFS = 13` enum entry gone.
- `kernel/core/vfs.c`: the `if (kind == FD_PROCFS) …` arms
  in `vfs_read` and `vfs_close` are gone. The mount table
  dispatch already handles `/proc` once `/bin/procd` calls
  `sys_mount`.
- `kernel/core/syscall.h`: `FD_PROCFS` references removed.
- `kernel/core/osfs.c`, `osfs2.c`, `tmpfs.c`, `userfs.c`,
  `thread.c`: every `case FD_PROCFS:` arm in close-on-exec
  / dup loops is gone.
- `kernel/core/main.c`: the `procfs_register_mount()` call
  in `vfs_init` is gone (procd's `sys_mount` does it now).
- `Makefile`: `KERNEL_OBJS` no longer lists `procfs.o`. A
  new `PROCD_OBJS` / `PROCD_ELF` / `PROCD_STRIPPED` block
  builds `/bin/procd` and the `mkosfs` step adds it to the
  disk image.

Net: ~750 lines of kernel C swap for ~580 lines of
userspace C. The kernel shrinks by more than the userspace
grows because the kernel-side staging-buffer scaffolding
(one per `fd_entry`, with manual carry-over for partial
reads) collapses into a single `malloc` in the daemon's
`on_open`.

## The three new syscalls

The kernel still has to publish the raw data — pmem
counters, runqueue lengths, thread snapshots, the
chapter-100 strace ring. Procd is in userspace; it can't
walk `g_threads[]` directly. So 114e adds three syscalls
that hand it the data in a stable ABI:

| ID | Name | Returns |
| --- | --- | --- |
| 98 | `SYS_KSTAT` | `struct kstat_pub` — uptime, mem, CPU count, runqueue lengths |
| 99 | `SYS_THREAD_SNAPSHOT` | array of `struct thread_snap_pub` — `pid=-1` returns all live; `pid>=0` returns one |
| 100 | `SYS_STRACE_RENDER` | up to 16 KiB of rendered strace text for one pid |

The ABI structs live in
[`userspace/libc/proc_stat.h`](../../../userspace/libc/proc_stat.h);
the kernel mirrors them as `*_wire` structs in
[`kernel/core/syscall.c`](../../../kernel/core/syscall.c)
so that touching the kernel snapshot type doesn't require
recompiling userspace.

`SYS_THREAD_SNAPSHOT` is the only one that takes an
argument. `pid=-1` is the bulk-listing path procd uses for
`/proc/` directory enumeration and `/proc/sched`;
`pid>=N` is the single-thread path procd uses every time
it opens `/proc/<pid>/status`. We deliberately did NOT
merge these into two calls — one syscall with a single
shape is easier to keep ABI-stable than two.

`SYS_STRACE_RENDER` is special because the strace ring
buffer lives in the kernel and is too large to copy out
piecewise on every read. The kernel renders the whole
thing into a kernel buffer (capped at 16 KiB), then
`copy_to_user`s it once. Procd caches that buffer in its
per-handle slot until close; subsequent reads serve from
the cache without re-entering the kernel.

The new IDs landed at the next-free slots in **both**
syscall enums — `kernel/core/syscall.h` and
`userspace/libc/syscall.h` (the project has carried two
parallel enums since chapter 14 because freestanding C
sees no shared header).

## `procd.c`'s shape

Five callbacks plus a renderer per file. The renderers are
direct copies of the chapter-99 `pf_*` helpers
(`pf_putu`, `pf_puts`, `pf_putc`, `pf_put_secs_cs`) —
hand-formatted because each one writes into a single
growing buffer and `printf` would force a snprintf-then-
concat dance for every line.

The state is one fixed-size handle table:

```c
#define PROCD_MAX_HANDLES 16
#define PROCD_FILE_CAP    8192u    /* same as kernel PROCFS_MAX_FILE */
#define PROCD_TRACE_CAP   16384u

struct slot {
    int       used;
    char     *buf;
    uint32_t  len;
};
static struct slot g_slots[PROCD_MAX_HANDLES];
```

`on_open` picks the buffer size (trace files get the larger
one), `malloc`s, rendres, and reserves a slot:

```c
static int on_open(void *ud, const char *path, int flags,
                   uint32_t *handle_out)
{
    int rw = flags & 3;
    if (rw == 1 || rw == 2) return -13;   /* -EACCES */
    if (flags & 0100)        return -13;  /* O_CREAT */
    if (flags & 01000)       return -13;  /* O_TRUNC */

    char *buf = NULL;
    uint32_t len = 0;
    long n = render_path(path, &buf, &len);
    if (n < 0) return -2;                 /* -ENOENT */

    uint32_t h = slot_alloc(buf, len);
    if (h == 0) { free(buf); return -24; } /* -EMFILE */
    *handle_out = h;
    return 0;
}
```

`on_read` is a straight memcpy out of the cached buffer at
`off`; `on_close` frees the buffer and clears the slot. The
"re-render on open, cache till close" discipline matches
what the chapter-99 kernel driver did — a `cat
/proc/uptime` reads a single snapshot, not a live stream.

## Two bugs the port shook loose

### libfs `on_listdir` type convention

The chapter-114c writeup left the `on_listdir` type
argument under-specified: the libfs comment claimed
`0 = file / 1 = directory`, but every existing consumer
(`ps`, `top`, `ls`) reads from the canonical
`fs_ops::listdir` encoding `1 = file / 2 = directory`
defined in `kernel/core/vfs.h::LISTDIR_TYPE_FILE` and
`OSFS2_TYPE_FILE` / `OSFS2_TYPE_DIR`.

`echofs` and `clipboardd` both flat-out work despite the
mismatch because they only list files and `ps`/`top` never
walk through them. Procd's `on_listdir` lists *both* files
(`uptime`, `meminfo`, `cpuinfo`, `sched`) AND directories
(`/proc/<pid>/`). The bug surfaced as `test_procfs.py`
failing with "ps did not list sh": the pid directory
entries had `type=1`, the kernel-side `userfs_op_listdir`
forwarded that to `ps`, and `ps` (which renders
directories specially) skipped them all.

Fix: harmonize all four sites — `libfs` docstring,
`echofs`, `clipboardd`, and procd — on `1 = file /
2 = directory`. Recorded as the canonical convention in
[`/memories/repo/chapter-114e-procd-port.md`](#) so the
next userfs daemon doesn't get this wrong again.

### `pipe_read` returns `-EINTR` even when data arrived

This one is more subtle and was latent in the kernel since
chapter 78 added SIGCHLD. The chapter-99 procfs never
triggered it; the chapter-114e port surfaced it
immediately, because `/proc/<pid>/trace` is the file
`strace` reads when the traced child has just exited.

`pipe_read` (kernel/core/pipe.c) used to be:

```c
while (p->count == 0) {
    if (p->w_refs == 0) return 0;
    thread_block_on(p);
    struct thread *me = thread_current();
    if (me && me->sig_pending) return -EINTR_PIPE;
}
```

The race window:

1. Strace calls `open("/proc/24/trace")`.
2. The kernel-side `userfs_op_open` sends an OPEN request
   into procd's `req_pipe` and blocks in
   `pipe_read(rsp_pipe)` waiting for the 32-byte reply.
3. Procd renders the trace, writes the reply — those 32
   bytes land in `rsp_pipe`, `thread_wake_blocked` fires,
   strace's blocked thread becomes `READY`.
4. **Before strace is scheduled**, the traced child
   (`/bin/echo`) exits. The exit path delivers `SIGCHLD`
   to strace. `thread_signal_pid` sets `sig_pending` and
   *also* calls `thread_wake_blocked` (so blocked-in-wait
   parents notice their children).
5. Strace resumes from `thread_block_on`. The `count > 0`
   condition would be true *and* `sig_pending` is set.
   The old code checked `sig_pending` first and returned
   `-EINTR_PIPE`, leaving the 32-byte reply unread in the
   pipe. `userfs_call` saw `r <= 0`, marked the channel
   dead, returned `-EIO`. `open()` failed with `-5` even
   though procd's response was sitting right there.

The fix is to prefer data over signal on wake. Re-check
`count` first; only return `-EINTR_PIPE` if we still have
nothing to give the caller:

```c
while (p->count == 0) {
    if (p->w_refs == 0) return 0;
    thread_block_on(p);
    if (p->count > 0) continue;       /* prefer data */
    struct thread *me = thread_current();
    if (me && me->sig_pending) return -EINTR_PIPE;
}
```

The signal stays pending. It will fire at the next syscall
return — `svc_dispatch` re-checks `sig_pending` before the
`eret`, so the handler runs on the very next user-mode
entry. Nothing is lost; only the ordering changes.

The lesson is wide enough to be worth a permanent note:
**any blocking primitive that wakes on both a wait
condition and a signal must check the wait condition
first**. Future condvars, semaphores, and select-style
multiplexers all face the same trap. Recorded as
[`/memories/repo/pipe-read-prefer-data-over-signal.md`](#).

The lesson is also why future *daemons* benefit from this
fix even though they don't experience it directly: any RPC
shape that puts a request-receiving process on the other
end of a pipe will eventually have the daemon's caller
receive a signal mid-protocol. Now the kernel guarantees
the data lands before the signal does.

## Why a fresh syscall instead of more userfs files

A reasonable design would have been to put the kernel
state behind another userfs mount: `/sys/kstat`, `/sys/
threads`, `/sys/strace/<pid>` — so the kernel exposes raw
data through the same file-shaped interface procd uses
upward. We chose the syscall route for three reasons:

1. **Procd already runs as one of two callers** (the other
   being a future `top` written in userspace). A whole new
   userspace daemon to serve `/sys` would add a third
   process on the boot path before anyone benefits.
2. **The data shape is a struct, not a stream.** Forcing
   `/sys/kstat` to be a parsed text file would make procd
   the parser, not the renderer — exactly the inversion of
   responsibility this chapter is trying to avoid.
3. **Three syscalls cost one enum entry each.** Today
   that's 98, 99, 100 out of an enum that runs to ~110.
   We have budget.

When the OS grows a second consumer of `SYS_KSTAT` (likely
`top` in chapter 116), we'll reassess: if the struct
threatens to grow per-consumer fields, we'll split it. For
now, the dual ABI struct in
[`userspace/libc/proc_stat.h`](../../../userspace/libc/proc_stat.h)
+ kernel `*_wire` mirror keeps the contract tight.

## What this unlocks

Per the apps-must-use-features discipline:

- **Existing app(s) modified to use the feature**: none.
  `ps`, `top`, `strace`, and `cat /proc/...` are all
  unchanged — they were always file-shaped consumers, and
  the file-shaped interface (mount table, `fs_ops`) didn't
  change. The kernel just isn't the one serving anymore.
- **New app(s) added**: `userspace/procd/procd.c` —
  supervised by init the same way `clipboardd` is.
- **Existing test scripts upgraded**: none — the chapter-99
  regressions (`test_procfs.py`, `test_strace.py`) pass
  unmodified against the new daemon.
- **New test scripts added**: `scripts/_dbg_strace_raw.py`
  — boots the kernel, runs `strace /bin/echo hello`, and
  dumps raw serial. Kept under the `_dbg_` prefix per the
  debug-scripts-policy because it's a one-shot capture
  tool, not a regression test, but invaluable for the
  next time a userfs daemon's reply gets lost in transit.

## Side-effects for the next chapter

Two:

1. **Userfs is now load-bearing on the boot path.** If
   procd fails to come up, every `ps`, `top`, and `cat
   /proc/...` returns `-ENOENT`. Today the supervise()
   loop respawns it on crash, but a daemon that hangs
   (e.g. waiting on a permanently-empty pipe) would freeze
   every `/proc` reader behind it.

2. **`userfs_call` is now exercised under real load.**
   The per-call lock holds for the entire request/reply
   round-trip, including procd's malloc and renderer. A
   slow renderer (`/proc/<pid>/trace` with 16 KiB of ring
   contents) means every other `/proc` reader stalls. The
   serialization is correct but the latency floor is set
   by the worst-case renderer.

Chapter 114f closes the loop on both: it adds per-request
timeouts to `userfs_call` so a wedged daemon can't hold the
rest of the system hostage, and documents the existing
`owner_pid`-based deadlock detector alongside the new
timeout work so "step 6" is one place in the book. Slot
pipelining — letting concurrent callers overlap requests
when the daemon's callbacks are re-entrant — is deferred to
a future chapter; the timeout's 5 s upper bound on
worst-case latency makes it a polish item rather than a
correctness fix.

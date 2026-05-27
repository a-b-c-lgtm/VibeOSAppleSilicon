# Chapter 101 — A /proc-shaped filesystem, ps, and top

The kernel knows a great deal. It knows how many threads are
alive, what each one is doing, which CPU it last ran on, how
many pages of physical memory are free, how deep the heap goes,
how many ticks have elapsed since boot. None of that is
observable from userspace. If you want to debug a runaway
process, watch memory pressure, or just confirm that `init`
really did adopt a particular orphan, you have nothing to look
at — every interesting fact is locked behind a kernel-only
global.

This chapter borrows Linux's solution: a read-only synthetic
filesystem mounted at `/proc/`, where each "file" renders its
content fresh on every open. There is no on-disk backing and
no cache; the bytes you read are the kernel's view of the world
at the moment you ran `cat`. On top of that we ship two new
binaries — `/bin/ps` and `/bin/top` — that walk `/proc/` and
present per-process tables.

After this chapter the regression sweep is **55 / 55 PASS**.

## What this chapter adds

- **A thread-snapshot API** in [kernel/core/thread.c](../../../kernel/core/thread.c) /
  [kernel/core/thread.h](../../../kernel/core/thread.h):
  `thread_snapshot()`, `thread_snapshot_pid()`,
  `thread_runqueue_len()`, and `thread_live_count()`. They walk
  the live thread list under `g_all_lock` and copy each thread
  into a self-contained `struct thread_snap` that callers can
  format without holding any locks.
- **A new pseudo-FS module** [kernel/core/procfs.c](../../../kernel/core/procfs.c):
  hand-rolled formatters (the kernel has no `printf`) that turn
  the snapshot structs into the textual files Linux's procfs
  has trained every Unix user to expect — `uptime`, `meminfo`,
  `cpuinfo`, `sched`, `<pid>/status`, `<pid>/cmdline`.
- **A fifth VFS prefix** in [kernel/core/vfs.c](../../../kernel/core/vfs.c)
  and a new `FD_PROCFS` fd kind in [kernel/core/vfs.h](../../../kernel/core/vfs.h):
  `vfs_open("/proc/...")` calls `procfs_render` into a fresh
  kheap buffer and stashes it on the fd; `vfs_read` slices that
  buffer; `vfs_close` `kfree`s it.
- **`sys_listdir_at` dispatch** in [kernel/core/syscall.c](../../../kernel/core/syscall.c):
  paths under `/proc/` and `/proc` route to `procfs_listdir`,
  which enumerates the static top-level files followed by one
  entry per live pid.
- **`/bin/ps`** [userspace/ps/ps.c](../../../userspace/ps/ps.c) —
  enumerates `/proc/<pid>/` directories, parses each `status`
  file, and prints a `ps`-style column.
- **`/bin/top`** [userspace/top/top.c](../../../userspace/top/top.c) —
  same parser plus an `\x1b[2J\x1b[H` redraw every second and
  an optional frame-count argument so test scripts don't hang.
- **A regression test** [scripts/test_procfs.py](../../../scripts/test_procfs.py):
  drives the kernel shell over the serial socket and exercises
  every file plus `ps`, end-to-end through the syscall layer.

## Prerequisites

- [Chapter 15 — Files, VFS, and a tiny ramfs](../04-userspace/015-files-and-vfs.md)
  introduced the fd table and the `vfs_open` / `vfs_read` /
  `vfs_close` shape that procfs hooks into. Every milestone
  since has added a new branch to those three functions:
  pipes (chapter 29), tmpfs (chapter 31), tcp sockets
  (chapter 54), ptys (chapter 79), the writable OSFS-2 mount
  (chapter 85), and now `/proc/`.
- The `SYS_LISTDIR_AT` syscall, introduced alongside the
  `/data` mount, is what `ls /proc` and `/bin/ps` use to
  enumerate the pseudo-FS.
- [Chapter 90 — SMP runqueue](../11-smp-and-memory/090-smp-runqueue.md)
  established the per-CPU runqueues that `/proc/sched` now
  reports the depth of.
- [Chapter 94 — refcounted fd_table](../11-smp-and-memory/094-clone-files.md)
  matters because procfs allocates a kheap buffer on `open`
  and frees it on `close`; the inherit-on-fork path needs to
  know not to share that pointer (more on this below).

## Design decisions

The "what should /proc/<x> look like" choices were easy —
Linux has one obvious answer for each of `uptime`, `meminfo`,
`cpuinfo`, `<pid>/status`. The interesting decisions were all
about *when* and *where* to do the work.

### Snapshot at open, slice on read

Procfs files are dynamic by definition. `cat /proc/uptime`
twice and you'll see two different numbers. The naïve
implementation re-renders on every `read()`: each call into
`vfs_read` for an FD_PROCFS fd runs the formatter again,
starting at byte zero, and slices out the requested range.

We don't do that. Instead, `vfs_open` calls `procfs_render`
once into an 8 KiB kheap buffer, stashes the pointer on the
fd, and `vfs_read` becomes a straight `memcpy` from
`buf + offset`:

```c
if (path_starts_with(name, "/proc/")) {
    const char *rel = name + 6;
    char *buf = (char *)kmalloc(PROCFS_MAX_FILE);
    if (!buf) return -ENOMEM_VFS;
    long n = procfs_render(rel, buf, PROCFS_MAX_FILE);
    if (n < 0) { kfree(buf); return -ENOENT_VFS; }
    /* allocate fd slot, set kind=FD_PROCFS, store buf + len */
}
```

Why? Two reasons, both about correctness rather than
performance.

1. **Atomicity.** A reader that takes ten `read(fd, ..., 256)`
   calls to consume `/proc/sched` would otherwise see a
   different runqueue state in every slice. With snapshot-at-
   open, the bytes that come back across all those calls
   describe a single coherent moment.
2. **Lock scope.** Render-on-open lets us take `g_all_lock`
   once per open, snapshot the world into a thread_snap array,
   and release the lock *before* the formatter runs. The
   formatter does no locking and no kernel work other than
   string assembly — exactly what you want for code that runs
   in process context. Render-on-read would force every read
   to re-acquire the lock, which scales badly when ten threads
   are all `cat`-ing different `/proc/<pid>` paths.

The cost is one 8 KiB allocation per open. Procfs files top
out at a few hundred bytes — `meminfo` is five lines, `sched`
is one row per CPU — so the buffer is hugely over-sized for
the data. It's sized to be generous: as we add more files
(per-thread CPU time, per-fd table dumps, IRQ counts), the
constant stays the same.

### Locked walk, then format

`thread_snapshot()` walks the entire thread list under
`g_all_lock`:

```c
int thread_snapshot(struct thread_snap *out, int max)
{
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    int n = 0;
    for (struct thread *t = g_all_head; t && n < max; t = t->all_next)
        thread_snap_fill(&out[n++], t);
    spin_unlock(&g_all_lock);
    irq_restore(f);
    return n;
}
```

The lock is held for exactly long enough to memcpy each thread
into a snapshot struct. No `pf_putu`, no `pf_puts`, no
filesystem calls happen with the lock held. Once we return,
the snapshot array is a pure value — the original threads can
exit, be reaped, or have their state changed and we still see
a coherent picture.

`struct thread_snap` carries every field the renderers ever
need: `name[32]`, `args[128]`, `cwd[96]`, plus the small
scalars. Sized at roughly 256 bytes; a 32-entry snapshot
array is 8 KiB on the caller's stack. Bumping the cap is a
one-line change — the trade-off is stack pressure on the
caller.

### Hand-rolled formatters

The kernel has no `printf`. We've gotten away with this for
84 milestones because everything previously kernel-printed
was either fixed strings (`serial_puts`) or hex dumps
(`serial_puthex`). Now we need decimal numbers with padding.

`kernel/core/procfs.c` ships a tiny set of `pf_*` helpers:

```c
static void pf_putc(char *buf, size_t cap, size_t *pos, char c);
static void pf_puts(char *buf, size_t cap, size_t *pos, const char *s);
static void pf_putu(char *buf, size_t cap, size_t *pos, uint64_t v);
static void pf_puti(char *buf, size_t cap, size_t *pos, int v);
static void pf_putu_w(char *buf, size_t cap, size_t *pos,
                       uint64_t v, int width);
static void pf_put_secs_cs(char *buf, size_t cap, size_t *pos,
                           uint64_t ms);
```

All five share the same "buffer + cap + cursor" contract.
Overflow is silent — once `*pos` hits `cap - 1` they stop
writing — because procfs renders always fit in `PROCFS_MAX_FILE`
and we'd rather truncate than panic if some pathological
combination of long names somehow exceeds it.

Two things to note about the helpers:

- `pf_putu` is purely iterative — no recursion, no
  `va_list` — so it's safe to call from any context. The
  digit reversal happens in a 20-byte stack buffer (max
  digits in `uint64_t` is 20).
- `pf_put_secs_cs` exists to render Linux-format uptimes:
  `<sec>.<centiseconds>`. The kernel's `timer_ticks() *
  TICK_INTERVAL_MS` is in milliseconds, so the conversion to
  centiseconds is `(ms % 1000) / 10` with a leading zero
  when needed.

The rule going forward: if we add a sixth or seventh
procfs file and find we need a new format primitive (octal?
hex with width?), it goes in this header-private set of
`pf_*` helpers, not a kernel-wide `ksprintf`. The kernel as a
whole is better off without `printf`.

### `/proc/<pid>/cmdline` for kernel threads

When this chapter first booted, `cat /proc/8/cmdline` printed
nothing at all — the `osfs2-flush` thread has no args
(it's a kernel thread, never spawned through ELF), so
`s.args[0] == '\0'` and we emitted just a bare newline.

The user couldn't tell whether the file was empty, the open
had failed, or `cat` was broken.

Linux's procfs has a long-standing convention for this:
kernel threads render as `[name]` in cmdline. We follow:

```c
if (s.args[0] == '\0') {
    pf_putc(out, cap, &pos, '[');
    pf_puts(out, cap, &pos, s.name);
    pf_putc(out, cap, &pos, ']');
} else {
    pf_puts(out, cap, &pos, s.args);
}
pf_putc(out, cap, &pos, '\n');
```

Now `cat /proc/8/cmdline` prints `[osfs2-flush]`, and `ps`
shows recognisable names for every kernel thread.

### `cat /proc` and `cat /proc/<pid>` show the directory

Linux's procfs returns `EISDIR` for `cat /proc`. Our shell has
no special handling for `EISDIR` — it just reports the errno —
and Unix users (the only users we'll ever have) really do
expect "cat a directory" to do *something* informative.

So `procfs_render("")` and `procfs_render("<pid>")` produce a
textual listing using the same backing data the
`procfs_listdir` iterator uses:

```
f  /proc/uptime
f  /proc/meminfo
f  /proc/cpuinfo
f  /proc/sched
d  /proc/19/
d  /proc/15/
d  /proc/0/
```

The `f`/`d` prefix mirrors `ls -l`'s file-type column. The
choice of "text directory" over "errno" is local to procfs —
the rest of the VFS continues to reject `read()` on a
directory fd.

This decision had a small implementation cost: `vfs_open`
must accept both `/proc/` and bare `/proc` and route the
latter as an empty relative path. The `procfs_render`
dispatcher must dispatch on `*path == '\0'` before its
"<pid>/<leaf>" parsing.

### Don't inherit FD_PROCFS across fork

Procfs fds carry a `kmalloc`'d snapshot buffer that
`vfs_close` will free. If a parent has `/proc/uptime` open at
fd 3 and forks, the naïve copy in `thread_inherit_fds`
duplicates the pointer — and both `vfs_close` paths will then
call `kfree` on the same address. Classic double-free.

We skip FD_PROCFS in the inherit loop:

```c
if (src->kind == FD_PROCFS) continue;
```

Children that want `/proc/uptime` can re-open it. The cost
of an open is one `kmalloc(8 KiB)` and one formatter run —
cheap enough to not need fancy reference counting just to
support inheriting these fds. The pattern matches what we
already do for FD_SOCKET, which is similarly child-unsafe to
share.

This is the kind of decision that's easy to skip until the
day a forking script does `exec sh -c "cat /proc/uptime"` and
the kernel double-frees on its next `close`. Marking it
explicit and *immediately* lets us not have to remember.

## Walkthrough

### kernel/core/procfs.c — render dispatcher

The top of the file declares the two static name tables
(`PROCFS_ROOT_FILES`, `PROCFS_PID_LEAVES`) once, so the
textual directory renderers and the `procfs_listdir`
enumerator share one source of truth. Adding a new top-level
file is now a one-line change.

The render dispatcher splits by path shape:

```c
long procfs_render(const char *path, char *out, size_t cap)
{
    if (*path == '\0') return render_proc_root_dir(out, cap);

    if (str_eq(path, "uptime"))   return render_uptime(out, cap);
    if (str_eq(path, "meminfo"))  return render_meminfo(out, cap);
    if (str_eq(path, "cpuinfo"))  return render_cpuinfo(out, cap);
    if (str_eq(path, "sched"))    return render_sched(out, cap);

    const char *after_pid;
    int pid = parse_pid(path, &after_pid);
    if (pid < 0) return -1;
    if (*after_pid == '\0')        return render_pid_dir(pid, out, cap);
    if (*after_pid != '/')         return -1;
    const char *leaf = after_pid + 1;
    if (*leaf == '\0')             return render_pid_dir(pid, out, cap);
    if (str_eq(leaf, "status"))    return render_pid_status(pid, out, cap);
    if (str_eq(leaf, "cmdline"))   return render_pid_cmdline(pid, out, cap);
    return -1;
}
```

Each `render_*` writes a fully-formed file body into `out`
and returns the byte length. -1 means "not a recognised
path", which `vfs_open` translates into `-ENOENT`.

### kernel/core/vfs.c — `/proc/` dispatch

The new prefix branch sits between `/bin/` (OSFS-1) and
`/mnt/` (also OSFS-1). The structure mirrors every other
branch in the function: detect the prefix, do the
filesystem-specific work, then allocate an fd slot:

```c
const char *rel = NULL;
if (path_starts_with(name, "/proc/"))
    rel = name + 6;
else if (name[0] == '/' && name[1] == 'p' && /* ... */
         name[5] == '\0')
    rel = "";
if (rel) {
    char *buf = (char *)kmalloc(PROCFS_MAX_FILE);
    if (!buf) return -ENOMEM_VFS;
    long n = procfs_render(rel, buf, PROCFS_MAX_FILE);
    if (n < 0) { kfree(buf); return -ENOENT_VFS; }
    /* find a free fd, set kind=FD_PROCFS, store buf */
}
```

`vfs_read`'s FD_PROCFS branch is the simplest in the file:

```c
if (e->kind == FD_PROCFS) {
    if (!e->procfs_buf) return 0;
    if (e->offset >= e->procfs_len) return 0;
    size_t remaining = (size_t)(e->procfs_len - e->offset);
    size_t to_copy   = len < remaining ? len : remaining;
    /* memcpy + advance offset */
    return (long)to_copy;
}
```

`vfs_close`'s FD_PROCFS branch is even simpler — one
`kfree` — but it's the only thing standing between us and a
memory leak that would compound over the lifetime of the
system.

### kernel/core/syscall.c — `sys_listdir_at` dispatch

The procfs listdir branch sits *before* the existing `/data`
branch, because it has a slightly different shape (no inode
table to walk, no `osfs2_present()` check):

```c
static const char proc_prefix[] = "/proc";
int j;
for (j = 0; j < (int)sizeof(proc_prefix) - 1; j++)
    if (path[j] != proc_prefix[j]) goto not_proc;
if (path[j] && path[j] != '/') goto not_proc;
const char *sub = path + j;
while (*sub == '/') sub++;
int got = procfs_listdir(*sub ? sub : NULL, (int)idx,
                          name, sizeof(name), &type);
```

The `goto not_proc` label is the chapter's only `goto`. The
alternative was a deeply-nested if/else; the goto is
shorter and the control flow is obvious once you see the
label.

### userspace/ls/ls.c — recognise the new prefix

`ls` already routed `/data/...` through `listdir_at` instead
of the flat `listdir`. We add a sibling check for `/proc`:

```c
else if (p[0] == '/' && p[1] == 'p' && p[2] == 'r' &&
         p[3] == 'o' && p[4] == 'c' &&
         (p[5] == '/' || p[5] == '\0'))
    use_at = 1;
```

Without this, `ls /proc` would fall through to the flat
`SYS_LISTDIR` which only knows about the embedded ramfs and
the OSFS mounts — and the user would see the unhelpful
"no files in /proc/" message.

### userspace/ps/ps.c — column output

`ps` walks `/proc` with `listdir_at`, filters for directory
entries (the per-pid subdirs), and for each one opens
`/proc/<pid>/status`, parses the textual key:value lines,
and prints a row:

```
  PID  PPID CPU S CMD
   19    18   0 S browser
   15     1   0 W gui_term
    8     0   0 S osfs2-flush
    0    -1   0 W boot
```

The parser is deliberately tolerant: it walks line-by-line,
looks for the four keys it cares about (`Name:`, `Pid:`,
`PPid:`, `Cpu:`, `State:`), and ignores anything else. If
procfs starts emitting new keys (RSS, ticks, signal mask),
old `ps` binaries keep working.

### userspace/top/top.c — same parser, refresh loop

`top` shares the parser by copy-paste — same struct, same
key matching. The only real differences are:

- The header prints `/proc/uptime` and three lines of
  `/proc/meminfo` before the column table.
- The output starts with `\x1b[2J\x1b[H` (ANSI clear-screen
  + home cursor). gui_term decodes this and produces a clean
  redraw. The kernel console ignores CSI, which means the
  output stacks downward; that's fine because the most
  recent block at the bottom of the scroll is still
  meaningful.
- An optional first arg sets the iteration count, defaulting
  to 3. `top -1` runs until killed.

The copy-paste was deliberate. A shared `procparse.h` in
`userspace/libc/` would be one more thing to keep in sync
between two binaries that already each fit in a hundred
lines of self-contained code. Both ps and top are stable
enough that the duplication isn't an active source of bugs.

## A bug we found, twice

Two real bugs surfaced during interactive testing — both
worth recording because they illustrate the same theme:
"silently failing" by emitting empty output.

### Bug 1: `cat /proc` returned ENOENT

`path_starts_with(name, "/proc/")` requires a trailing
slash. The bare path `/proc` fell through every branch in
`vfs_open` and ended up in the OSFS-1 lookup, which
correctly reported ENOENT. The user saw:

```
/$ cat /proc
cat: cannot open /proc: errno=2
```

Fix: accept both `/proc/` and `/proc` in `vfs_open`, route
the latter to `procfs_render("")`, which now produces a
textual directory listing.

### Bug 2: `cat /proc/<kernel-thread>/cmdline` printed nothing

The renderer printed `s.args` verbatim, which is the empty
string for kernel threads. Output: a bare newline. The user
couldn't distinguish "this thread genuinely has no args"
from "the file failed to open" from "cat is broken".

Fix: when args is empty, print `[<name>]` instead. This is
Linux's convention and makes every cmdline file informative.

### The pattern

In both cases the kernel was *correct* — `/proc` really
wasn't a valid open path, and a kernel thread really has no
argv. But correctness without observability is a usability
failure. The procfs hierarchy is meant to be *browsable*:
users `cat` paths to learn what's there, and silent-empty
or generic-ENOENT responses train them to stop browsing.

The takeaway for future pseudo-FS files: every path that
parses syntactically should produce output. If a thread's
field is empty, render `<empty>` or `[name]`. If a directory
is being read, render its contents. ENOENT is for paths
that don't refer to anything; empty output is for things
that exist but are uninteresting.

## Lessons

- **Snapshot-at-open is the right shape for any synthetic
  FS.** It bounds the lock scope, gives readers a coherent
  view, and the cost (one kmalloc per open) is trivial.
- **Hand-rolled formatters > a kernel printf.** Procfs only
  needs six primitives; each is shorter than the equivalent
  format-string parser. The kernel as a whole is better off
  not having a printf — every fixed-string `serial_puts`
  call is a place we can't accidentally format-inject from
  attacker input.
- **fork-inherit needs to know about every new fd kind.**
  FD_PROCFS, FD_SOCKET, and any future fd kind that owns
  kheap memory has to be skipped from `thread_inherit_fds`.
  The list isn't long, but it's grown with every milestone
  that added a new fd kind, and we should treat "what does
  fork do with this?" as a required consideration when
  adding any new `enum fd_kind` value.
- **`cat` should be informative.** Returning ENOENT for a
  recognised directory, or empty output for a recognised
  file, breaks the discoverability that makes procfs useful
  in the first place. Both bugs above were "correct but
  user-hostile" — easy to write, hard to spot until someone
  tries to browse.

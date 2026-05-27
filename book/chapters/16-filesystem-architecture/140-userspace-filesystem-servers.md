# Chapter 140 — User-space filesystem servers (9P-shaped)

> **Milestone in this chapter:** 114 — design contract for the
> userspace-filesystem refactor. The implementation lands across
> the six follow-up chapters 141–146.
> **Code referenced (where the contract lands):**
> - [kernel/core/userfs.c](../../../kernel/core/userfs.c)
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_MOUNT` / `SYS_UMOUNT`)
> - [userspace/libfs/](../../../userspace/libfs/) (the helper
>   library every userspace FS server links against)
>
> **At the end of this chapter** you will have the dispatch shape
> that lets a `/bin/` binary register itself as the backing for a
> mountpoint, the wire format for the kernel-to-server RPC, and the
> error-and-timeout policy. **No code lands in this chapter** —
> chapter 141 is the first implementation step, and requires the
> [chapter 132](132-mount-table-and-vtable.md) mount table to be in
> place.

## What this chapter proposes

A way to mount a userspace program as a filesystem.

Today every filesystem is in-kernel: ramfs, OSFS-1, OSFS-2, tmpfs,
procfs. Each one is a few hundred lines of C that runs at EL1 with
full kernel privileges. That is fine for filesystems that need
block-device access (OSFS-1, OSFS-2) or kernel-internal state
(procfs walks the thread table), but it is wrong for everything
else.

The system clipboard already ships
([Chapter 113](../14-userspace-services/113-clipboard.md), riding
on the named-IPC bus from
[Chapter 112](../14-userspace-services/112-ipc.md)) — but it lives
outside the filesystem namespace, with its own one-off API. Step
[144](144-clipboardd-port.md) re-fronts it as `/clipboard/text`.
Audio probably wants `/dev/audio` or `/dev/snd/` shaped paths. A
future browser may expose its history as `/sys/browser/history/`.
None of these need to live in the kernel — they need *interactivity*
with running userspace processes, which the kernel is bad at.

The Unix-y answer is "a userspace daemon." The Plan-9 / 9P answer
is "a userspace daemon that *is* a filesystem" — so applications
interact with it through the same `open`/`read`/`write`/`close`
they use for every other file, and the daemon's namespace is
browsable with `ls`.

This chapter adopts that second approach.

## The shape: 9P, simplified

The protocol shape is borrowed from
[Plan 9's 9P](https://9p.io/sys/man/5/INDEX.html), reduced to the
operations `struct fs_ops` already names:

```c
enum p9_op {
    P9_OPEN    = 1,
    P9_READ    = 2,
    P9_WRITE   = 3,
    P9_CLOSE   = 4,
    P9_LISTDIR = 5,
    P9_UNLINK  = 6,
    P9_MKDIR   = 7,
    P9_IS_DIR  = 8,
    P9_LOAD    = 9,
};

struct p9_msg {
    uint32_t op;       /* p9_op */
    uint32_t tag;      /* request id, echoed in reply */
    uint32_t handle;   /* per-mount fid: 0 for open, else fd-on-server-side */
    uint32_t flags;    /* op-specific */
    uint64_t offset;   /* for read/write */
    uint32_t length;   /* of the payload that follows */
    /* payload bytes: path (for open/listdir/unlink/mkdir/is_dir/load),
     * data         (for read/write replies) */
};
```

Variable-length payloads (paths, read buffers) follow the
header. Maximum message size capped at 64 KiB to keep the
RPC bounded and predictable.

A request goes kernel → userspace; a reply comes back. The
reply uses the same struct: `op | P9_REPLY` (high bit set)
in the op field, `tag` matches the request, `length` is
either bytes-of-payload-returned or `(uint32_t)errno`
under the right `flags` bit.

We **do not** implement 9P proper. The real protocol has
auth, walks, clones (Tattach/Twalk/Tclunk), and a separate
fid model. We don't need those for a private kernel↔
single-daemon channel inside one machine. What we keep is
the *shape*: small message header, fixed op set, variable
payload, tags for in-flight pipelining.

## The kernel side: one new fs_ops

The mount-table work in Chapter 132 makes this trivial.
We add one more driver: `userfs_ops`. Its `cookie` is a
`struct userfs_channel`:

```c
struct userfs_channel {
    int       owner_pid;     /* the process serving this mount */
    int       req_fd;        /* kernel writes requests here */
    int       rsp_fd;        /* kernel reads replies here */
    spinlock_t lock;         /* one outstanding request at a time, v1 */
    uint32_t  next_tag;
    /* later: a wait queue keyed by tag for full pipelining */
};
```

Every `userfs_op_*` function builds a `p9_msg`, writes it
to `req_fd`, blocks the calling thread on `rsp_fd`, parses
the reply, returns the result. The mechanism is exactly
the same as `pipe_write`/`pipe_read` — we reuse the
existing pipe machinery rather than inventing new IPC.

```c
static long userfs_op_read(void *cookie, struct fd_entry *e,
                           void *buf, size_t n) {
    struct userfs_channel *c = cookie;
    spin_lock(&c->lock);
    p9_msg req = { .op = P9_READ, .tag = ++c->next_tag,
                   .handle = e->userfs_handle,
                   .offset = e->offset, .length = (uint32_t)n };
    vfs_write(c->req_fd, &req, sizeof req);
    /* No payload for a read REQUEST; the data comes back in the reply. */

    p9_msg rsp;
    vfs_read(c->rsp_fd, &rsp, sizeof rsp);   /* blocks */
    /* validate tag, fetch payload */
    long got = (long)rsp.length;
    if (got < 0) { spin_unlock(&c->lock); return got; }
    vfs_read(c->rsp_fd, buf, got);
    e->offset += got;
    spin_unlock(&c->lock);
    return got;
}
```

The `spin_lock` serialises requests on a per-mount basis
for v1. v2 will replace the lock with a tag-keyed wait
queue so multiple in-flight requests can pipeline; that's
purely a performance optimisation and not required to
ship.

## Two new syscalls

```c
#define SYS_MOUNT    N    /* (mountpoint, req_fd, rsp_fd) -> mount_id */
#define SYS_UMOUNT   N    /* (mount_id)                   -> errno   */
```

`sys_mount` allocates a `struct mount` slot, populates a
`userfs_channel` with the two fds (which the caller has
already created as pipes, sockets, or whatever), and
registers `userfs_ops` for the given prefix.

`sys_umount` drains in-flight requests, marks the mount
inactive (subsequent opens get `-ENOENT_VFS`), waits for
existing open fds to close, then frees the slot.

Only `init` (pid 1) is allowed to call `sys_mount` in v1.
Future: a capability-based system or a per-user mount
namespace.

## The user side: `struct userfs_handler`

A library in `userspace/libfs/` provides the boilerplate.
Daemons write something like:

```c
#include "libfs/userfs.h"

static long my_open(const char *path, int flags, struct userfs_handle *out)
{
    /* daemon-specific: validate path, allocate handle */
    out->handle_id = next_id();
    out->size = ...;
    return 0;
}

static long my_read(struct userfs_handle *h, uint64_t off,
                    void *buf, size_t n)
{
    /* daemon-specific: copy bytes from your representation */
    return memcpy_clamp(buf, n, h->blob + off, h->size - off);
}

int main(void) {
    struct userfs_handler ops = {
        .open = my_open, .read = my_read, .close = my_close,
        /* others can be NULL; library returns ENOSYS to kernel */
    };
    return userfs_serve("/clipboard", &ops);   /* never returns */
}
```

`userfs_serve` calls `pipe()` twice, calls `sys_mount`,
then loops reading 9P requests off one pipe and writing
replies to the other.

## Bootstrap and ordering

`init` (`userspace/init/init.c`) gains an extra phase:

```c
spawn("/bin/clipboardd");   /* mounts /clipboard */
spawn("/bin/procd");        /* mounts /proc, replacing kernel procfs (later) */
/* ...then spawn desktop / taskbar / launcher / sh as today */
```

This raises the bootstrap question: what if a userspace
fs server needs files from a filesystem that's itself a
userspace fs? Answer: don't allow it. The mount table
imposes a strict ordering — userspace fs mounts cannot
depend on other userspace fs mounts being live. In
practice that means our daemons must be statically
self-contained (the binary, on `/bin/`, is OSFS-1, which
is always in-kernel).

## Migrating procfs out of the kernel (case study)

Once Chapter 132 has procfs behind `procfs_ops` and
Chapter 140 has the userfs machinery, we can write
`/bin/procd` as a normal C program that:

- Opens `/dev/kthread_snapshot` (a new in-kernel character
  device that returns the thread-table snapshot as a blob)
- Translates 9P requests into the same textual rendering
  procfs.c does today.

Then we drop `kernel/core/procfs.c` and replace the mount
table entry from in-kernel `procfs_ops` to a `userfs_ops`
backed by `/bin/procd`. **No userspace tool notices** —
`ls /proc`, `ps`, `top` all keep working because their
view of `/proc/` is the same paths returning the same
bytes.

This is the validation that the abstraction is right: a
filesystem can move between kernel and userspace without
its clients knowing.

## What user-space filesystems unlock

Concrete things we'd build on top of the userspace-fs
machinery (some are re-implementations of already-shipped
IPC services, some are new):

- **`/bin/clipboardd`** mounts `/clipboard/text`. Today
  the clipboard is a chapter-108 IPC service with its
  own bespoke API; step
  [144](144-clipboardd-port.md) re-fronts it as a
  file. `notepad` Ctrl-C writes to `/clipboard/text`;
  Ctrl-V reads from it. No new syscalls.
- **`/bin/audiofs`** mounts `/dev/audio`. Programs play
  sound by `cat foo.raw > /dev/audio`. No new syscalls.
- **`/bin/cookiesd`** mounts `/sys/browser/cookies/`.
  The browser reads/writes cookies through the file
  interface. Inspectable by `cat`, editable by `notepad`.
- **`/bin/sshd`-style daemons** can expose connection
  state as files: `/net/tcp/22/sessions/<id>/peer`,
  `/net/tcp/22/sessions/<id>/stdin`. Plan 9's actual
  shape; surprisingly debuggable.
- **A debugger** can mount `/proc/<pid>/mem/` as a
  read/write filesystem, with `cat /proc/19/mem/0x401000`
  doing what `gdb x/` does today.

The unifying theme: **anything that wants to be inspected
and edited becomes a filesystem.** This is the Plan 9
insight that Unix kind-of-half-adopted with `/proc` and
then never followed through on. We can.

## Failure modes and what we do about them

User-space filesystems introduce failure modes the
in-kernel ones don't have. They are worth naming.

| Failure                                | Response                                      |
| -------------------------------------- | --------------------------------------------- |
| Daemon crashes mid-request             | Kernel returns `-EIO_VFS` to the open caller; subsequent opens get `-ENOENT_VFS`; init reaps via SIGCHLD (chapter 77) |
| Daemon hangs (never replies)           | Per-request 5-second timeout; kernel returns `-ETIMEDOUT_VFS`; mount stays live |
| Daemon reads its own filesystem        | Detected by checking `current->pid == c->owner_pid`; returns `-EDEADLK_VFS` to break the cycle |
| Daemon sends malformed 9P              | Connection torn down, mount marked inactive, daemon killed |
| Two daemons claim the same mount point | `sys_mount` returns `-EEXIST_VFS` for the second |
| Mount has open fds when `umount` runs  | `umount` waits up to 1 second; if any fds remain, returns `-EBUSY_VFS` |

The "daemon reads its own filesystem" case is the most
subtle. A `procd` that tried to `cat /proc/<self>/status`
during a request would deadlock against its own `lock`.
We detect this in the kernel by carrying `owner_pid`
in the channel and erroring at `open` time.

## Why not just use sockets / D-Bus / a shell pipe?

We considered three alternatives:

- **Sockets.** Every clipboard/audio/etc daemon would
  define its own protocol. No reuse, no `ls`-browsability,
  no `cat` as the universal debug tool.
- **Shell pipes** (each daemon is a pipeline filter).
  Works for streaming data (audio) but has no namespace
  — there's no `ls /clipboard/text` if `clipboardd` is
  just `cat | tee`. And nothing is browsable.
- **A `/var/run/<daemon>.sock` D-Bus shape.** Real
  systems do this; we don't have D-Bus and writing it
  would be its own book chapter. Plus the same
  no-namespace, no-browse problem.

The 9P approach gives us all three properties (named
namespace, file-shaped operations, browsable) for the
cost of one new fs_ops driver and two new syscalls.

## What this chapter does NOT propose

- **Full Plan-9 9P compatibility.** No T-message family,
  no fid walks, no auth. If we ever want to talk to a
  real 9P server (Linux's `v9fs`, Plan 9's `fossil`), we
  can adapt the protocol later — but starting with our
  simplified version keeps the kernel code small.
- **A new IPC primitive.** Kernel ↔ userspace channels
  are existing pipes; the kernel side opens them with
  the same `vfs_open` everyone else uses.
- **Caching.** Each kernel-side request goes straight to
  the daemon. We may add a per-mount read cache later
  for read-heavy filesystems, but v1 is straight-through.
- **A change to the syscall ABI for userspace clients.**
  `open`, `read`, `write`, `close` continue to work
  unchanged. The whole point of putting userspace
  filesystems behind the mount table is that callers
  don't know.

## Userspace application updates

These fall out naturally from having the abstraction:

### `notepad` gains clipboard

Once `/bin/clipboardd` mounts `/clipboard/text`, notepad's
Ctrl-C/Ctrl-V become two-line additions: open the file,
write/read, close. No notepad-side knowledge of where the
clipboard lives or how it's implemented.

### `sh` gains `mount` and `umount` builtins

Probably implemented as `/bin/mount` and `/bin/umount`
binaries rather than builtins, but the shell needs to
recognise the commands and pass them through. (Same
pattern as `ps`, `top`, `ls`.)

### `ls` and `cat` get auto-discovery

The mount table is already exposed via `SYS_MOUNTS`
(shipped in [chapter 138](138-sys-mounts.md)), so
`ls /` shows every mounted prefix as a directory.
`cat <unknown_mount>` falls through to the userspace
daemon. No `ls`-side or `cat`-side changes needed
beyond what chapter 132 already provides.

### `top` shows daemon stats

Once `procd` is itself a userspace process,
`/proc/<procd-pid>/status` shows up under `ps` like any
other process. The clipboard's RSS, the audio daemon's
queue depth — all observable as files.

## Implementation sequence

Sweep-green after every step.

1. **Land `struct userfs_channel`, `userfs_ops`, and the
   p9_msg encoder/decoder.** No mounts yet, just the
   kernel-side machinery. Unit-testable via a kernel
   self-test that loops back to itself.

2. **Land `SYS_MOUNT` / `SYS_UMOUNT`.** Tested with a
   shell-level `/bin/mount` that takes the path + two
   already-open fds as arguments.

3. **Write `userspace/libfs/userfs.c`.** The boilerplate
   handler library. One test program (`/bin/echofs`) that
   mounts `/echo/` and returns whatever it's read.

4. **Write `/bin/clipboardd`.** First production user.
   Adds Ctrl-C/Ctrl-V to `notepad`.

5. **Port procfs to `/bin/procd`.** Validates that
   filesystems can migrate kernel ↔ userspace without
   client changes. Drops `kernel/core/procfs.c`.

6. **Per-request timeout + `EDEADLK` detection.**
   Hardening pass. Triggered by deliberately writing a
   daemon that hangs and a daemon that reads itself.

Each step is a book-chapter-sized milestone. Total: 6 new
chapters in Part XVI, on top of the 1–7 chapters in Chapter
113's implementation plan.

## Why the order matters

Chapter 132 (mount table) is a pure refactor with no new
features. Chapter 140 (userspace fs) is a major new
feature.

If we had shipped 114 first (i.e., added a sixth prefix
ladder for `/userspace_fs_$N/` paths), we'd have
cemented the prefix-special-casing pattern even harder
and the eventual 113 refactor would have had to undo
it. Doing 113 first gave 114 a clean abstraction to
slot into, and 113 shipped value (clean dispatch,
runtime mount) immediately — which is exactly the
order we ended up landing them in.

## When to schedule

Chapter 132 is the prerequisite. Once 113 is done and the
sweep is green, Chapter 140 can begin. Both together are
substantial — probably 3–4 weeks of focused work for the
implementation chapters. Worth doing because every
subsequent feature (clipboard, audio, anything
inspectable) gets dramatically simpler once we can write
it as "a userspace daemon that's also a filesystem."

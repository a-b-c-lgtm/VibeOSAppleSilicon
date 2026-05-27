# Chapter 141 — Step 1: the kernel half of userfs

[Chapter 140](140-userspace-filesystem-servers.md) committed
us to six landings, each sweep-green, that turn a userspace
process into a real filesystem mount. This is the first of
them. By the end of this chapter the kernel can construct a
`struct userfs_channel`, send a 9P-shaped request to a
hypothetical daemon, parse the reply, and dispatch every
`fs_ops` method through that round-trip. No syscall exposes
the machinery yet — there's no way to *create* a channel from
userspace until [chapter 142](142-sys-mount-umount.md) lands
`SYS_MOUNT` — so the visible behaviour of the kernel is
unchanged. That is the point: we want to be able to land,
review, and revert the protocol layer without any user-facing
surface area on the line.

The chapter is mostly about the four design choices baked into
the wire format and the lifecycle. They are individually small;
the value is in writing them down before there's any
caller. Once a daemon is talking 9P to the kernel, changing
the byte layout becomes a coordinated upgrade. Right now it's
a header edit.

## What lives in the new files

Two new files in `kernel/core/`:

- [`kernel/core/userfs.h`](../../../kernel/core/userfs.h) — opcodes,
  the 32-byte `struct p9_msg`, the channel handle, and the
  three exported functions (`userfs_channel_create`,
  `userfs_channel_destroy`, `userfs_call`) plus the
  `g_userfs_ops` vtable symbol.
- [`kernel/core/userfs.c`](../../../kernel/core/userfs.c) — the
  channel allocator, the request/reply marshaller, and the ten
  `fs_ops` methods that bridge into `userfs_call`.

Plus three small edits in existing files:

- [`kernel/core/vfs.h`](../../../kernel/core/vfs.h) gains a new
  `fd_kind` value (`FD_USERFS_FILE`), two fields on
  `struct fd_entry` (`userfs_ch`, `userfs_handle`), and a
  forward declaration of `struct userfs_channel`.
- [`kernel/core/vfs.c`](../../../kernel/core/vfs.c) gains a
  `FD_USERFS_FILE` branch in `vfs_read` and `vfs_close`, plus
  the field-zero discipline in `fd_table_create` and
  `fd_table_unref` so every newly allocated fd starts with
  null `userfs_ch`.
- [`Makefile`](../../../Makefile) adds `kernel/core/userfs.c` to
  `KERNEL_OBJS`.

That's the full surface. There is intentionally no syscall
glue, no mount-table touch, and no userspace header — those
land in 142. If we hate the wire format tomorrow, deleting
this chapter is `git revert` plus removing the new
`fd_kind` value.

## Design choice 1: simplified 9P, not real 9P

Plan 9's 9P2000 is twenty messages with a fid walk, a session
header, and an auth dance. We don't need any of that. The
filesystems we want behind this protocol — clipboard, procfs,
audio, eventually cookies — are tiny per-mount caches with no
notion of cross-session identity. We collapse the surface to
nine opcodes:

```c
#define P9_OP_OPEN    1u
#define P9_OP_READ    2u
#define P9_OP_WRITE   3u
#define P9_OP_CLOSE   4u
#define P9_OP_LISTDIR 5u
#define P9_OP_UNLINK  6u
#define P9_OP_MKDIR   7u
#define P9_OP_IS_DIR  8u
#define P9_OP_LOAD    9u
#define P9_REPLY      0x80000000u
```

One per `fs_ops` method. Replies are the same opcode with
`P9_REPLY` ORed into the high bit. The kernel matches reply
tags to outstanding requests by sequence number, so there is
no protocol-level multiplexing logic; each channel serialises
requests strictly under its own spinlock, which means an
in-flight request always has exactly one outstanding tag and
the comparison is a tagged equality check, not a search.

The thing we give up by skipping real 9P is the ability to
mount a Linux `v9fs` server or a Plan 9 fossil from inside our
guest. The trade is fewer messages, smaller header, no
walk-state to manage in the channel struct, and a wire format
that fits on a screen. We can always add a `T-version`
handshake later if we want to talk to a foreign server; the
on-disk encoding of our nine messages is then just one of
several profiles the channel can speak.

## Design choice 2: one struct, 32 bytes, fixed layout

The header is a plain struct with no version field:

```c
struct p9_msg {
    uint32_t op;
    uint32_t tag;
    uint32_t handle;
    uint32_t flags;
    uint64_t offset;
    uint32_t length;
    int32_t  status;
};
```

We rely on identical ABI between kernel and userspace (same
toolchain, same target triple, same struct packing) rather
than serialise field-by-field. That's defensible because our
userspace is built from the same source tree as our kernel by
the same `aarch64-elf-gcc`. If we ever support an
out-of-tree daemon written in another language, we'd need to
hand-marshal — but that's a problem for whoever wants to
write the second daemon in Rust, not for us today.

`length` is the count of bytes following the header. For
requests it carries the path or the write payload. For
replies it carries the read result or the listdir name. Two
small twists:

- `READ` requests carry `length = 0` because there's no
  request payload; the daemon needs to know how many bytes
  the caller wanted, so the byte count rides in `flags`. The
  matching comment in
  [`kernel/core/userfs.c`](../../../kernel/core/userfs.c)
  (`userfs_op_read`) documents the convention; the daemon
  side recovers it in
  [`userspace/libfs/userfs.c`](../../../userspace/libfs/userfs.c)'s
  `P9_OP_READ` switch arm.
- `LISTDIR` piggybacks similarly: the request's `flags`
  carries the iteration index, the reply's `flags` carries
  the entry's file/dir type.

This is the kind of overloading that pays for itself once.
The cost is that the field-meaning table is one page longer
in the chapter writeup; the benefit is that the wire format
doesn't grow a per-op header struct family.

## Design choice 3: pair of anonymous pipes, kernel-owned

A channel is two pipes — one carries requests
(kernel → daemon), one carries replies (daemon → kernel) —
plus a spinlock, a tag counter, an "alive" flag, an open-fd
counter, and the owner pid:

```c
struct userfs_channel {
    struct pipe *req_pipe;
    struct pipe *rsp_pipe;
    int         owner_pid;
    spinlock_t  lock;
    uint32_t    next_tag;
    int         alive;
    int         open_fds;
    const char *prefix;
};
```

The kernel calls `pipe_alloc()` to create each pipe, then
transfers one end of each to the daemon's fd table when
`SYS_MOUNT` runs in chapter 142. After that hand-off the
channel owns exactly two pipe refcounts: the writer end of
the request pipe (it writes requests) and the reader end of
the reply pipe (it reads replies). The daemon owns the
opposite ends.

The refcount accounting matters because we use the same
`pipe_unref()` API for both halves. `userfs_channel_destroy`
drops only the kernel-owned side:

```c
if (c->req_pipe) {
    pipe_unref(c->req_pipe, PIPE_REF_W);
    c->req_pipe = NULL;
}
if (c->rsp_pipe) {
    pipe_unref(c->rsp_pipe, PIPE_REF_R);
    c->rsp_pipe = NULL;
}
```

If we dropped both refs on each pipe we'd double-unref the
side already transferred to the daemon, corrupt the
refcount, and eventually free a pipe the daemon is still
reading. The matching half-allocated-cleanup branch in
`userfs_channel_create` *does* drop both refs on the
request pipe — but only on the early-exit path where the
reply pipe allocation failed before any hand-off happened.
The two cases look superficially similar; the comment in
both call sites is the discipline that keeps them honest.

## Design choice 4: hold the spinlock across the whole RPC

`userfs_call` takes `c->lock` before writing the request,
keeps it held through the reply read, and releases it after
copying the payload out:

```c
spin_lock(&c->lock);
if (!c->alive) { spin_unlock(&c->lock); return -EIO; }

/* ... write header, write payload, read header, read payload ... */

if (rsp_out) *rsp_out = rsp;
spin_unlock(&c->lock);
return 0;
```

This makes the channel strictly serial — only one request in
flight per mount, no pipelining. The cost is throughput on a
mount with concurrent callers; the benefits are several:

- The tag counter becomes a non-issue: every reply matches
  exactly one outstanding request.
- The reply parser doesn't need a per-request waitqueue;
  the calling thread reads its own reply directly off
  `rsp_pipe`.
- If the daemon writes the reply chunked, we drain the
  remainder under the lock — no other thread can observe the
  half-drained pipe.

We'll relax this in
[chapter 146](146-timeouts-and-deadlock.md) when we add
per-request timeouts and the daemon-self-mount deadlock
detector. For v1 the serialisation is the simplest thing
that's correct.

The lock also encodes a safety check we want from day one:

```c
struct thread *me = thread_current();
if (me && c->owner_pid > 0 && me->id == c->owner_pid)
    return -EDEADLK;
```

A `procd` that tried to `cat /proc/<self>/status` mid-request
would deadlock against its own request spinlock. Detecting
the direct case at `userfs_call` entry — before we take the
lock — turns a hang into a clean errno. Step 6 will widen
this to cover transitively-held mounts (procd opens
`/clipboard/foo` while clipboardd opens `/proc/<procd-pid>`);
the direct check here breaks the obvious case and costs one
field compare.

## The new fd_kind value

The dispatcher in
[`kernel/core/vfs.c`](../../../kernel/core/vfs.c) needs to know that
an fd backed by a userfs mount routes through `g_userfs_ops`
instead of any of the pre-existing per-driver branches. We
add a new value to the kind enum and two fields to
`struct fd_entry`:

```c
enum fd_kind {
    /* ... existing values ... */
    FD_USERFS_FILE,
};

struct fd_entry {
    /* ... existing fields ... */
    struct userfs_channel *userfs_ch;
    uint32_t               userfs_handle;
};
```

`userfs_handle` is the daemon-allocated cookie returned by
`P9_OP_OPEN`. Subsequent `READ`/`WRITE`/`CLOSE` quote it
verbatim so the daemon can look up its own per-fd state.

The reason these two fields land *now* and not in 142 is
the field-zero discipline. Every allocator in `vfs.c` that
ever produces a fresh `fd_entry` must initialise these
fields to zero or the userfs branch in `vfs_close` will
chase a stale pointer. The two places that matter are
`fd_table_create` (init loop for FD_TABLE_SIZE entries
plus the fd 0/1/2 mini-loop for the std streams) and
`ramfs_op_open` (the generic open path that all the
in-kernel filesystems eventually go through).

Adding the field but forgetting the field-zero is exactly
the GCC freestanding `= {0}` trap (large aggregate inits
become an implicit `memset` call that the freestanding
kernel doesn't link). We avoid it by
explicit assignment of every field, not aggregate
initialisation. The cost is six new lines in the two
initialiser loops; the benefit is a kernel that doesn't
randomly route fd-3 close-syscalls through a heap address
left over from the previous process.

## The vfs.c branches

`vfs_read` gets one new arm between `FD_SRV_CONN` and
`FD_TMPFS_RW`:

```c
} else if (e->kind == FD_USERFS_FILE) {
    return g_userfs_ops.read(e->userfs_ch, e, buf, n);
}
```

`vfs_close` gets a corresponding cleanup:

```c
} else if (e->kind == FD_USERFS_FILE && e->userfs_ch) {
    (void)g_userfs_ops.close(e->userfs_ch, e);
    e->userfs_ch = NULL;
    e->userfs_handle = 0;
}
```

And the close-loop inside `fd_table_unref` (the per-process
exit path) gains the same `userfs_ch != NULL` check so we
RPC a `P9_OP_CLOSE` to the daemon for every open file when
the process dies, not just the ones it explicitly closed.

`vfs_write` is conspicuously not on this list. Userfs
writes go through `sys_write` directly because the dispatch
needs to chunk-loop with the daemon's reply-acknowledged
byte count, and `sys_write` already owns the chunk loop. The
chunk loop lands in 142 alongside `SYS_MOUNT`.

## Why no SYS_MOUNT yet

The whole story of this chapter — channel construction, the
vtable, the fd_kind, the close path — is dead code until
something allocates a channel and registers it in the mount
table. The next chapter, [142](142-sys-mount-umount.md),
adds the two syscalls that make this code reachable.

We could have landed them together. The reason to split is
the same reason chapter 132 split the table from the first
driver port: a smaller landing surface, a smaller revert if
the design turns out wrong, and the ability to review the
protocol design separately from the syscall ABI. The next
chapter has its own pile of decisions (how many fds to
return, whether `umount` blocks on open fds, what `-EBUSY_VFS`
means at the syscall boundary) and folding those into one
commit would make the review impossibly wide.

## What got verified

- `make` succeeds with `kernel/core/userfs.c` added to
  `KERNEL_OBJS`. The new file compiles cleanly with the
  same `-Wall -Werror -ffreestanding` flags as the rest of
  the kernel; no implicit `memset` calls, no struct-init
  traps.
- The kernel boots through `userspace_demo()`. No reaper
  trap was tripped because we haven't spawned any
  long-lived daemon yet.
- The full regression sweep stays green. Every existing
  test that opens / reads / writes a file is now going
  through a `vfs_*` dispatcher that explicitly tests
  `e->kind != FD_USERFS_FILE` before falling through — and
  every test still passes, confirming the field-zero
  discipline is correct.

## Applied to

- **Existing apps modified**: none. The `FD_USERFS_FILE`
  branch is unreachable until 142.
- **New apps added**: none. Daemon-side comes in 143.
- **Tests upgraded**: none. The whole-system sweep
  validates the field-zero discipline indirectly by
  continuing to pass.
- **Tests added**: none in this chapter. Step 1 has no
  user-visible surface to test; we lean on the chapter
  142/143 tests to exercise the protocol end-to-end.

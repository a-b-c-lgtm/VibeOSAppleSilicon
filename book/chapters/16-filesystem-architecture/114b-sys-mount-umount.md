# Chapter 114b — Step 2: `SYS_MOUNT` and `SYS_UMOUNT`

[Chapter 114a](114a-kernel-userfs-module.md) landed the
kernel's userfs protocol module — channels, p9_msg encoder,
the `g_userfs_ops` vtable — without a way to instantiate any
of it from userspace. This chapter wires up the two syscalls
that close the loop. After this chapter a process can call
`mount_kernel("/echo", fds)`, get back a pair of fds onto a
fresh request/reply pipe, and any other process that opens
`/echo/anything` lands in `g_userfs_ops.open` against that
channel.

There is still no daemon to serve those requests — the libfs
boilerplate and `/bin/echofs` land in
[chapter 114c](114c-libfs-and-echofs.md). What we get here
is the kernel-side abstraction at full strength, exercisable
manually by writing the smallest possible daemon (a `while
(read; write)` shell loop, eight lines of C) if you wanted
to. The sweep doesn't add a new test for this step because
the syscalls have no meaningful behaviour until a daemon
exists; chapter 114c's `test_userfs_echo.py` covers them
end-to-end.

## The two syscalls

The numbers and the doc comments are in
[`kernel/core/syscall.h`](kernel/core/syscall.h):

```c
SYS_MOUNT  = 96,   /* (prefix, fds_out[2]) -> mount_id or -errno */
SYS_UMOUNT = 97,   /* (mount_id)           -> 0 or -errno */
```

The userspace wrappers in
[`userspace/libc/syscall.h`](userspace/libc/syscall.h) look
exactly the way you'd expect:

```c
static inline long mount_kernel(const char *prefix, int fds_out[2])
{ return _svc2(SYS_MOUNT,  (uint64_t)prefix, (uint64_t)fds_out); }

static inline long umount_kernel(int mount_id)
{ return _svc1(SYS_UMOUNT, (uint64_t)mount_id); }
```

`mount_kernel` returns the slot index from `vfs_mount_register`
so the daemon can pass it to `umount_kernel` later. Negative
returns are errnos. The "kernel" suffix is to distinguish
these from a future `/bin/mount` builtin that wraps them; the
shell command is the user-facing thing and the syscall is
the primitive.

## What `sys_mount` does

The implementation in
[`kernel/core/syscall.c`](kernel/core/syscall.c) is short
enough to walk through end to end. It does four things, in
strict order:

1. Validate and copy the prefix string from userspace.
2. Verify there's no existing mount at that prefix.
3. Allocate a `userfs_channel` and register it in the mount
   table.
4. Install the daemon-side ends of the two pipes into the
   caller's fd table and write the fd numbers back to
   `fds_out[2]`.

Each step has its own failure mode and its own cleanup. The
function reads like a list of `goto fail` ladders in spirit
but C89-cleanly because the kernel never throws.

### Step 1: copy and validate the prefix

```c
char prefix_buf[USERFS_PREFIX_MAX + 1];
if (copy_user_prefix(prefix_uptr, prefix_buf,
                     sizeof prefix_buf) < 0) return -EFAULT;

size_t plen = k_strlen(prefix_buf);
if (plen == 0 || plen >= USERFS_PREFIX_MAX) return -EINVAL;
if (prefix_buf[0] != '/') return -EINVAL;
if (plen > 1 && prefix_buf[plen - 1] == '/') return -EINVAL;
```

`copy_user_prefix` is a thin wrapper over
[`kernel/core/uaccess.h`](kernel/core/uaccess.h)'s
`copy_string_from_user`. We cap at 32 bytes
(`USERFS_PREFIX_MAX`) because the prefix lives in the mount
table for the lifetime of the mount, and the table is
statically sized; a path that long is already absurd, and a
malicious caller can't blow stack by feeding us a 1 MiB
string.

The three rejections are:

- Empty prefix — would alias the root mount.
- Doesn't start with `/` — would never match anything in
  `vfs_resolve`'s longest-prefix walk.
- Trailing slash — would confuse the resolver. `/echo/` and
  `/echo` should behave identically; we canonicalise on the
  way in by rejecting the slashed form.

### Step 2: reject duplicate mounts

We scan the existing mount table for an entry with the same
prefix:

```c
for (int i = 0; i < vfs_mount_count(); i++) {
    const struct mount *m = vfs_mount_at(i);
    if (m && m->prefix && k_streq(m->prefix, prefix_buf))
        return -EEXIST;
}
```

This is O(n) over a table of at most 16 entries, which is
fine. The point is that if two daemons race to mount the
same path, exactly one wins; the loser sees `-EEXIST` and
can either retry on a different prefix or exit. There is no
fancy "shadowing" semantics where a later mount stacks on
top of an earlier one; we don't need it and Plan 9's bind
operator is a separate feature we're not building.

### Step 3: allocate the channel and register

```c
struct userfs_channel *c =
    userfs_channel_create(prefix_buf_dup, t->id);
if (!c) return -ENOMEM;

int slot = vfs_mount_register(prefix_buf_dup, &g_userfs_ops,
                              c, 0u /* flags */);
if (slot < 0) {
    userfs_channel_destroy(c);
    kfree(prefix_buf_dup);
    return slot;
}
```

`prefix_buf_dup` is a heap copy of the validated prefix; the
mount table stores the pointer for the life of the mount and
we don't want to point it at our stack frame. We're careful
to free `prefix_buf_dup` only if registration fails — on
success ownership transfers to the mount table and is
released by `sys_umount`'s symmetric `kfree` call.

`t->id` is the calling thread's pid. We snapshot it into the
channel as `owner_pid` so `userfs_call` can refuse to enter
a daemon's own filesystem with `-EDEADLK`. The mechanism
landed in 114a; this is the first call site that fills the
field with a non-zero value.

### Step 4: install the daemon-side fds

The new channel owns the writer end of `req_pipe` and the
reader end of `rsp_pipe`. The daemon needs the *other* ends:
read from `req_pipe`, write to `rsp_pipe`. We hand them over
without bumping refcounts — `pipe_alloc` returned each pipe
with `r_refs = 1` and `w_refs = 1`, and the channel only
retained two of the four refs:

```c
int rfd = fd_table_alloc(t->files, 3);
if (rfd < 0) goto fail_unregister;
fd_init_pipe(&t->files->entries[rfd], c->req_pipe, FD_PIPE_R);

int wfd = fd_table_alloc(t->files, 3);
if (wfd < 0) { fd_table_free(t->files, rfd); goto fail_unregister; }
fd_init_pipe(&t->files->entries[wfd], c->rsp_pipe, FD_PIPE_W);

/* ...write {rfd, wfd} back to fds_out[2] via uaccess_copy_to_user... */
return slot;
```

`fd_table_alloc(t->files, 3)` skips fds 0/1/2 (stdin/stdout/
stderr) because real daemons want predictable fd numbers for
their service pipes — clobbering stdout would be hostile to
`printf`. The two `fd_init_pipe` calls field-zero every
slot per the chapter-113 discipline.

The cleanup path on `fd_table_alloc` failure unregisters the
mount with `vfs_mount_remove(slot)` (the new helper, see
below) and destroys the channel. The cleanup order matters:
the channel destruction drops the kernel-owned refs, which
makes any in-flight daemon `read` return EOF; the mount
table removal happens *before* the channel goes away, so by
the time the channel's pipes are gone the resolver has
already stopped pointing at them.

## What `sys_umount` does

The implementation is the inverse:

```c
if (mid < 0 || mid >= vfs_mount_count()) return -EINVAL;
const struct mount *m = vfs_mount_at(mid);
if (!m || m->ops != &g_userfs_ops) return -EINVAL;

struct userfs_channel *c = (struct userfs_channel *)m->cookie;
if (c->open_fds > 0) return -EBUSY_VFS;

const char *prefix_to_free = m->prefix;
c->alive = 0;
vfs_mount_remove(mid);
userfs_channel_destroy(c);
kfree((void *)prefix_to_free);
return 0;
```

Three checks:

- The mount id is in range.
- The mount is actually a userfs mount, not an in-kernel
  filesystem like ramfs or osfs. We don't want a buggy
  daemon umounting `/proc` from under the kernel.
- No open fds against the mount.

The last check is the source of `-EBUSY_VFS`. Step 6 will
add a one-second grace period so a daemon racing its own
fds at shutdown gets a chance to settle; for v1 the error is
immediate. The `open_fds` counter is incremented by
`userfs_op_open` (114a) and decremented by `userfs_op_close`
(114a + the `fd_table_unref` close loop), so it tracks the
live-fd count accurately whether the process closes them
explicitly or exits.

The order on the way out — `c->alive = 0` first,
`vfs_mount_remove(mid)` second, `userfs_channel_destroy(c)`
third — is chosen so concurrent `vfs_open` calls see the
"dead" flag before the mount table entry disappears, and the
pipe refcounts drop only after the resolver can no longer
hand out new references to the channel. The `kfree` on the
prefix is the last thing because the mount table entry is
gone by then; freeing it earlier would risk the resolver
loop indexing a freed string.

## The new helper: `vfs_mount_remove`

Chapter 113 didn't need a removal API because the in-kernel
mounts never unmounted. We add it now:

```c
/* kernel/core/vfs.c */
int vfs_mount_remove(int idx)
{
    if (idx < 0 || idx >= g_mount_count) return -EINVAL;
    /* Compact: shift everything past idx down by one and null
     * the trailing slot.  The vfs_resolve longest-prefix walk
     * doesn't care about index ordering, but compacting keeps
     * g_mount_count truthful. */
    for (int i = idx; i < g_mount_count - 1; i++)
        g_mounts[i] = g_mounts[i + 1];
    g_mounts[g_mount_count - 1].prefix = NULL;
    g_mounts[g_mount_count - 1].ops    = NULL;
    g_mounts[g_mount_count - 1].cookie = NULL;
    g_mounts[g_mount_count - 1].flags  = 0;
    g_mount_count--;
    return 0;
}
```

Compaction (rather than tombstoning) is the simpler choice
because nothing in `vfs_resolve` cares about stable slot
indices. The cost is that a long-running daemon that
remembers its slot id across an unrelated `umount` would
find its slot id no longer valid — but the slot id is
returned from `sys_mount` precisely so the daemon can pass
it to `sys_umount`, and that's the only place it should
ever flow back into kernel space.

## The `sys_write` chunk loop

Userfs writes go through `sys_write` instead of `vfs_write`
because the byte-count semantics differ. `vfs_write` is a
single dispatch; the userfs daemon may legitimately accept
fewer bytes than the caller requested (a write to a
fixed-size buffer, for example), and the kernel needs to
loop until either the daemon stops accepting or the whole
slice is consumed.

The arm in
[`kernel/core/syscall.c`](kernel/core/syscall.c) looks like:

```c
} else if (e->kind == FD_USERFS_FILE) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > 256) chunk = 256;  /* p9 budget, conservative */
        long w = g_userfs_ops.write(e->userfs_ch, e,
                                    (const uint8_t *)kbuf + off,
                                    chunk);
        if (w <= 0) break;
        off += (size_t)w;
    }
    return (long)off;
}
```

256 bytes is a conservative chunk because the libfs scratch
buffer caps at `P9_MAX_PAYLOAD` (2048) and we want headroom
for the daemon's own per-request bookkeeping. A short
daemon-accepted write breaks the loop cleanly — the caller
sees `off` bytes returned, which matches POSIX `write`
semantics for partial writes.

## Why the daemon doesn't own the prefix

The prefix string is allocated by `sys_mount` and freed by
`sys_umount`. The daemon never sees the storage. We could
have passed the kernel a daemon-owned pointer, but then a
crashing daemon would leave the mount table holding a
dangling pointer. Kernel-owned strings are the simpler
invariant.

The same reasoning applies to the channel: the kernel
allocates it, the kernel destroys it. A daemon that exits
without calling `umount_kernel` leaks the channel — but
chapter 114f's hardening pass adds an exit hook to
`fd_table_unref` that walks the mount table and unmounts
any channel whose owner pid matches the dying process. For
v1, we trust the daemon to clean up.

## What got verified

- `make` succeeds with the new dispatcher cases in
  `syscall.c` and the new helper in `vfs.c`.
- The kernel boots, the regression sweep stays green. Every
  existing syscall still routes correctly because
  `SYS_MOUNT` / `SYS_UMOUNT` are new numbers slotted at
  the end of the dispatch switch; the surface for everyone
  else is unchanged.
- The userspace wrappers compile against
  `userspace/libc/syscall.h` and a freestanding ELF can
  link them — verified by building `/bin/echofs` (chapter
  114c) which is the first caller.

## Applied to

- **Existing apps modified**: none. Daemon-side machinery
  is in 114c.
- **New apps added**: none. The shell could spawn
  `mount_kernel("/x", fds)` as a one-liner but there's no
  reason to without a daemon to serve requests.
- **Tests upgraded**: none. The kernel-table change is
  exercised indirectly by the existing `test_mounts.py`
  (chapter 113f), which still passes despite the new
  mount-removal code path being added.
- **Tests added**: none in this chapter — see 114c for
  `test_userfs_echo.py` covering the full open / read /
  write / close round-trip.

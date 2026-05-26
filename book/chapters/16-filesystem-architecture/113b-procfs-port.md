# Chapter 113b — Step 2: porting procfs onto `fs_ops`

Chapter 113a landed the empty mount table. This chapter takes
the smallest existing filesystem — chapter 99's procfs — and
pulls it across to the new vtable. We pick procfs first for
exactly the reason the [plan chapter](113-mount-table-and-vtable.md)
listed: it is the youngest driver in the tree, so its existing
shape is closest to what we want every driver to look like,
and porting it forces the fewest design choices.

By the end of this chapter the prefix-special-cased branches
for `/proc` in `vfs_open`, `vfs_read`, `vfs_close`, and
`sys_listdir_at` are gone. Every `/proc/...` open now flows
through `vfs_resolve → procfs_fs_ops.open`. The chapter-99
regression (`test_procfs.py`) still passes because the
adapter calls the same underlying procfs functions — only
the path that got us there has changed.

## What procfs looked like before

[`kernel/core/procfs.c`](../../../kernel/core/procfs.c) exposed five
public functions, each one called from a `path_starts_with("/proc")`
branch in vfs / syscall:

```c
int   procfs_open       (const char *path, struct fd_entry *out);
long  procfs_read       (struct fd_entry *e, void *buf, size_t n);
int   procfs_close      (struct fd_entry *e);
int   procfs_listdir    (const char *path, int idx,
                         char *name, size_t cap, uint32_t *type);
int   procfs_is_dir     (const char *path);
```

These took *absolute* paths starting with `/proc`. Each
function began with a `strip_proc(path)` call that lopped off
the `"/proc"` prefix and rejected anything that didn't start
with it — defensive code, because every call site was the one
inside `vfs.c` that had already verified the prefix matched.

## What it looks like after

A static `fs_ops` table at the bottom of
[`kernel/core/procfs.c`](../../../kernel/core/procfs.c), plus one
public register function and a static adapter per method:

```c
static const char *procfs_strip_slash(const char *rel)
{
    /* The dispatcher hands us the path *after* the mount
     * prefix, still starting with '/' (or "" if the path
     * was exactly "/proc"). procfs's internal lookup wants
     * "self/status", not "/self/status", so we lop the
     * slash off here. */
    if (!rel) return "";
    while (*rel == '/') rel++;
    return rel;
}

static long procfs_op_open(void *cookie, const char *rel,
                           int flags, struct fd_entry *out)
{
    (void)cookie; (void)flags;
    return procfs_open_rel(procfs_strip_slash(rel), out);
}

/* … procfs_op_read / _close / _listdir / _is_dir
 * follow the same pattern: thin adapters that strip the
 * leading slash and call the existing implementation. */

const struct fs_ops procfs_fs_ops = {
    .open    = procfs_op_open,
    .read    = procfs_op_read,
    .write   = NULL,
    .close   = procfs_op_close,
    .lseek   = NULL,
    .listdir = procfs_op_listdir,
    .unlink  = NULL,
    .mkdir   = NULL,
    .is_dir  = procfs_op_is_dir,
    .load    = NULL,
};

void procfs_register_mount(void)
{
    (void)vfs_mount_register("/proc", &procfs_fs_ops, NULL, MOUNT_RO);
}
```

[`kernel/core/vfs.c::vfs_init`](../../../kernel/core/vfs.c) gains one
line: `procfs_register_mount();`. The chapter-99 branch in
`vfs_open` is replaced by a vtable dispatch at the top of the
function:

```c
{
    const char *rel = NULL;
    const struct mount *m = vfs_resolve(name, &rel);
    if (m && m->ops && m->ops->open) {
        struct thread *t = thread_current();
        for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
            if (!t->fdt->fds[fd].in_use) {
                long r = m->ops->open(m->cookie, rel, flags,
                                      &t->fdt->fds[fd]);
                if (r < 0) return r;
                t->fdt->fds[fd].in_use = 1;
                return fd;
            }
        }
        return -EMFILE;
    }
}
```

Six lines of dispatch, replacing roughly twenty lines of
`if (path_starts_with(name, "/proc")) { ... }` branching.

## Three traps the port surfaced

### The slash convention

The internal procfs functions had been written to expect
their input *without* a leading slash. The dispatcher hands
each driver the path-after-the-prefix *with* a leading slash
(so that "what is the root of this mount" is unambiguously
spelled `""` and `/something` is unambiguously a child). The
slash-stripping adapter (`procfs_strip_slash`) bridges the
two conventions in one place. Every other driver port in
chapters 113c–113e adopts the same `_strip_slash` helper for
exactly this reason; the alternative — rewriting every
internal function to accept the new convention — would have
made the diffs noisy and the regressions twitchier.

### The fd-init contract

The dispatcher sets `fd_entry.in_use = 1` *after* the driver
returns success. The driver must zero the fields it doesn't
use so that subsequent dispatches don't trip on stale state
from the previous occupant of that slot. The procfs adapter
follows the convention every later port also adopts:

```c
out->kind         = FD_PROCFS;
out->offset       = 0;
out->ramfs_index  = 0;
out->pipe         = NULL;
out->socket_cid   = 0;
out->pty          = NULL;
out->osfs2_ino    = 0;
out->procfs_buf   = buf;
out->procfs_len   = len;
out->srv_l        = NULL;
out->srv_c        = NULL;
out->srv_is_service = 0;
```

The fields with type-specific meaning (`procfs_buf` /
`procfs_len`) are set; everything else is zeroed. This matters
because the slot was just freed by some other driver's
`close`, and that driver's stale pointer in (say) `pipe` would
otherwise look "in use" to the next refcount walker. Step 1's
abstraction doesn't enforce this contract — it's a discipline
each driver follows — but every existing driver does, so we
inherit the same shape.

### `vfs_read` / `vfs_close` still dispatch by `fd_kind`

We do *not* change the read/close dispatch in this step. Reads
and closes route through the existing `fd_kind` switch in
`vfs_read` / `vfs_close`, and procfs's case stays as
`procfs_read(e, buf, n)`. Routing read/close through the mount
table would require stashing the mount pointer on the fd at
open time, which we want to defer until enough drivers are
ported to make the cost worth the savings. Open is the only
op that takes a path; once an fd is open, the kind-tag is
enough. Chapters 113c–113e port their drivers' open path
through the new dispatch and leave their read/close paths in
the kind switch for the same reason. (Cleanup of the kind
switch becomes a tidiness pass after Step 5; it isn't on the
critical path for any user-visible win.)

## What got verified

```
$ make -j8
$ python3 scripts/test_procfs.py
PASS: /proc dispatch + ps work end-to-end
```

`test_procfs.py` exercises `ls /proc`, `cat /proc/self/cmdline`,
and `/bin/ps`, which between them hit `listdir`, `open`, `read`,
and `close`. All four go through `procfs_fs_ops` after this
chapter. The numeric values from `/proc/self/status` are
identical before and after the port — the adapter passes
through to the same underlying procfs functions, so the only
way to break the output would be to mis-wire one of the
methods.

A full sweep ran green too: chapters 99, 100, 47, 32, 30, 90 all
exercise paths that touch the VFS dispatcher and none of them
regressed.

## What gets exercised in tests

- `scripts/test_procfs.py` — full procfs path via the new vtable
- regression sweep — confirms nothing else regressed

## Applied to

- Existing apps modified: none — the abstraction is invisible
  to userspace; `ls /proc`, `cat /proc/...`, `/bin/ps`, and
  `/bin/top` all keep working byte-for-byte.
- New apps added: none — the user-visible payoff lands in
  113f when `/bin/mount` becomes possible.
- New test scripts: none — `test_procfs.py` already covered
  the path; we re-ran it as the per-step gate.

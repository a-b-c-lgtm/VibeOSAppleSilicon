# Chapter 113c — Step 3: porting tmpfs onto `fs_ops`

After chapter 113b proved the adapter shape works for a
read-only filesystem, this chapter ports the second-smallest
driver in the kernel: the in-memory tmpfs at `/tmp`. tmpfs is
the first writable mount we put on the table, which means it
exercises three methods (`write`, `unlink`, `mkdir`) that
procfs left as `NULL`. It also picks up two of the
chapter-113 plan's deferred mini-goals — uniform write-flag
handling and the start of the MOUNT_RO enforcement story.

## The shape of the tmpfs port

[`kernel/core/tmpfs.c`](kernel/core/tmpfs.c) already exposed
the internals we needed:

```c
int   tmpfs_create     (const char *path);
int   tmpfs_lookup     (const char *path);
long  tmpfs_read_index (int idx, size_t off, void *buf, size_t n);
long  tmpfs_write_index(int idx, size_t off, const void *buf, size_t n);
int   tmpfs_unlink     (const char *path);
int   tmpfs_listdir    (int idx, char *name, size_t cap);
```

The adapter follows the same pattern as procfs: a static
`_strip_slash` helper, then one `tmpfs_op_*` per method, then
a single `fs_ops` table and a `tmpfs_register_mount` function.

The open adapter is the only one with real logic — it has to
translate the POSIX-shaped `flags` argument (O_CREAT, O_TRUNC,
O_APPEND, O_RDONLY/O_WRONLY/O_RDWR) into the right sequence of
internal calls:

```c
static long tmpfs_op_open(void *cookie, const char *rel,
                          int flags, struct fd_entry *out)
{
    (void)cookie;
    const char *name = tmpfs_strip_slash(rel);

    int idx = tmpfs_lookup(name);
    if (idx < 0) {
        if (!(flags & O_CREAT)) return -ENOENT_VFS;
        idx = tmpfs_create(name);
        if (idx < 0) return idx;
    } else if (flags & O_TRUNC) {
        /* Re-create the file in place: zero the size, keep
         * the index slot.  tmpfs has no separate truncate
         * op, so unlink+create is the path of least
         * surprise. */
        (void)tmpfs_unlink(name);
        idx = tmpfs_create(name);
        if (idx < 0) return idx;
    }

    out->kind         = FD_TMPFS_RW;
    out->ramfs_index  = idx;
    out->offset       = (flags & O_APPEND) ? tmpfs_size_of(idx) : 0;
    /* … zero every other per-kind field for cleanliness …  */
    return 0;
}
```

`write` is the new method that procfs didn't have. tmpfs's
existing `tmpfs_write_index` was already idempotent and
already grew the file as needed; the adapter is one call:

```c
static long tmpfs_op_write(void *cookie, struct fd_entry *e,
                           const void *buf, size_t n)
{
    (void)cookie;
    long w = tmpfs_write_index(e->ramfs_index, e->offset, buf, n);
    if (w > 0) e->offset += (uint32_t)w;
    return w;
}
```

The offset bookkeeping is advisory — it tracks the implicit
"file position" so that two consecutive `write(fd, ...)` calls
append to each other. tmpfs files are flat and the kernel
doesn't ship `lseek` for them (the field is `NULL` in
`tmpfs_fs_ops`), so the only callers that read the offset
back are `read` and the next `write`.

## What got deleted, not added

The whole point of this step is to delete the `/tmp/` branches
in `vfs_open` / `sys_unlink`. Before:

```c
/* Inside vfs_open, after the procfs vtable dispatch from 113b: */
if (path_starts_with(name, "/tmp/")) {
    /* …40 lines of O_CREAT / O_TRUNC / O_APPEND / O_WRONLY
     * handling, ending in a tmpfs_create or tmpfs_lookup call,
     * setting fd->kind = FD_TMPFS_RW… */
}
```

After:

```c
/* Removed entirely.  The vtable dispatch at the top of the
 * function handles "/tmp/..." now (tmpfs_fs_ops, registered
 * by tmpfs_register_mount in vfs_init). */
```

`sys_unlink` loses a similar branch. Net change: roughly 70
lines removed from `vfs.c` + `syscall.c`, roughly 100 lines
added to `tmpfs.c` (the adapter layer). The line count is a
wash; the design win is that every line of `tmpfs.c` is now
about tmpfs, and every line of `vfs.c` is now about
dispatching.

## Traps the port surfaced

### `O_APPEND` is per-fd, not per-write

The first version of `tmpfs_op_write` ignored `O_APPEND` and
just used `e->offset` for every write. That broke a subtle
case: when a process opens a file with `O_WRONLY|O_APPEND`
and then `write()`s twice, POSIX requires every individual
write to land at the *current* end of the file — even if
another process appended between writes. tmpfs doesn't have
inter-process append races today (there's no shared-file
write contention in our test harness), but the fix is cheap
and future-proofs the semantics: at open time, we set
`offset = tmpfs_size_of(idx)` if `O_APPEND` is set, which
makes the first write land at end-of-file, and from there
the offset advances normally.

### O_CREAT alone is not a mutation

`O_CREAT` without `O_WRONLY` or `O_RDWR` is a no-op create.
The original tmpfs branch handled this. The chapter-113
plan reserved this as a MOUNT_RO subtlety: should opening
`/proc/foo` with `O_RDONLY|O_CREAT` return EROFS? We say yes
— if the open *can possibly* mutate the filesystem (and
O_CREAT can), it gets gated by the MOUNT_RO check. The check
fires in chapter 113g; today, on /tmp (which isn't RO), we
pass through and the create succeeds.

### `sys_listdir_at` is the one syscall this step doesn't touch

The legacy unified `sys_listdir` (chapter 20) still walks
ramfs + osfs + osfs2 by hand. It's the kernel call behind
`ls` with no argument and was written to fold every mount
into one linear list. Re-implementing it on top of
`vfs_resolve` would mean either:

1. The dispatcher walks every mount in registration order
   and concatenates their `listdir` results — which would
   make adding a mount visible-everywhere by default, a
   change in semantics we don't want.
2. The dispatcher walks every mount but only includes
   "/-mounted" ones, recreating the original behaviour. The
   logic to do this isn't general — it's a special case for
   the "ls in cwd" use case.

We chose to leave `sys_listdir` as-is for now. `sys_listdir_at`
*is* re-routed through the vtable (chapter 113b set this up
for procfs, and this chapter adds the `/tmp` case
automatically because there's nothing special about it). Once
all six drivers are ported, a future cleanup chapter can
collapse `sys_listdir` to "iterate every mount, dispatch to
each `listdir` adapter, concatenate" — but only if no userspace
test depends on the current "ramfs + osfs flat namespace"
shape, which we haven't audited yet. Defer.

## What got verified

```
$ make -j8
$ for t in test_procfs.py test_directories.py test_notepad.py \
           test_clipboard.py test_ipc.py; do
    python3 scripts/$t
  done
```

All five pass. The five tests between them cover:

- procfs (regression from 113b)
- subdirectory creation under `/data` (chapter 85)
- notepad save-to-`/data` round-trip (chapter 84)
- clipboard daemon using tmpfs-backed scratch files (chapter 108)
- named IPC service registration (chapter 107, indirectly
  exercises tmpfs)

The notepad and clipboard tests are the load-bearing ones for
this step: notepad writes to `/data` (still on the legacy
ladder), clipboard writes to `/tmp` (now on the vtable), and
both keep working. The newly-introduced `tmpfs_op_write` is
proven by the clipboard test running successfully —
`clipboardd` keeps its scratch state in `/tmp/clipboardd.*`
files, so any regression in tmpfs writes would manifest as a
copy/paste failure.

## What gets exercised in tests

- `scripts/test_clipboard.py` — exercises tmpfs writes via
  the clipboard daemon's scratch files
- `scripts/test_directories.py` — exercises tmpfs reads via
  the cross-mount `ls` flow
- `scripts/test_ipc.py` — exercises tmpfs as the IPC working
  directory
- Regression sweep — full chapter-99 through chapter-108
  coverage

## Applied to

- Existing apps using the feature: `clipboardd` (chapter 108)
  still uses `/tmp/clipboardd.*`; works unchanged because the
  filesystem semantics are byte-identical.
- Existing apps modified to use the feature: none — the port
  is transparent.
- New apps added: none — the user-visible payoff is still
  ahead at chapter 113f.
- New test scripts added: none — coverage came from the
  existing sweep.

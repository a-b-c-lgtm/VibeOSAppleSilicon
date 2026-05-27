# Chapter 136 — Step 4: porting OSFS-1 and OSFS-2

The two on-disk filesystems in the tree are the biggest single
step in the chapter-113 sequence. OSFS-1 (chapter 20) is the
read-only filesystem behind `/mnt` and `/bin`; OSFS-2 (chapter
84) is the writable one behind `/data`. Both are real disk
drivers with their own caches, their own inode tables, and
their own path walkers, and porting them was a single chapter
because the abstractions land best together: doing only one
of the two leaves either `/mnt` or `/data` on the legacy
ladder, which makes the vtable dispatch in `vfs_open` an
"if vtable, else legacy" chain that nobody enjoys reading.

By the end of this chapter every on-disk filesystem dispatch
runs through `vfs_resolve → fs_ops`. The only mount still on
the legacy ladder is the root ramfs — handled in chapter
137.

## OSFS-1: two mounts, one driver

OSFS-1 is the test case for the chapter-113-plan claim that
the same driver instance can register at multiple prefixes.
`/mnt` and `/bin` are both backed by the same on-disk
filesystem; today the kernel hand-aliases them by walking
both prefixes in `vfs_open` (each branch falls through to the
same `osfs_*` calls).

The port replaces this with a single `osfs1_fs_ops` table
registered twice:

```c
void osfs1_register_mount(void)
{
    (void)vfs_mount_register("/mnt", &osfs1_fs_ops, NULL, MOUNT_RO);
    (void)vfs_mount_register("/bin", &osfs1_fs_ops, NULL, MOUNT_RO);
}
```

Both mounts pass `NULL` as the cookie — OSFS-1 has no
per-instance state, only the one global disk cache, so the
two registrations share everything but the prefix. The
dispatcher hands each open the right `rel` (so a request for
`/bin/ls` becomes `rel = "/ls"`, identical to what
`/mnt/ls` would have produced), and OSFS-1's path walker
doesn't know or care which mount point routed it.

The `fs_ops` table:

```c
const struct fs_ops osfs1_fs_ops = {
    .open    = osfs1_op_open,
    .read    = osfs1_op_read,
    .write   = NULL,             /* read-only filesystem */
    .close   = osfs1_op_close,
    .lseek   = NULL,
    .listdir = osfs1_op_listdir,
    .unlink  = NULL,
    .mkdir   = NULL,
    .is_dir  = osfs1_op_is_dir,
    .load    = osfs1_op_load,
};
```

`load` is the one extra method this filesystem implements
that procfs and tmpfs left as `NULL`. It's the exec-path
optimisation: when the kernel runs an ELF off `/bin/ls`, it
wants the whole file as a single kheap-allocated blob rather
than ten 4 KiB reads. OSFS-1 was already structured to hand
that out — chapter 12's exec path already called
`osfs_load(path, &buf, &len)` — so the adapter is one line:

```c
static long osfs1_op_load(void *cookie, const char *rel,
                          uint8_t **out_data, size_t *out_len)
{
    (void)cookie;
    return osfs_load(rel, out_data, out_len);
}
```

The `vfs_load` dispatcher (the exec-path entry point) now
walks the mount table first and calls `m->ops->load` if
present, falling back to the legacy ramfs branch otherwise.
Chapter 137 collapses that fallback when the root ramfs
becomes a mount too.

## OSFS-2: writable, subdirectories, the lot

OSFS-2 is the larger of the two ports because every method
needs an adapter, not just the read-side ones. The full
table:

```c
const struct fs_ops osfs2_fs_ops = {
    .open    = osfs2_op_open,
    .read    = osfs2_op_read,
    .write   = osfs2_op_write,
    .close   = osfs2_op_close,
    .lseek   = NULL,             /* no random-access yet */
    .listdir = osfs2_op_listdir,
    .unlink  = osfs2_op_unlink,
    .mkdir   = osfs2_op_mkdir,
    .is_dir  = osfs2_op_is_dir,
    .load    = osfs2_op_load,
};
```

Two of these methods are interesting enough to call out.

### `osfs2_op_is_dir`: probing via listdir

The internal OSFS-2 `lookup` returns an inode, and the inode
carries the directory bit. The cleanest adapter would have
been:

```c
static int osfs2_op_is_dir(void *cookie, const char *rel)
{
    uint32_t ino = osfs2_lookup(rel);
    if (!ino) return -ENOENT_VFS;
    return osfs2_inode_is_dir(ino);
}
```

`osfs2_inode_is_dir` doesn't exist as a public API. Adding
one means widening OSFS-2's surface for one caller — a smell.
Instead we use the listdir probe trick, which the existing
chapter-85 code already used internally:

```c
static int osfs2_op_is_dir(void *cookie, const char *rel)
{
    (void)cookie;
    char name[OSFS2_NAME_MAX];
    uint32_t type;
    /* If listdir_at(path, 0, ...) succeeds with idx=0, the
     * path is a directory (with at least zero entries — every
     * dir has "." and ".." even if otherwise empty).  If it
     * returns ENOENT, either the path is missing or it's a
     * regular file.  Either way: not a dir. */
    long r = osfs2_listdir_at(rel, 0, name, sizeof name, NULL, &type);
    return r >= 0 ? 1 : 0;
}
```

Half a side-effect-free call instead of a new accessor.
Future-us extending OSFS-2 can replace this if a cleaner
public API arrives; the contract — `1` for dir, `0` for not,
negative on error — is identical either way.

### `osfs2_op_listdir`: root vs. named subdir

OSFS-2 has real subdirectories (chapter 86), so the adapter
has to handle both "list the root of the mount" and "list
this named child":

```c
static int osfs2_op_listdir(void *cookie, const char *rel,
                            int idx, char *name, size_t cap,
                            uint32_t *type)
{
    (void)cookie;
    /* "/data" and "/data/" both resolve to rel == "" or "/".
     * The OSFS-2 root inode is well-known. */
    if (rel[0] == 0 || (rel[0] == '/' && rel[1] == 0)) {
        return osfs2_listdir_inode(OSFS2_INODE_ROOT,
                                   idx, name, cap, type);
    }

    /* "/data/notes" → look up "notes" relative to root,
     * then enumerate that inode. */
    uint32_t ino = osfs2_lookup(rel);
    if (!ino) return -ENOENT_VFS;
    return osfs2_listdir_inode(ino, idx, name, cap, type);
}
```

This is the only adapter in the chapter-113 series that does
non-trivial path interpretation. The reason it stays here
rather than moving into OSFS-2's own internals: the rest of
OSFS-2 takes leaf names against an inode, which is the
natural shape for a directory-aware filesystem. The "translate
a path into an inode then list it" sequence is the *VFS-side*
thing, not the *filesystem-side* thing. Putting it in the
adapter keeps the OSFS-2 module's public API symmetric.

## What got deleted

The deletions in this step are bigger than any previous step.
`vfs.c` and `syscall.c` shed:

- The `path_starts_with(name, "/mnt/")` and
  `path_starts_with(name, "/bin/")` branches in `vfs_open`
  and `vfs_open_into` — replaced by the vtable dispatch.
- The `/data/` branches in `vfs_open` and `vfs_open_into` —
  ditto.
- The `/data/` and `/tmp/` legacy ladders in `sys_unlink`
  (the chapter-135 port already cut the `/tmp/` one for
  the unlink syscall; the `/data/` one was the last surviving
  one in that function) — now `sys_unlink` is "vtable dispatch
  or -EINVAL_VFS", and looks it.
- The `/data/` mkdir branch in `sys_mkdir` — sys_mkdir now
  follows the same one-liner shape: vtable dispatch or
  -EINVAL_VFS fall-through.
- The `/data/` branch in `sys_listdir_at` (the `/proc` and
  `/tmp` branches went earlier; this is the last one) —
  `sys_listdir_at` becomes a single vtable dispatch with no
  legacy fallback.
- The `/mnt/` branch in `vfs_load` (the exec path) — replaced
  by `m->ops->load` dispatch.

Net: roughly 200 lines deleted from the dispatcher core, and
the per-syscall logic in `sys_unlink`, `sys_mkdir`, and
`sys_listdir_at` is now four lines each.

## Traps

### Adding `#include "vfs.h"` to `osfs.c`

OSFS-1's old internal API used the kernel's *legacy* errno
constants (`ENOENT`, `EINVAL`). The new adapters return
the VFS-namespace ones (`ENOENT_VFS`, `EROFS_VFS`,
`ENOSYS_VFS`) so that the dispatcher and userspace get the
same numbers. That meant `osfs.c` needed to `#include "vfs.h"`
where it had previously got by without — the original file
only needed the OSFS-internal types. One-line addition, but
worth noting because chapter 137 does the same trick for
`vfs.c`'s ramfs adapter (the new adapter is in `vfs.c`
itself, but it needed the public errno names rather than the
legacy internal ones).

### `struct fd_entry` field zeroing

Every adapter zeros all `fd_entry` fields it doesn't use,
exactly as procfs did in 134. The OSFS-2 open path is the
most invasive — it has to clear `kind`, `offset`,
`ramfs_index`, `osfs_start`, `osfs_size`, `pipe`,
`socket_cid`, `pty`, `procfs_buf`, `procfs_len`, `srv_l`,
`srv_c`, `srv_is_service` before setting `kind = FD_OSFS2_FILE`
and `osfs2_ino = ino`. Missing one would leave a stale pointer
from the slot's previous occupant and the next refcount
walker would dereference garbage. The freestanding-C lesson
behind this is the same one chapter 27 first hit: an explicit
per-field clear is the cheapest way to zero a struct in
freestanding C without dragging in a synthetic `memset` from
the compiler's optimiser.

### The "OSFS-1 has no cookie" decision

OSFS-1 has exactly one disk image (the one mounted on /dev/vda
at boot). Both `/mnt` and `/bin` mount points refer to the
same on-disk state. We pass `NULL` as the cookie for both
registrations because there is no per-mount state to
distinguish them — the adapter's behaviour is identical
regardless of which prefix routed the call.

The day OSFS-1 grows a second disk (chapter ~150, maybe, if
we ever support mounting a removable image), the cookie
becomes the natural place to stash the per-instance handle.
Today's `NULL` doesn't preclude that future; it just records
that we don't need it yet.

## What got verified

```
$ make -j8
$ for t in test_procfs.py test_directories.py test_notepad.py \
           test_clipboard.py test_ipc.py test_fsync.py test_clock.py; do
    python3 scripts/$t
  done
```

Seven tests, all green. The load-bearing ones:

- `test_directories.py` — exercises OSFS-2's mkdir, listdir,
  is_dir, unlink, plus persistence across reboot. Every adapter
  method runs in this one test.
- `test_notepad.py` — writes via the new vtable, reads back
  after reboot.
- `test_fsync.py` — exercises the OSFS-2 write path's
  durability contract from chapter 83.
- `test_clock.py` — reads `/bin/date` (which is loaded via
  the new OSFS-1 `load` adapter) and validates RTC output.

If any of the four adapters' field-zeroing was wrong, or the
RO check (added in 139) was firing inappropriately, or the
listdir probe trick for `is_dir` was buggy, one of these tests
would fail. They all pass.

## What gets exercised in tests

- `scripts/test_directories.py` — mkdir, listdir, unlink,
  is_dir for OSFS-2
- `scripts/test_notepad.py` — write + read for OSFS-2,
  persistence across reboot
- `scripts/test_clock.py` — `load` adapter for OSFS-1
  (`/bin/date`)
- Regression sweep — full chapter 20 through chapter 101
  coverage

## Applied to

- Existing apps using the feature: `notepad`, every binary
  that lives in `/bin`, `cat`, `ls`, `httpd`, `fontd`,
  `clipboardd`, `wsd`, `desktop`, `taskbar`, `launcher`,
  `clock`, `gui_term`, `browser` — every userspace process
  that opens a file under `/mnt`, `/bin`, or `/data` is now
  flowing through the new dispatch.
- Existing apps modified to use the feature: none — the port
  is transparent.
- New apps added: none — the visible payoff is still in
  chapter 138.
- New test scripts: none — coverage came from the existing
  sweep.

# Chapter 132 — A real VFS: mount table and `struct fs_ops`

> **Milestone in this chapter:** 113 — design contract for the
> mount-table refactor. The implementation lands across the
> seven follow-up chapters 133–139, each one sweep-green
> before the next began.
> **Code referenced (where the contract lands):**
> - [kernel/core/vfs.c](../../../kernel/core/vfs.c)
>   (`vfs_resolve`, the mount table, `struct fs_ops`)
> - [kernel/core/vfs.h](../../../kernel/core/vfs.h)
>
> **At the end of this chapter** you will have a written contract
> for the dispatch shape the rest of Part XVI uses: a mount table
> of `(prefix, fs_type, fs_ops *, void *cookie)` rows, a
> `vfs_resolve(path) → (mount, relpath)` helper, and a `struct
> fs_ops` vtable whose entries match the chapter-16 syscalls. **No
> code lands in this chapter** — chapter 133 is the first
> implementation step.

## Why this chapter exists

After chapter 101 (`/proc`) shipped, the VFS dispatch in
[kernel/core/vfs.c](../../../kernel/core/vfs.c) and
[kernel/core/syscall.c](../../../kernel/core/syscall.c) had grown
into five separate prefix-special-cased ladders for five
filesystems:

| prefix     | filesystem driver                       | added in   |
| ---------- | --------------------------------------- | ---------- |
| (root)     | embedded ramfs (`/motd`, `/README`)     | chapter 15 |
| `/mnt/`    | OSFS-1 (read-only on-disk)              | chapter 20 |
| `/bin/`    | OSFS-1 (binaries, same mount)           | chapter 21 |
| `/tmp/`    | tmpfs (in-memory, writable)             | chapter 40 |
| `/data/`   | OSFS-2 (writable on-disk)               | chapter 85 |
| `/proc/`   | procfs (synthetic, read-only)           | chapter 101 |

Each of these requires:

- A branch in `vfs_open`
- A branch in `vfs_read`
- A branch in `vfs_close`
- A branch in `vfs_load` (the exec path)
- A branch in `sys_listdir_at`
- A branch in `sys_unlink`
- A branch in `sys_mkdir_at`
- An entry on every "list of writable prefixes" lookup in the
  shell, `notepad`, `cat`, and friends.

That is seven-plus branches in the kernel **and** scattered
userspace knowledge per filesystem. Six filesystems live in the
tree today; the next one (audio? sysfs? netfs? a user-provided FS
via chapter 140?) is the one that crosses the line from "annoying"
to "going to introduce a bug."

Five places where the current dispatch already drifts inconsistently:

- `vfs_open` accepts both `/proc` and `/proc/` (chapter 101
  retrofit) but `sys_listdir_at` accepts only `/proc[/...]` (the
  post-fix code).
- `/data/` supports `mkdir`; `/tmp/` doesn't. Both are writable.
  The reason is "nobody bothered."
- `userspace/ls/ls.c` carries its own copy of the prefix list to
  decide between `listdir` and `listdir_at`. Add a filesystem to
  the kernel; remember to update `ls` too.
- `userspace/libgui/save_dialog.c` hard-codes the `/data/` prefix
  as the only place save dialogs can write.

This is the standard problem the Unix VFS abstraction was invented
to solve. Time to adopt the standard solution.

## What this chapter proposes

A classical Unix-shaped VFS, with two new core types:

```c
/* Operations a filesystem driver must implement.  Optional
 * methods are NULL; the dispatcher returns -ENOSYS_VFS when
 * a caller asks for an unimplemented operation. */
struct fs_ops {
    long (*open)    (void *cookie, const char *rel, int flags,
                     struct fd_entry *out);
    long (*read)    (void *cookie, struct fd_entry *e,
                     void *buf, size_t n);
    long (*write)   (void *cookie, struct fd_entry *e,
                     const void *buf, size_t n);
    long (*close)   (void *cookie, struct fd_entry *e);
    long (*lseek)   (void *cookie, struct fd_entry *e,
                     int64_t off, int whence);
    int  (*listdir) (void *cookie, const char *rel, int idx,
                     char *name, size_t cap, uint32_t *type);
    int  (*unlink)  (void *cookie, const char *rel);
    int  (*mkdir)   (void *cookie, const char *rel);
    int  (*is_dir)  (void *cookie, const char *rel);
    long (*load)    (void *cookie, const char *rel,
                     uint8_t **out_data, size_t *out_len);
};

/* One mount point. */
struct mount {
    const char          *prefix;   /* "/proc", "/data", "/tmp", "/bin", "/mnt", "/" */
    const struct fs_ops *ops;
    void                *cookie;   /* driver-private (e.g. tmpfs root, osfs2 handle) */
    uint32_t             flags;    /* MOUNT_RO, MOUNT_NODEV, etc */
};

static struct mount g_mounts[MOUNT_MAX];   /* MOUNT_MAX = 16 to start */
static int          g_mounts_n;
```

And one resolver:

```c
/* Longest-prefix-match.  Returns NULL if no mount covers
 * the path (which should never happen once "/" is mounted). */
const struct mount *vfs_resolve(const char *path, const char **rel_out);
```

Every syscall in the file then collapses to:

```c
long sys_open(const char *path, int flags) {
    const char *rel;
    const struct mount *m = vfs_resolve(path, &rel);
    if (!m)        return -ENOENT_VFS;
    if (!m->ops->open) return -ENOSYS_VFS;
    /* allocate fd slot, then... */
    return m->ops->open(m->cookie, rel, flags, slot);
}
```

That's the entire abstraction. Every existing prefix-ladder
in `vfs_open`, `vfs_read`, `vfs_close`, `vfs_load`,
`sys_listdir_at`, `sys_unlink`, `sys_mkdir_at` becomes a
single `vfs_resolve` call followed by one indirect dispatch.

## Migration: how the six existing filesystems land

Each becomes a `struct fs_ops` instance with the same
behaviour as today; only the dispatch shape changes. No
on-disk format changes, no userspace ABI changes, no test
breakage expected.

### `/` — embedded ramfs

Two files today (`/motd`, `/README`). The driver becomes a
tiny `struct fs_ops` whose `open` walks the static
`g_ramfs[]` array. `cookie` is just `&g_ramfs[0]` plus a
count. `listdir` enumerates the static array. Everything
else returns `-ENOSYS_VFS`.

Open question: should `/motd` actually live at `/` (root
ramfs), or should we mount the ramfs at `/etc/motd`-shaped
paths? Defer; the migration preserves current paths.

### `/mnt/` and `/bin/` — OSFS-1

Both mount points are backed by the same on-disk OSFS-1
filesystem. Two paths to handle this:

1. **Two `struct mount` entries sharing one `struct
   fs_ops` and one `cookie`.** Cleanest. The `prefix`
   differs but the driver instance is identical. Adding a
   third synonym would be one line.
2. **One mount + a kernel-internal symlink.** More work
   and reintroduces special-casing.

We pick option 1. The mount table accepts duplicate
cookies happily; the dispatcher only cares about the
prefix-to-cookie binding.

### `/tmp/` — tmpfs

The current tmpfs (in
[kernel/core/tmpfs.c](../../../kernel/core/tmpfs.c)) already has a
near-vtable shape: it exposes `tmpfs_open`,
`tmpfs_read`, `tmpfs_write`, `tmpfs_close`, etc. The
migration is mechanical: wrap each in a `static long
tmpfs_op_open(void *cookie, ...)` adapter and populate a
`struct fs_ops`.

Bonus during this work: enable `mkdir` so `/tmp/sub/` is
allowed. The kernel-side change is trivial once tmpfs has
a directory-aware backing; the reason it's missing today
is "no one demanded it."

### `/data/` — OSFS-2

Same shape as tmpfs. The OSFS-2 driver already has
subdirectory support (chapter 86), so the `fs_ops` instance
will populate every method including `mkdir` and
`listdir`. The retro of `osfs2_*` to take a leading
`void *cookie` argument is the largest diff in the file.

### `/proc/` — procfs

The driver added in chapter 101 fits the vtable trivially.
`open` becomes `procfs_op_open` which calls `procfs_render`
into a fresh kheap buffer (today's behaviour). `read`
slices, `close` `kfree`s. `listdir` is already present.
`write`/`unlink`/`mkdir` return `-ENOSYS_VFS`.

The `cookie` is `NULL` (procfs has no per-mount state).

## Migration: userspace cleanup

This is the half of the work the user explicitly called out
("Section 15 should handle these two items and updating all
of the existing applications to use the new systems").

### `userspace/ls/ls.c`

Today: a hand-maintained list of prefixes that need
`listdir_at` instead of the flat `listdir`. After the
refactor we collapse to one syscall: `listdir_at` works on
every mount because the kernel resolves to a mount and
dispatches; the flat `listdir` syscall becomes
`listdir_at("/")` for backward compat.

Diff: ~20 lines deleted, no new code.

### `userspace/libgui/save_dialog.c`

Today: hard-codes `/data/` as the writable destination.
After the refactor we expose the mount table to userspace
via a new `SYS_MOUNTS` syscall (returns
`{prefix, flags}` pairs); the save dialog iterates the
table and offers any mount with `!MOUNT_RO` as a save
destination. Currently that's `/data/` and `/tmp/`. Future:
also any user-mounted fs (chapter 140).

### `userspace/sh/sh.c`

Today: builtins like `cd` accept any path; `mkdir` shells
out to a hypothetical `/bin/mkdir`. After the refactor:
unchanged — the kernel's `sys_mkdir_at` now works against
any mount that advertises a `mkdir` method, so `/tmp/sub/`
suddenly works without `sh` knowing.

### `userspace/notepad/notepad.c`

Today: lets you save to anywhere you can spell a path,
relies on `vfs_open` to reject `/mnt/` (read-only). After
the refactor: unchanged — the read-only mount flag is
enforced in the kernel and produces `-EROFS_VFS`.

### Everyone else

`cat`, `head`, `tail`, `wc`, `grep`, `ps`, `top`,
`httpget`, `browser`: no diff. They already speak through
the syscall interface; the kernel-side refactor is
invisible to them.

## New syscalls and changed errnos

Two small additions:

```c
#define SYS_MOUNTS   N  /* enumerate the mount table */
#define SYS_MOUNT    N  /* PLAN, used by chapter 140 */
#define SYS_UMOUNT   N  /* PLAN, used by chapter 140 */
```

`SYS_MOUNT`/`SYS_UMOUNT` are reserved here but defined in
[Chapter 140](140-userspace-filesystem-servers.md). For the
mount-table refactor in isolation, only `SYS_MOUNTS` is
strictly needed (and only for the save dialog rework).

Errnos:

- `EROFS_VFS` — new, returned when a write hits a
  `MOUNT_RO` mount. Today the OSFS-1 code returns
  `-EINVAL_VFS` for the same case; we tighten this.
- `ENOSYS_VFS` — new, returned when a driver doesn't
  implement the requested op. Today this is silent or
  manifests as `-EINVAL_VFS`.

## What this does NOT change

- On-disk formats: OSFS-1 and OSFS-2 stay byte-identical.
- Path syntax: `/proc/`, `/tmp/...`, `/data/...` all
  unchanged.
- Userspace ABI for any tool other than the save dialog.
- The fd_table refcounting in chapter 94.
- The fd-kind enum: the existing `FD_FILE`, `FD_TMPFS_RW`,
  `FD_PROCFS`, `FD_OSFS2_FILE` stay. Drivers continue to
  set the kind they want on their fds; the kind tells
  `vfs_close` which `fs_ops` to dispatch to via the mount
  pointer we stash on the fd at open time.

## Implementation sequence

Each step keeps the sweep green. The sweep is the safety
net: do not start the next step until the previous one is
55/55.

1. **Land the types and resolver.** Add `struct fs_ops`,
   `struct mount`, `vfs_resolve` to `kernel/core/vfs.h`/`.c`.
   No callers yet. Pure type addition.

2. **Port procfs first** (smallest driver, just shipped).
   Write `procfs_ops`. Register a `g_mounts[]` entry for
   `/proc`. Change `vfs_open`'s `/proc` branch to use the
   resolver. Sweep.

3. **Port tmpfs.** Same shape; flips two more branches.
   Sweep.

4. **Port OSFS-1 and OSFS-2.** Larger drivers but no
   format changes. Sweep.

5. **Port the embedded ramfs as the root mount.** This
   removes the last fallback. After this step the prefix
   ladders are gone and every syscall is one
   `vfs_resolve` + one dispatch.

6. **Add `SYS_MOUNTS` + rework `save_dialog.c`.** Sweep.

7. **Add `MOUNT_RO` enforcement + `EROFS_VFS`.** Sweep.

Each numbered step is one PR-sized commit and one
book-chapter-sized milestone. Total budget: ~5–7 days of
focused work; the bulk is porting tmpfs and OSFS-2.

## Lessons recorded for the implementation

(Future-us reading the actual implementation chapter will
appreciate these being written down now.)

- **The `cookie` argument matters.** It's the difference
  between "the tmpfs driver" and "an instance of the
  tmpfs driver." Today there is only one tmpfs, so it
  feels redundant — but Chapter 140's per-userspace-fs
  state lives here, and we want the same vtable to work
  for both kernel and userspace filesystems.
- **Longest-prefix match, not first-match.** With six
  mounts the order is unambiguous, but as soon as `/`
  is mounted alongside `/proc`, first-match by
  registration order would route everything to `/`.
- **The mount table is global and lock-light.** Mounts
  rarely change (boot once + maybe `mount` from
  userspace). A reader-writer lock or even
  `irq_save_disable` for the table itself is fine; we
  don't need per-mount locking unless filesystems share
  cookies, which they only do in the OSFS-1 case where
  the driver is already responsible for its own locks.
- **Don't unify fd_kinds.** It's tempting to drop the
  enum (everyone goes through the vtable). But the kind
  is still useful for the "is this a TTY?" / "is this a
  socket?" checks that aren't filesystem operations.
  Leave the enum alone.

## When to schedule

After chapter 101 ships (done) and before any new
filesystem (audio, sysfs, netfs, or the userspace-fs work
in Chapter 140). The refactor is a prerequisite for
Chapter 140 — adding userspace filesystems on top of the
current prefix ladder would multiply the existing ladder
problem by every userspace fs we ever mount.

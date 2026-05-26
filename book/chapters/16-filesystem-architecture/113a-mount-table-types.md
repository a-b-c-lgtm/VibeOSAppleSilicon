# Chapter 113a — Step 1: the mount table and `vfs_resolve`

[Chapter 113](113-mount-table-and-vtable.md) laid out the plan:
collapse six prefix-special-cased ladders in
[`kernel/core/vfs.c`](../../../kernel/core/vfs.c) and
[`kernel/core/syscall.c`](../../../kernel/core/syscall.c) into one
table-driven dispatcher. The plan called for a strict
seven-step landing — each step ending with a green regression
sweep — so that no single commit broke userspace. This is the
first of those steps. It adds the types, the resolver, and an
empty mount table; nothing actually dispatches through the new
machinery yet. The kernel's behaviour is byte-identical to the
pre-113 build. We just have the wiring in place to start
pulling drivers across in chapters 113b through 113e.

## Why land an empty table first

The temptation when refactoring a dispatch layer is to land
the new shape and the first driver in one commit, "to prove it
works." We resist it. The rules of the migration are:

1. Each step keeps the sweep green.
2. Each step lands one mechanical thing.
3. Steps are reversible.

A single commit that adds the table AND ports procfs is
neither mechanical nor reversible — if the table design turns
out to be wrong, you have to undo two changes at once and
re-derive whatever the procfs port taught you about the
shape. By contrast, an empty table is an unused header. If we
hate it tomorrow, we delete one struct and one function. No
caller broke because no caller called.

The two artefacts this step lands are also the two things every
later step depends on, so getting them reviewed and merged
first lets the subsequent driver ports proceed in parallel —
or, more honestly for a one-person project, lets each one
focus only on its own driver shape without also designing the
abstraction it lives in.

## The types

The full surface is small enough to fit on one screen. From
[`kernel/core/vfs.h`](../../../kernel/core/vfs.h):

```c
#define MOUNT_MAX   16
#define MOUNT_RO    0x1u   /* writes return -EROFS_VFS */

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

struct mount {
    const char          *prefix;
    const struct fs_ops *ops;
    void                *cookie;
    uint32_t             flags;
};

int                  vfs_mount_register(const char *prefix,
                                        const struct fs_ops *ops,
                                        void *cookie, uint32_t flags);
const struct mount  *vfs_resolve       (const char *path,
                                        const char **rel_out);
int                  vfs_mount_count   (void);
const struct mount  *vfs_mount_at      (int idx);
```

Five things are worth pointing out:

- **`cookie` exists from day one even though every kernel
  filesystem will pass `NULL`.** It is the difference between
  "the tmpfs driver" and "an instance of the tmpfs driver."
  Today there is only one tmpfs instance, but Chapter 114's
  userspace filesystem servers need to distinguish multiple
  mounts with the same `struct fs_ops` (one per running
  daemon), and retro-fitting `cookie` later would touch every
  adapter. Free now; expensive later.

- **The methods take `rel` (the path *after* the mount
  prefix), not the full path.** This is what makes drivers
  reusable across mount points without each one parsing its
  own prefix back off. OSFS-1 in chapter 113d will register
  itself at both `/mnt` and `/bin` with the same `fs_ops` and
  the same `cookie` (`NULL`); the dispatcher hands each open
  the right `rel` and the driver doesn't notice that two
  mount points exist.

- **`flags` is a u32 even though we only define one bit
  today.** `MOUNT_RO` is bit 0. Bits 1..31 are room for the
  obvious next entries (`MOUNT_NODEV`, `MOUNT_NOEXEC`,
  `MOUNT_NOSUID`, the BSD `MNT_DONTBROWSE` shape) without
  needing an ABI break.

- **`MOUNT_MAX = 16`.** Today we have six mounts. The cap is
  chosen to keep the table inline (no kmalloc, no per-mount
  refcounting) while leaving headroom for chapter 114 to
  mount a handful of userspace servers without a recompile.

- **No locking on the table.** Mounts are added at boot only
  in this chapter. When chapter 114 introduces `SYS_MOUNT` we
  will revisit; for now the table is read-mostly and the
  reader-writer race window is empty.

## The resolver

Longest-prefix-match, with one wrinkle for the root mount. From
[`kernel/core/vfs.c`](../../../kernel/core/vfs.c):

```c
const struct mount *vfs_resolve(const char *path,
                                const char **rel_out)
{
    if (!path || path[0] != '/') return NULL;

    const struct mount *best = NULL;
    size_t              best_n = 0;

    for (int i = 0; i < g_mounts_n; i++) {
        const struct mount *m = &g_mounts[i];
        if (!m->prefix) continue;
        size_t n = s_len(m->prefix);

        /* The single-character "/" mount matches anything but
         * loses every tie — it is the catchall. */
        if (n == 1 && m->prefix[0] == '/') {
            if (!best) { best = m; best_n = 1; }
            continue;
        }

        if (!path_starts_with(path, m->prefix)) continue;
        if (path[n] != 0 && path[n] != '/') continue;   /* "/bin" must not match "/binary" */
        if (n > best_n) { best = m; best_n = n; }
    }

    if (best && rel_out) {
        if (best_n == 1) {
            *rel_out = path;            /* root: whole path */
        } else {
            *rel_out = path + best_n;   /* everything after the prefix, still leading '/' */
        }
    }
    return best;
}
```

The root-mount carve-out is the part of the function that
took the longest to settle. Three competing constraints:

1. The root mount has to match `/anything-the-other-mounts-
   didn't-cover`.
2. It must never win against a more-specific mount. A path of
   `/proc/meminfo` must go to the procfs mount, not the root
   one — even though `"/"` is a prefix of `"/proc/meminfo"`.
3. The `rel` it hands to the ramfs driver must be `/motd`,
   not `motd` and not the empty string, because the existing
   ramfs lookup function (`ramfs_lookup`) stores its names
   with a leading slash and expects them that way.

The "loses every tie" rule does the first two. The
`*rel_out = path` branch does the third. Without that third
branch the ramfs adapter would need to add a leading slash
back on, which is the kind of asymmetry that bites you in
chapter 114 when a userspace fs sends the same path back over
the wire and now has to know which mount point trimmed what.

The `path[n] != 0 && path[n] != '/'` guard is the one we'd
have got wrong if we had only tested the existing six prefixes.
None of them share a prefix with another (`/proc`, `/data`,
`/tmp`, `/mnt`, `/bin` are pairwise prefix-disjoint), so a
naïve `path_starts_with` would have passed every regression
test. The guard exists so the day someone mounts `/binary` at
some new prefix, the existing `/bin` mount doesn't claim
`/binary/...`. Future-proofing in a single line.

## Mount registration

`vfs_mount_register` is the boring half:

```c
int vfs_mount_register(const char *prefix, const struct fs_ops *ops,
                       void *cookie, uint32_t flags)
{
    if (!prefix || prefix[0] != '/') return -EINVAL_VFS;
    if (!ops)                        return -EINVAL_VFS;
    if (g_mounts_n >= MOUNT_MAX)     return -ENOSPC;
    g_mounts[g_mounts_n].prefix = prefix;
    g_mounts[g_mounts_n].ops    = ops;
    g_mounts[g_mounts_n].cookie = cookie;
    g_mounts[g_mounts_n].flags  = flags;
    g_mounts_n++;
    return 0;
}
```

Three notes:

- **No de-duplication.** Two mounts at the same prefix are
  allowed. The longest-prefix-match logic picks the one with
  the longer prefix; on a literal tie, the later registration
  wins (because we iterate forwards and use strict `n > best_n`,
  so a later entry with the same length doesn't beat the
  earlier one — but we'd never register two entries at the
  exact same prefix in practice). Userspace `mount` in
  chapter 114 will reject duplicates at the syscall layer.

- **Prefix strings are borrowed, not copied.** Every caller
  passes a string literal (e.g. `"/data"`); the table holds
  the pointer. The strings live in `.rodata` for the kernel's
  lifetime, so this is safe. If chapter 114 ever wants to
  mount with a userspace-provided prefix string, the syscall
  glue will own its own copy on the kheap.

- **Errors are unrecoverable at boot.** Every kernel-internal
  caller in `vfs_init()` cast-discards the return value with
  `(void)`. The mount table at boot is a hard-coded six-entry
  shape; if it fails to fit in `MOUNT_MAX`, the bug is in our
  constants, not in the call site.

## What got verified

`make -j8` builds clean. The new types compile with no warnings
under `-Wall -Wextra -Werror`. The sweep is unchanged — every
existing regression script still passes because nothing calls
`vfs_resolve` yet. The sweep being green here is a non-event:
"we didn't break anything" is the bar, and we cleared it.

The sentinel that this step worked is that `vfs.h` now exports
a working abstraction. Chapter 113b will be the first chapter
that proves the abstraction *fits a real driver*.

## What gets exercised in tests

- No new tests in this chapter (the table is empty).
- Existing regression sweep stays green, confirming the
  header additions don't trip anything.
- The `MOUNT_MAX`, `MOUNT_RO`, `EROFS_VFS` and `ENOSYS_VFS`
  defines compile-test the freestanding build with `-Werror`.

## Applied to

- Existing apps modified: none yet (this is a kernel-internal
  type addition).
- New apps added: none yet (each driver port lands the
  user-visible payoff in its own chapter).
- New test scripts added: none yet — coverage lands with each
  driver port in 113b–113e.

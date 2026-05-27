# Chapter 137 — Step 5: the embedded ramfs as a root mount

The mount table at the end of chapter 136 has five entries:
`/proc`, `/tmp`, `/mnt`, `/bin`, `/data`. Five mounts cover
every userspace-visible file in the kernel — except the two
that have been baked into the kernel image since chapter 15:
`/motd` and `/README`. Those live in the embedded ramfs,
which until this chapter was reached via a final fall-through
branch in `vfs_open`:

```c
/* All the vtable + legacy branches above didn't match.
 * Last resort: try the embedded ramfs. */
return ramfs_lookup_and_open(name, flags, out);
```

This chapter ports the ramfs onto `fs_ops` and registers it
at `"/"` as the root mount — the catchall. After this chapter
lands, *every* path goes through `vfs_resolve` and there is
no fall-through. The dispatcher in `vfs_open` becomes
purely table-driven, and the file ends with a single
`return -ENOENT_VFS;` instead of the historical legacy ladder.

## Why `"/"` as a mount, not as a fallback

Two designs were on the table:

1. **Special-case the root: if no mount matches, route to
   ramfs.** Keeps ramfs implicit. One line of code.
2. **Make the root ramfs a real mount at `"/"` with the
   same `MOUNT_RO` flag every other RO mount uses.**
   Five lines of code, plus the resolver special case
   from chapter 133.

Design 2 wins for three reasons:

- **Uniformity.** Every other filesystem in the kernel goes
  through `fs_ops`. The root ramfs being the one exception
  was the kind of asymmetry that breeds future bugs. If a
  later chapter (or chapter 140) adds a feature that walks
  `g_mounts[]`, the root mount appears in that walk
  automatically.
- **`SYS_MOUNTS` enumeration.** Chapter 138 exposes the
  mount table to userspace. A user running `/bin/mount` will
  expect to see `/` listed, not "five mounts and an implicit
  root." Listing only what the table contains is the simplest
  semantics; making the root explicit is the way to get there.
- **`MOUNT_RO` enforcement.** Chapter 139 tightens the
  write-flag check. The root ramfs is read-only; it deserves
  to participate in the same EROFS_VFS contract every other
  RO mount obeys. The alternative — a custom branch that
  returns `-ENOENT_VFS` for writes against root paths — is
  one more place to drift inconsistently from the standard.

## The ramfs adapter

The internal ramfs functions take leaf-style paths
(`/motd`, `/README`) and have done since chapter 15. The
adapter is correspondingly small:

```c
static long ramfs_op_open(void *cookie, const char *rel,
                          int flags, struct fd_entry *out)
{
    (void)cookie; (void)flags;
    int idx = ramfs_lookup(rel);
    if (idx < 0) return -ENOENT_VFS;
    out->kind         = FD_FILE;
    out->offset       = 0;
    out->ramfs_index  = idx;
    out->osfs_start   = 0;
    out->osfs_size    = 0;
    out->pipe         = NULL;
    out->socket_cid   = 0;
    out->pty          = NULL;
    out->osfs2_ino    = 0;
    out->procfs_buf   = NULL;
    out->procfs_len   = 0;
    out->srv_l        = NULL;
    out->srv_c        = NULL;
    out->srv_is_service = 0;
    return 0;
}

static const struct fs_ops g_ramfs_root_ops = {
    .open    = ramfs_op_open,
    .read    = ramfs_op_read,
    .write   = NULL,
    .close   = ramfs_op_close,
    .lseek   = NULL,
    .listdir = ramfs_op_listdir,
    .unlink  = NULL,
    .mkdir   = NULL,
    .is_dir  = ramfs_op_is_dir,
    .load    = ramfs_op_load,
};

static void ramfs_register_root_mount(void)
{
    (void)vfs_mount_register("/", &g_ramfs_root_ops, NULL, MOUNT_RO);
}
```

The interesting choice is `rel` passing. The chapter 133
resolver's root carve-out hands the ramfs adapter the *full*
path, leading slash and all (so `/motd` stays `/motd`).
That's exactly what `ramfs_lookup` was already written to
expect — the historical fall-through called it with the
full path, never with a stripped one. No adapter-side
translation needed; the ramfs adapter is the one adapter in
the whole chapter that has no `_strip_slash` helper.

## `ramfs_op_listdir` strips the leading slash

The one place ramfs *does* need a translation is in `listdir`.
Every other filesystem's adapter returns bare leaf names
(`status`, `cmdline`), but the ramfs entries are stored as
`/motd` and `/README` — the slash is part of the canonical
name. To keep the chapter-113 listdir contract uniform
("return leaf names, no leading slash"), the adapter strips
it:

```c
static int ramfs_op_listdir(void *cookie, const char *rel,
                            int idx, char *name, size_t cap,
                            uint32_t *type)
{
    (void)cookie; (void)rel;
    /* The root mount has only the top-level files (no
     * subdirectories), so any rel value points back at the
     * same flat list. */
    const char *raw = ramfs_name_at(idx);
    if (!raw) return -ENOENT_VFS;
    if (*raw == '/') raw++;
    size_t n = s_len(raw);
    if (n >= cap) return -EINVAL_VFS;
    for (size_t i = 0; i < n; i++) name[i] = raw[i];
    name[n] = '\0';
    if (type) *type = 0;   /* regular files only */
    return (int)n;
}
```

The translation could equally well live in `ramfs_name_at`
itself, but that function is called from other places (the
chapter 15 boot-time directory print) which would suddenly
see different output. Keeping the translation in the adapter
preserves the legacy callers and isolates the vtable
convention to one place.

## What got deleted, finally

`vfs_open` and `vfs_open_into` lose their final fall-through.
Before chapter 137:

```c
/* (vtable dispatch above) */
/* (legacy /tmp /data /proc /mnt /bin branches, mostly empty
 *  now that 134–136 ate them) */
/* Last resort: */
return ramfs_lookup_and_open(name, flags, out);
```

After:

```c
/* (vtable dispatch above) */
/* (legacy branches all gone) */
return -ENOENT_VFS;
```

The "ramfs catchall" comment in the source moves up to the
vtable-dispatch block, recording that the root mount handles
"anything that didn't match a more specific mount." A reader
who looks at `vfs_open` now sees one branch — the vtable
dispatch — and a single error return. The same is true of
`vfs_open_into`, `vfs_load`, and the other dispatchers.

This is the win the whole chapter sequence was for. The
diff that lands this step is bigger than any previous one in
deleted lines and smaller than any in added lines.

## One MOUNT_RO subtlety this step introduces

The root mount has `MOUNT_RO`. Once chapter 139 adds the
write-flag check on top of `vfs_open`, writes against paths
under `"/"` that no other mount covers will now return
`-EROFS_VFS` instead of `-ENOENT_VFS`. Concretely:
`echo hi > /foo` used to fail because `/foo` doesn't exist
in the ramfs; after chapter 139 it fails because the root
mount is read-only.

Semantically the new error is more correct (the filesystem
*is* read-only; the path's nonexistence is incidental to the
fact that we'd refuse to create it anyway). The change is
invisible to every regression in the sweep because none of
them open non-existent paths under `"/"` for writing — they
all write to `/data`, `/tmp`, or the redirect targets the
shell creates. But it's a real behavioural shift, and chapter
139 calls it out explicitly so future bug reports about
"`echo > /etc/passwd` returns the wrong errno" don't surprise
anyone.

## What got verified

```
$ make -j8
$ for t in test_procfs.py test_directories.py test_notepad.py \
           test_clipboard.py test_boot_to_desktop.py \
           test_clone_files.py test_fork_exec.py test_cow.py \
           test_clock.py test_fontd.py; do
    python3 scripts/$t
  done
```

Ten tests, all green. The load-bearing ones for this step
are:

- `test_boot_to_desktop.py` — reads `/motd` during the boot
  sequence; if the root-mount dispatch is wrong, the boot
  banner doesn't appear and the desktop never reaches its
  ready state.
- `test_fontd.py` — `/bin/fontd` loads font data from
  `/mnt/fonts/...`; this exercises the OSFS-1 dispatch and
  the new "no more fall-through" behaviour together. If the
  root mount accidentally swallowed `/mnt/...` paths instead
  of `/mnt` losing the tie, font rendering would break.
- `test_fork_exec.py` — exec loads `/mnt/forktest` via the
  `vfs_load` dispatcher; same shape as `test_fontd.py` but
  exercises the `load` adapter instead of `open`+`read`.

The "resolver loses ties for `/`" behaviour from chapter
133 is what makes those tests stay green. If the root mount
had won every match, `/mnt/forktest` would have gone to the
ramfs (which doesn't have it) and exec would have failed.

## What gets exercised in tests

- `scripts/test_boot_to_desktop.py` — `/motd` read via root
  mount during boot
- `scripts/test_fontd.py` — confirms OSFS-1 dispatch still
  wins against the root mount for `/mnt/...` paths
- `scripts/test_fork_exec.py` — confirms exec's `vfs_load`
  dispatcher routes `/mnt/forktest` to OSFS-1, not to the
  root mount
- Regression sweep — full chapter 15 through chapter 101
  coverage

## Applied to

- Existing apps using the feature: the entire boot sequence
  (motd, the boot banner readers in init/desktop) routes
  through the new root mount. Same content, new dispatch
  path.
- Existing apps modified: none — invisible to userspace.
- New apps added: none yet.
- New test scripts: none — coverage came from the existing
  sweep.

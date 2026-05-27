# Chapter 139 — Step 7: `MOUNT_RO` + `EROFS_VFS` hardening

The final step in Part XVI's first half tightens one
invariant. Five of the six mounts in the table have
`MOUNT_RO` set: `/`, `/proc`, `/mnt`, `/bin` (yes, both;
they share an OSFS-1 instance), with `/tmp` and `/data` as
the two writable ones. The plan promised that *every*
mutation attempt against an RO mount would return
`-EROFS_VFS` (errno 30), and that the check would fire in
the VFS dispatcher *before* the driver's method ran. After
chapter 138's user-visible payoff, this chapter delivers
the contract enforcement.

It also closes a real bug — one the regression sweep didn't
catch and the previous six chapters didn't surface — where
mutations against RO mounts whose driver lacked the relevant
op pointer returned `-EINVAL_VFS` instead of the documented
`-EROFS_VFS`.

## The bug the chapter caught

The chapter-113 plan said RO enforcement was already done.
The first version of [`scripts/test_mount_ro.py`](../../../scripts/test_mount_ro.py)
disagreed loudly:

```
FAIL: mkdir /proc/foo => EROFS_VFS
   raw: b'mkdir: /proc/foo: errno=22\n/$ '
FAIL: mkdir /bin/foo  => EROFS_VFS
   raw: b'mkdir: /bin/foo: errno=22\n/$ '
FAIL: mkdir /mnt/foo  => EROFS_VFS
   raw: b'mkdir: /mnt/foo: errno=22\n/$ '
FAIL: mkdir /motd     => EROFS_VFS (root ramfs)
   raw: b'mkdir: /motd: errno=22\n/$ '
FAIL: rm /proc/self/cmdline => EROFS_VFS
   raw: b'rm: /proc/self/cmdline: errno=22\n/$ '
…
4 PASS / 8 FAIL
```

Every mutation against a RO mount was returning errno 22
(`EINVAL_VFS`), not errno 30 (`EROFS_VFS`). Only the
open-with-write-flags case worked correctly — and only
because it goes through `vfs_open`'s dispatch, which had the
RO check in the right place.

The cause was a two-line refactor mistake. In `sys_unlink`
and `sys_mkdir`, the chapter-113-step-4 code was:

```c
const struct mount *m = vfs_resolve(path, &rel);
if (m && m->ops && m->ops->unlink) {       /* ← bug */
    if (m->flags & MOUNT_RO) return -EROFS_VFS;
    return m->ops->unlink(m->cookie, rel);
}
return -EINVAL_VFS;
```

The RO check was inside the "the driver has an unlink op"
branch. But the RO mounts (`/proc`, `/`, `/mnt`, `/bin`)
deliberately have `unlink = NULL` because they don't support
deletion. So the `m->ops->unlink` check failed, the branch
was skipped entirely, and we fell through to the generic
`-EINVAL_VFS`. The RO check never had a chance to fire.

The fix is to hoist the RO check above the op-pointer check:

```c
const struct mount *m = vfs_resolve(path, &rel);
if (m) {
    if (m->flags & MOUNT_RO) return -EROFS_VFS;
    if (m->ops && m->ops->unlink)
        return m->ops->unlink(m->cookie, rel);
}
return -EINVAL_VFS;
```

Now an RO mount returns `EROFS_VFS` *regardless* of whether
its driver implements unlink. The `EINVAL_VFS` fall-through
only fires for writable mounts that legitimately don't
support the operation — which is the right shape (compare
POSIX, where `unlink` on a writable filesystem that doesn't
support deletion returns `EPERM` or `EOPNOTSUPP`; we map
both to `EINVAL_VFS` for now). The same fix is applied to
`sys_mkdir`.

## What MOUNT_RO catches

After the fix, the contract becomes:

| Op against RO mount | Returns | Where the check fires |
|---|---|---|
| `open(path, O_WRONLY)` | `-EROFS_VFS` | `vfs_open` |
| `open(path, O_RDWR)` | `-EROFS_VFS` | `vfs_open` |
| `open(path, O_CREAT)` | `-EROFS_VFS` | `vfs_open` |
| `open(path, O_TRUNC)` | `-EROFS_VFS` | `vfs_open` |
| `unlink(path)` | `-EROFS_VFS` | `sys_unlink` |
| `mkdir(path)` | `-EROFS_VFS` | `sys_mkdir` |

The same MOUNT_RO bit gates all six. `O_APPEND` is not on
the list because POSIX requires it be combined with
`O_WRONLY` or `O_RDWR`, both of which already trigger the
check.

`write(fd, ...)` on an fd opened against an RO mount can't
happen — the open would have been rejected — so there's no
runtime check inside `sys_write` and no slowdown on the hot
path.

## EROFS_VFS == 30, matches POSIX

The numeric value of `EROFS_VFS` is 30, the same as the
existing kernel-internal `EROFS` (`vfs.h`) and the same as
Linux's `EROFS`. The two names exist for documentation
clarity: the unsuffixed `EROFS` was used by chapter 17's
read-only fd writes ("you opened me O_RDONLY, you can't
write me"), and `EROFS_VFS` is the chapter-113 spelling for
mount-level RO. They're aliases at the wire level, so
userspace can compare against either and any future
distinction (filesystem-RO vs. fd-RO) can be teased apart
without a renumber.

## What got verified

```
$ make -j8
$ python3 scripts/test_mount_ro.py
PASS: mkdir /proc/foo => EROFS_VFS
PASS: mkdir /bin/foo  => EROFS_VFS
PASS: mkdir /mnt/foo  => EROFS_VFS
PASS: mkdir /motd     => EROFS_VFS (root ramfs)
PASS: rm /proc/self/cmdline => EROFS_VFS
PASS: rm /bin/ls => EROFS_VFS
PASS: rm /mnt/init => EROFS_VFS
PASS: rm /motd => EROFS_VFS (root ramfs)
PASS: echo > /proc/foo => EROFS_VFS at open
PASS: echo > /motd => EROFS_VFS at open (root ramfs)
PASS: echo > /tmp/probe.txt succeeds
PASS: echo > /data/probe.txt succeeds
12 PASS / 0 FAIL
```

Twelve cases, all green. The first eight assert that *every*
mutation against the four RO mounts returns errno 30. The
ninth and tenth re-confirm the open-time check still works.
The last two are the negative half of the contract:
writable mounts continue to accept writes (no overzealous
RO check sneaking in).

The full regression sweep from chapter 137 was rerun and
stayed green:

```
$ for t in test_procfs.py test_directories.py test_clipboard.py \
           test_notepad.py test_fsync.py test_clock.py \
           test_clone_files.py test_boot_to_desktop.py \
           test_mounts.py; do
    python3 scripts/$t
  done
```

Nine tests, all passing. The full mount-table refactor —
chapters 133 through 139 — is sweep-green and the prefix
ladders are gone.

## What didn't make this chapter

A handful of nice-to-haves are deliberately not in this
step:

- **`statfs`-style readback.** Userspace can't yet query the
  per-mount free-space or RO status of an open fd. The
  information is available via `mounts()`, so a caller can
  resolve their path to a mount and read the flag from
  there. A `SYS_FSTATFS` is straightforward to add when the
  first caller needs it.
- **The `MOUNT_NODEV`, `MOUNT_NOEXEC`, `MOUNT_NOSUID` bits.**
  Chapter 133 reserved bits 1..31 for them. None of them
  are implemented yet because the OS doesn't have suid (we
  don't have multi-user yet), doesn't have device files (we
  don't have a /dev hierarchy yet), and doesn't have an
  exec policy worth defining. They will land in the
  chapters that introduce the underlying features.
- **The legacy `sys_listdir`.** Chapter 135 noted that the
  flat-namespace `sys_listdir` still does its own ramfs +
  osfs walk and was deliberately not ported. After this
  chapter, every other code path is on the vtable — but
  `sys_listdir` remains as is. A future cleanup chapter
  can collapse it once we're sure no userspace depends on
  the "ramfs + osfs concatenated" shape.

## Looking ahead

Part XVI's first half is done. The dispatcher is table-driven,
the mount table is enumerable from userspace, and the
read-only contract is enforced uniformly. Chapter 140 picks
the abstraction up from here: a userspace daemon can now
mount itself into the namespace by implementing a 9P-shaped
RPC and registering through a new `SYS_MOUNT` syscall. The
work is unchanged in shape from the chapter 132 plan; the
infrastructure to support it is what these seven chapters
landed.

## What gets exercised in tests

- `scripts/test_mount_ro.py` — twelve cases covering every
  mutation operation × every RO mount, plus the writable
  control cases
- Regression sweep — confirms no behavioural shifts in any
  existing test

## Applied to

- Existing apps using the feature: every app that ever
  wrote to a path. The new error code is more accurate;
  no app today bears EROFS specifically, so the change is
  invisible to all of them.
- Existing apps modified: none.
- New apps added: none — the user-visible payoff was in
  138's `/bin/mount`. 139 is the invariant tightening
  that makes the abstraction safe.
- New test scripts: **`scripts/test_mount_ro.py`**.

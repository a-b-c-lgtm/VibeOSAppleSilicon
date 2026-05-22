# Chapter 113f — Step 6: `SYS_MOUNTS` and `/bin/mount`

The previous five chapters retired the prefix ladders without
changing anything userspace could see. This chapter is the
first one in the section that adds a new user-visible piece:
a way to ask the kernel "what filesystems are mounted, and
which ones can I write to?". The answer is the new
`SYS_MOUNTS` syscall, the userspace `mounts()` wrapper, and
a `/bin/mount` binary that prints the table — the same
information a Linux user gets from `cat /proc/mounts` or
`mount` with no arguments.

This is also the chapter that takes the chapter-113 plan's
"rework `save_dialog.c` to iterate the mount table" item and
defers it (with paperwork) to a later UI refresh. The
infrastructure to *do* the rework is what this chapter
delivers; the UI redesign that would actually consume it is
its own chunk of work and shouldn't ride along on a
filesystem-architecture chapter.

## The syscall

`SYS_MOUNTS = 95` (slot picked because chapter 112's
`SYS_GETRANDOM = 94` was the previous highest). The
[`kernel/core/syscall.h`](kernel/core/syscall.h) entry
spells the contract:

```c
/* Snapshot the kernel's mount table.  Userspace passes a
 * buffer typed as `struct mount_info[]` and a count; the
 * kernel writes back as many entries as fit (clamped to
 * the count), one per registered mount, and returns the
 * number written.  The struct layout is fixed at:
 *
 *   struct mount_info {
 *       char     prefix[32];
 *       uint32_t flags;
 *   };
 *
 * 32 bytes of prefix + a u32 flags word.  Prefix is
 * NUL-terminated and bounded at 31 chars (which fits every
 * mount we expect to ever ship; MOUNT_MAX is 16, and the
 * longest user-coined prefix in a userspace-mounted FS is
 * an open design choice the syscall just caps).
 *
 * Errors: -EFAULT on a bad pointer, -EINVAL on a negative
 * count.  A count of 0 returns 0 (the "how many would I
 * need?" answer is not yet available; pass a generous
 * buffer until we add a separate count query).
 */
SYS_MOUNTS = 95,
```

The kernel implementation is small enough to inline. From
[`kernel/core/syscall.c`](kernel/core/syscall.c):

```c
struct kern_mount_info {
    char     prefix[32];
    uint32_t flags;
};

static long sys_mounts(long out_uptr, long max_l)
{
    int max = (int)max_l;
    if (max < 0) return -EINVAL_VFS;
    if (max == 0) return 0;
    int total = vfs_mount_count();
    int n = total < max ? total : max;
    for (int i = 0; i < n; i++) {
        const struct mount *m = vfs_mount_at(i);
        struct kern_mount_info mi;
        for (size_t k = 0; k < sizeof(mi.prefix); k++) mi.prefix[k] = 0;
        const char *p = m->prefix;
        size_t k = 0;
        for (; p[k] && k + 1 < sizeof(mi.prefix); k++)
            mi.prefix[k] = p[k];
        mi.flags = m->flags;
        uint64_t dst = (uint64_t)(out_uptr + (long)(i * (long)sizeof(mi)));
        if (copy_to_user(dst, &mi, sizeof(mi)) < 0) return -EFAULT;
    }
    return (long)n;
}
```

Three things worth pointing at:

- **No padding requested, no padding inferred.** The struct
  is 32 + 4 = 36 bytes. The compiler may decide to round it
  up to 40 (alignment of the next struct in an array of
  these), but every concrete byte we emit lives at a known
  offset. The userspace counterpart is declared the same
  way, so the per-entry stride agrees. If we ever add a
  field, the bump goes at the end so old userspace continues
  to read the prefix and flags correctly.

- **Per-field zero, not `= {0}`.** Freestanding C with
  `-O2` will silently emit a `memset` call if we initialise
  `mi` with `{}` and the compiler thinks the struct is "big
  enough." We don't ship a `memset`, so the link breaks.
  This is the trap from
  [`/memories/freestanding-c-memset-trap.md`](memories/freestanding-c-memset-trap.md)
  and it's why the prefix is zeroed in an explicit loop.

- **Bounded copy.** The kernel's `m->prefix` is a string
  literal from the registration call ("/proc", "/data",
  etc.), so it always fits comfortably. The `k + 1 <
  sizeof(mi.prefix)` guard isn't load-bearing today but
  protects us the day chapter 114 lets userspace coin its
  own prefix string — at that point the kernel side stays
  unchanged, the cap just truncates.

## The userspace wrapper

From [`userspace/libc/syscall.h`](userspace/libc/syscall.h):

```c
#define MOUNT_RO 0x1u

struct mount_info {
    char     prefix[32];
    unsigned flags;
};

static inline long mounts(struct mount_info *out, int max)
{
    return _svc2(SYS_MOUNTS, (long)(uintptr_t)out, (long)max);
}
```

The header is the userspace ABI. `MOUNT_RO` is exported here
too — userspace callers should be able to test the bit
without `#include "kernel/core/vfs.h"`. The numeric value
matches the kernel one. Any future flag (`MOUNT_NODEV`,
`MOUNT_NOEXEC`) gets added in both headers at the same time
and the same bit number.

## `/bin/mount`: the user-visible payoff

A small new binary lives at
[`userspace/mount/mount.c`](userspace/mount/mount.c):

```c
#include "../libc/syscall.h"
#include "../libc/printf.h"

#define MAX_ENTRIES 16   /* matches MOUNT_MAX in kernel/core/vfs.h */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct mount_info table[MAX_ENTRIES];
    long n = mounts(table, MAX_ENTRIES);
    if (n < 0) {
        printf("mount: kernel returned %d\n", (int)n);
        return 1;
    }
    for (long i = 0; i < n; i++) {
        const char *ro = (table[i].flags & MOUNT_RO) ? "  [ro]" : "";
        printf("%s%s\n", table[i].prefix, ro);
    }
    return 0;
}
```

When you boot the OS and run `/bin/mount`, the output is:

```
$ /bin/mount
/proc  [ro]
/tmp
/mnt  [ro]
/bin  [ro]
/data
/  [ro]
```

That's the entire current mount table, in registration order.
The double-space before `[ro]` is intentional — it matches
`mount(8)` on Linux closely enough that muscle memory works.

This is the first chapter in Part XVI where you can sit at
the shell, type something, and *see* the new architecture.
The previous five chapters were structural: they made every
later chapter possible without changing what the user sees.
This one inverts that — the architecture finally surfaces as
a tool.

The chapter-113 Makefile wiring follows the same shape as
every other `/bin/` binary (compare `GETRAND_OBJS` /
`GETRAND_ELF` / `GETRAND_STRIPPED` from chapter 112).
`MOUNT_STRIPPED` gets added to `OSFS_BIN_FILES` so the file
ships in the disk image, and the OSFS layout rule names it
as `mount=$(MOUNT_STRIPPED)`. No other build-system surgery
needed.

## What we deferred (and where it lives)

The chapter-113 plan called for one more thing in this step:
rework `userspace/libgui/save_dialog.c` so it iterates the
mount table and offers any non-RO mount as a save
destination, instead of hard-coding `/data` as the only
writable place.

We did not do this. The reasons:

- The save dialog's existing UI is a single-pane file
  browser with one fixed root. Adding a "destination
  picker" page is a real UI redesign, not a one-line
  swap. It needs new widgets (a left rail listing mounts,
  a right pane showing the chosen mount's contents), new
  layout code, new tests.
- The save dialog has exactly one caller in production —
  `notepad`'s File → Save As — and that caller already
  defaults to `/data`. There is no user-reported pain to
  fix today.
- A half-done version (e.g. "if `dir_prefix` is NULL, call
  `mounts()` and pick the first non-RO one") buys nothing:
  notepad never passes NULL.

The `mounts()` syscall is the *infrastructure* that makes a
later UI rework possible without any kernel changes. When
the multi-mount destination picker lands (probably the same
chapter that adds a third writable mount), it will be a pure
userspace diff. Today's `SYS_MOUNTS` is forward-compatible
with that future without anticipating its shape.

## What got verified

```
$ make -j8
$ python3 scripts/test_mounts.py
PASS: / present with [ro]
PASS: /proc present with [ro]
PASS: /tmp present (writable)
PASS: /mnt present with [ro]
PASS: /bin present with [ro]
PASS: /data present (writable)
PASS: at least 6 mount entries reported (got 10)
7 PASS / 0 FAIL
```

The "got 10" line is the shell echo accounting for the
command, the prompt, and a couple of harness fragments —
`mount`'s actual output is six lines, and the test asserts
each of the six expected entries by name and RO disposition.

The full Step-5 sweep was rerun and stayed at green
(procfs, directories, clone_files, fork_exec, clock,
clipboard, notepad, fsync, boot_to_desktop).

## What gets exercised in tests

- `scripts/test_mounts.py` — calls `/bin/mount` and asserts
  every expected entry is present with the right RO flag
- Existing sweep — confirms no regressions from the new
  syscall slot or the `/bin/mount` addition

## Applied to

- Existing apps using the feature: none yet — `mounts()` is
  the new API; chapter 114 (userspace fs servers) will be
  the first heavy user. The save-dialog rework is deferred
  per the discussion above.
- Existing apps modified: none.
- New apps added: **`/bin/mount`**
  ([`userspace/mount/mount.c`](userspace/mount/mount.c)).
- New test scripts: **`scripts/test_mounts.py`**.

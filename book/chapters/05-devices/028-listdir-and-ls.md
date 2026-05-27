# Chapter 28 — Browsing the namespace: `SYS_LISTDIR` and `/bin/ls`

We've had a filesystem since chapter 11 and binaries on disk
since chapter 12. But to know what's actually *in* the FS you've
had to read the Makefile or grep the kernel for `RAMFS_FILE`
macros — the namespace was never visible from user code. That
ends here.

This chapter adds a single new syscall, `SYS_LISTDIR`, and a tiny
user program, `ls`, that walks every directory entry in the
combined ramfs + OSFS namespace and prints what it sees.

## What the user-visible result looks like

```
$ /bin/ls
        8  /motd
      234  /README
      113  /mnt/hello.txt
      569  /mnt/poem.txt
     5288  /mnt/init
     5656  /mnt/sh
     6704  /mnt/cat
     6488  /mnt/hello
     4584  /mnt/badpoke
     4896  /mnt/badptr
     5848  /mnt/heaptest
     6496  /mnt/echo
     7272  /mnt/printftest
     6496  /mnt/ls
```

Two ramfs entries (the embedded text files), eleven OSFS entries
(the on-disk binaries plus a couple of text files). Every path is
a real path you can hand to `cat` or `spawn`.

## The kernel side

There's no global "directory entry" abstraction in our VFS yet —
we have a hand-rolled ramfs table and a separate OSFS dirent
array. `vfs_listdir` glues them into one flat enumeration:

```c
long vfs_listdir(int idx, char *name, size_t cap, uint32_t *size_out)
{
    /* Slot 0..RAMFS_COUNT-1 -> ramfs file. */
    if ((size_t)idx < RAMFS_COUNT) {
        const struct ramfs_file *f = &g_ramfs[idx];
        copy_name(name, cap, f->name);
        *size_out = (uint32_t)ramfs_size(f);
        return strlen_capped(name, cap);
    }

    /* Slot RAMFS_COUNT.. -> OSFS dirent. */
    if (!osfs_present()) return -ENOENT_VFS;
    size_t osfs_idx = (size_t)idx - RAMFS_COUNT;
    if (osfs_idx >= osfs_file_count()) return -ENOENT_VFS;

    const struct osfs_dirent *e = osfs_dirent_at(osfs_idx);
    /* Compose "/mnt/" + dirent.name */
    write_prefixed(name, cap, "/mnt/", e->name, OSFS_NAME_MAX);
    *size_out = e->size_bytes;
    return strlen_capped(name, cap);
}
```

Three points worth noting:

1. **Single contiguous index space.** The caller doesn't need to
   know about mounts; it just walks 0, 1, 2, ... until it gets
   `-ENOENT`. When we add a third FS later (proc, dev, whatever)
   it slots into the index range without changing the API.
2. **No iterator state.** Each `listdir(idx, ...)` is independent
   — no per-process directory cursor to manage, no rewinddir,
   nothing. Costs an O(idx) probe per call but our directories
   are tiny (≤16 entries). When that becomes painful we'll add a
   real `DIR *` abstraction.
3. **No "/bin" alias listed separately.** OSFS files are
   accessible at both `/mnt/<name>` and `/bin/<name>` (the
   chapter-13 dispatch in `vfs_load`), but they're the same
   files. Listing them under one path keeps the catalogue honest.

## The syscall handler

Four arguments — first time we've used `x3` for anything, so the
SVC dispatcher had to grow:

```c
long a3 = (long)frame->x[3];
/* ... */
case SYS_LISTDIR: ret = sys_listdir(a0, a1, a2, a3); break;
```

The handler itself is straightforward except for the user-pointer
plumbing:

```c
static long sys_listdir(long idx, long name_ptr, long cap, long size_ptr)
{
    if (cap <= 0) return -EINVAL_VFS;
    if (cap > 256) cap = 256;       /* cap the kernel staging buffer */
    if (uaccess_check((uint64_t)name_ptr, (size_t)cap) != 0)
        return -EFAULT;
    if (uaccess_check((uint64_t)size_ptr, sizeof(uint32_t)) != 0)
        return -EFAULT;

    char     name[256];
    uint32_t size = 0;
    long     n = vfs_listdir((int)idx, name, (size_t)cap, &size);
    if (n < 0) return n;

    if (copy_to_user((uint64_t)name_ptr, name, (size_t)n + 1) < 0)
        return -EFAULT;
    if (copy_to_user((uint64_t)size_ptr, &size, sizeof(size)) < 0)
        return -EFAULT;
    return n;
}
```

Same pattern as every other user-pointer-touching syscall (see
chapter 24): bounds-check both pointers up front, do the work
into kernel-side staging buffers, then `copy_to_user` the
results. The kernel staging cap (`256`) is intentionally larger
than `OSFS_NAME_MAX + len("/mnt/") + 1 = 25` so future longer
paths fit without changing the syscall ABI.

## The user side

Three new lines in `userspace/libc/syscall.h`:

```c
static inline long listdir(int idx, char *name, size_t cap,
                           unsigned int *size_out)
{
    return _svc4(SYS_LISTDIR, idx, (long)(uintptr_t)name,
                              (long)cap, (long)(uintptr_t)size_out);
}
```

And we needed a new `_svc4` helper too — every syscall up to
this one fit in three or fewer arg registers.

The `ls` program itself:

```c
int main(int argc, char **argv)
{
    char         name[128];
    unsigned int size = 0;
    int          listed = 0;

    for (int idx = 0;; idx++) {
        long n = listdir(idx, name, sizeof(name), &size);
        if (n < 0) break;
        printf("  %8u  %s\n", size, name);
        listed++;
    }

    if (listed == 0)
        printf("ls: no files\n");
    return 0;
}
```

That's the entire program. No flags, no globbing, no sort, no
column formatting. Width-8 right-justified size column courtesy of
the chapter-28 `printf`.

## Why this matters

Two reasons:

**Discoverability.** Up to now the only way for a user of our OS
to find out what they could run was to know it ahead of time.
That's fine for a hobby project where the user is the developer,
but it's not how Unix systems feel — `ls /bin` is the first thing
anyone does on a strange box. We have it now.

**An iteration interface for future code.** Anything that needs
to walk every file (a backup tool, a search command, a content
indexer for a future browser cache) now has a syscall it can
call. `vfs_listdir` is also the natural hook for a `glob(3)`
implementation when we want one.

## What's still missing

- **No `stat`.** Today `listdir` returns name + size only. A
  future `stat`/`fstat` syscall would expose mtime, mode bits
  (rwx for owner/group/world — once we have those at all),
  inode/dev numbers, file type. Since we don't have mtime (no
  RTC integration) or mode bits (no permissions) yet, there's
  not much more to expose right now.
- **No subdirectories.** Both ramfs and OSFS-1 are flat. The
  flat namespace means `listdir` doesn't need a path argument.
  When we get a hierarchical FS the syscall grows a path:
  `listdir(int idx, const char *path, char *name, ...)`.
- **No iterator state.** Today's syscall is O(idx) per call
  because each invocation walks the OSFS dir from start. Fine
  at 11 entries, painful at 11000.
- **No write side.** Can't `mkdir`, can't `unlink`, can't
  rename. OSFS-1 is read-only by design (no free-space map);
  the writable filesystem chapter is still ahead.

## What changed

```
kernel/core/vfs.{h,c}           vfs_listdir, walks ramfs + OSFS
kernel/core/syscall.{h,c}       SYS_LISTDIR = 12; sys_listdir handler;
                                svc_dispatch grows a3 register slot
userspace/libc/syscall.h        listdir() wrapper + _svc4 helper
userspace/ls/ls.c               NEW — walks the namespace and prints
Makefile                        wires ls into disk image
```

A new syscall (#12), one new VFS function, one new program,
~150 lines of new code total. The OS is now self-describing.

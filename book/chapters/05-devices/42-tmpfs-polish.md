# Chapter 42 — tmpfs polish: `>>`, `ls /tmp/`, `rm /tmp/...`

Chapter 41 introduced a writable filesystem at `/tmp/` and a shell `>`
operator.  This chapter rounds it out into a useful working FS by
adding three pieces that every Unix user expects:

1. `>>` for **append**-mode redirection.
2. `ls` enumerates `/tmp/` files alongside ramfs and OSFS entries.
3. `rm <path>` removes a tmpfs file.

Each is small in isolation; together they make the writable FS feel
like a filesystem rather than a write-once curiosity.

## `>>`: append

The kernel side is almost free.  `tmpfs_write` already appends to the
file's current size — the `O_TRUNC` semantics in chapter 41 only kicked
in inside `vfs_open`'s `/tmp/` branch.  We split it:

```c
if (flags & O_CREAT) {
    int found = tmpfs_lookup(bare);
    if (found >= 0) {
        tidx = found;
        if (!(flags & O_APPEND))
            (void)tmpfs_create_or_truncate(bare);   /* truncate */
    } else {
        tidx = tmpfs_create_or_truncate(bare);      /* create */
        if (tidx < 0) return -ENOMEM_VFS;
    }
}
```

So `O_CREAT|O_TRUNC` truncates an existing file (the `>` case),
`O_CREAT|O_APPEND` keeps existing bytes (the `>>` case), and either way
a missing file is created fresh.

`tmpfs_seek_end` is added as a no-op hook — once tmpfs grows
seek/overwrite support, append-on-open will need to actually advance the
write cursor.  Today the cursor *is* the size, so opening an existing
file already lands "at end" naturally.

The shell parser detects `>>` by peeking one character past the `>`:

```c
if (*p == '>') {
    int two = (p[1] == '>');
    char *q  = p + (two ? 2 : 1);
    ...
    redir_out_append = two;
}
```

…and the open call site picks the right flag:

```c
int oflags = O_WRONLY | O_CREAT;
oflags |= redir_out_append ? O_APPEND : O_TRUNC;
sh_out_fd = open(redir_out, oflags);
```

Verified:

```
/$ echo aaa > /tmp/x
/$ echo bbb >> /tmp/x
/$ echo ccc >> /tmp/x
/$ cat /tmp/x
aaa
bbb
ccc
```

## `ls /tmp/`: third tier in vfs_listdir

`vfs_listdir(idx)` previously walked two tiers: ramfs first, then OSFS
when `idx >= RAMFS_COUNT`.  We extend the index space to include tmpfs:

```c
size_t osfs_n = osfs_present() ? osfs_file_count() : 0;
if (osfs_idx < osfs_n) { /* OSFS branch */ }
size_t tmp_idx = idx - RAMFS_COUNT - osfs_n;
if (tmp_idx >= tmpfs_count()) return -ENOENT_VFS;
char tname[TMPFS_MAX_NAME];
uint32_t tsize = 0;
tmpfs_listdir(tmp_idx, tname, sizeof(tname), &tsize);
/* compose "/tmp/" + tname into `name` */
```

`tmpfs_listdir(k, ...)` is "the kth in-use file" rather than a slot
index, so iterating `0..N-1` doesn't have to skip holes left behind by
`rm`.  The kernel-side helper walks the slot table linearly counting
in-use entries until it reaches `k`.

The userspace `ls` does not need any change; it's been calling
`SYS_LISTDIR` in a loop since chapter 29 and just sees a longer list
now.  Output:

```
/$ echo aaa > /tmp/x
/$ ls
   ...
        12  /tmp/x
```

(tmpfs entries appear *after* OSFS entries because they live at the
tail of the unified index space.  Sorting is a userspace concern.)

## `rm /tmp/...`: SYS_UNLINK and the shell builtin

A new syscall:

```c
SYS_UNLINK = 25,    /* (const char *path) -> int (0 ok, -errno) */
```

The implementation refuses anything outside `/tmp/` — OSFS-1 and ramfs
have no way to delete entries, so attempting `rm /motd` would silently
succeed if we let it through, then leak the next time the file was
read.  Returning `-EINVAL` for non-tmpfs paths is honest.

```c
static long sys_unlink(long path_uptr)
{
    char path[128];
    long n = copy_string_from_user(path, path_uptr, sizeof(path));
    if (n < 0) return n;
    static const char prefix[] = "/tmp/";
    int i;
    for (i = 0; i < (int)sizeof(prefix) - 1; i++)
        if (path[i] != prefix[i]) return -EINVAL_VFS;
    if (!path[i]) return -EINVAL_VFS;     /* "/tmp/" with no name */
    return tmpfs_unlink(path + i);
}
```

`tmpfs_unlink(name)` looks up the slot, frees its data buffer with
`kfree`, and clears `in_use`.  Open fds referencing the unlinked file
will start returning `-EBADF` on subsequent read/write because tmpfs's
read/write checks `f->in_use`.  This is **not** POSIX semantics (where
unlinked-but-open files survive until the last fd closes), but it's
adequate for now and avoids the refcount machinery we'd otherwise need.

The shell `rm` builtin parses one or more whitespace-split paths and
calls `unlink()` on each:

```c
if (starts_with(line, "rm ")) {
    char *p = line + 3;
    while (*p == ' ' || *p == '\t') p++;
    int any_err = 0;
    while (*p) {
        char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char saved = *p; *p = '\0';
        int rc = unlink(start);
        if (rc != 0) { /* report error, set any_err */ }
        *p = saved;
        while (*p == ' ' || *p == '\t') p++;
    }
    g_last_exit = any_err ? 1 : 0;
}
```

Verified end-to-end:

```
/$ ls | grep tmp        (none)
/$ echo aaa > /tmp/a; echo bbb > /tmp/b
/$ ls | grep tmp
        4  /tmp/a
        4  /tmp/b
/$ rm /tmp/a /tmp/b
/$ ls | grep tmp        (none)
/$ rm /tmp/a
rm: /tmp/a: errno=2
```

## Files changed

- `kernel/core/tmpfs.h`: added `tmpfs_seek_end`, `tmpfs_unlink` protos.
- `kernel/core/tmpfs.c`: added `tmpfs_seek_end` (no-op) and
  `tmpfs_unlink` (frees buffer + clears slot).
- `kernel/core/vfs.c`: `vfs_open` /tmp/ branch honours `O_APPEND`;
  `vfs_listdir` walks tmpfs as a third tier after OSFS.
- `kernel/core/syscall.h`: `SYS_UNLINK = 25`.
- `kernel/core/syscall.c`: `sys_unlink` implementation; dispatch case.
- `userspace/libc/syscall.h`: `SYS_UNLINK` enum entry; `unlink()` wrapper.
- `userspace/sh/sh.c`: `>>` parsing in the existing `>` parser; `rm`
  builtin; help text updated.
- `kernel/core/main.c`: banner -> milestone 33.

## What's still missing

- Open fds to an unlinked tmpfs file go straight to `-EBADF` on next
  read/write — not POSIX "linger until last close" semantics.
- `rm` doesn't accept globs or `-r`.
- No `mv` (rename).
- No truncate-to-N or ftruncate; only via `>`-style truncate-on-open.
- No `mkdir` / no subdirectories.
- Still no quote-aware shell lexer, so `rm '/tmp/odd name'` is a syntax
  error.

The next milestones are likely signals (so `^C` works), a real shell
line editor with arrow-key history, or the start of a disk-backed
writable FS.  None of those depend on tmpfs polish that wasn't already
in chapter 41.

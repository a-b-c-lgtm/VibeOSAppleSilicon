# Chapter 117 — A POSIX-ish libc, part 2: stat, fcntl, dirent, getcwd

**Status:** Stub. Tracking the second chunk of milestone
"native compiler libc". See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

Chapter 116 covered the *byte streams* a compiler needs.
This chapter covers the *file system* surface it needs:
the calls that ask "is this a header or a directory?",
"what flags did this fd open with?", "what's my working
directory?", "give me the next directory entry".

GCC and TCC walk the include path with `stat` to decide
between `#include "foo.h"` and `#include <foo.h>`.
Configure-style projects (which we'll meet at chapter
122 when GCC's own build system runs) use `access(path,
X_OK)` to test for tools. `dup2` and `fcntl(F_DUPFD)`
are how shells (and gcc's driver) plumb pipes between
`cpp`, `cc1`, `as`, and `ld`. Our existing fork/exec
plumbing inherits fds from chapter 93, but the
*userspace* primitives that move them around haven't
shipped yet.

## What this chapter adds

- `userspace/libc/sys/stat.h` + `stat.c`:
  `stat` / `fstat` / `lstat` returning a `struct stat`
  with `st_mode`, `st_size`, `st_mtime`, plus the
  classification macros (`S_ISDIR`, `S_ISREG`, ...).
  Backed by a new `SYS_FSTATAT` syscall that walks our
  mount table (chapter 113, once it lands) and returns
  the metadata each FS already tracks internally.
- `userspace/libc/fcntl.h` + the `open` flag set we
  haven't needed yet (`O_CREAT`, `O_TRUNC`, `O_APPEND`,
  `O_EXCL`, `O_RDWR` — `O_RDONLY` and `O_WRONLY`
  already exist). `creat(p, m)` as a one-line wrapper.
  `fcntl(fd, F_GETFL/F_SETFL/F_DUPFD)`.
- `userspace/libc/unistd.h`: `dup` / `dup2` (real),
  `access(path, mode)`, `getcwd(buf, n)`, `chdir`,
  `rmdir`, `link` (returns `-ENOSYS` for now — we don't
  have hard links yet, but the symbol has to exist or
  GCC's host code won't link), `readlink` (same — stub
  returning `-EINVAL`).
- `userspace/libc/dirent.h` + `dirent.c`:
  `opendir` / `readdir` / `closedir` / `rewinddir` on
  top of our `SYS_LISTDIR_AT`.
- `getopt` (chapter 122's GCC driver needs it).

## Prerequisites

- Chapter 113 — VFS mount table (provides the uniform
  `stat` dispatch). If chapter 113 hasn't landed when
  this chapter is implemented, the stat path is built
  as a sixth prefix-special-case ladder and refactored
  in chapter 113.
- Chapter 93 — CLONE_FILES (so `dup2` semantics work
  correctly across fork).

## Plan

1. Add `SYS_FSTATAT` and the in-kernel `vfs_fstatat`.
   The four filesystems we have today (OSFS-1, tmpfs,
   OSFS-2, procfs) each get a `.stat` method on the
   forthcoming `fs_ops` vtable.
2. Add the `O_*` flag-set expansion to `sys_open` and
   plumb each through the kernel. `O_CREAT` already
   exists in tmpfs / OSFS-2; this chapter generalises
   the surface.
3. Implement `dup` / `dup2` as one wrapper around a
   new `SYS_DUP2`. The kernel already refcounts
   `fd_entry`; the userspace work is mostly the libc
   wrappers and `unistd.h`.
4. `opendir` / `readdir` is a 50-line wrap of the
   existing `SYS_LISTDIR_AT`. The `DIR *` carries a
   small cached batch so we don't syscall per entry.
5. `getcwd` reads `/proc/self/cwd` once chapter 99's
   procfs is in scope (which it is — chapter 99 is
   shipped). Chapters that miss procfs can fall back
   to the env `PWD` chapter 33 maintains.

## What you'll learn

- Why `struct stat` looks the way it does — every
  field is something a POSIX shell command actually
  reads.
- The difference between `dup`, `dup2`, and
  `fcntl(F_DUPFD)`, and why all three exist.
- How `opendir` / `readdir` is *not* a syscall on most
  Unixes — it's a libc convenience over the underlying
  directory-reading primitive.

## What this unlocks

- Chapter 118 (`/bin/as`) can use real `fopen` on `.s`
  inputs and `creat` on the output `.o`.
- Chapter 120's runtime crt0 can locate `/etc/libc.so`
  via `access` (when we eventually grow shared libs).
- Chapter 121 (TCC port) goes through with no
  per-call hand-holding around missing libc symbols.

## Applied to

- **Existing app rewrites:** `ls` rewritten on top of
  `opendir` / `readdir` (loses ~80 lines of
  prefix-special-casing). Notepad's save dialog
  (chapter 84) gets `getcwd` so it opens in the
  current directory, not always at `/data/`. Shell's
  `cd` builtin moves to `chdir(2)` instead of the
  custom syscall we added in chapter 32.
- **New apps:** none in this chapter; the toolchain
  bring-up (118+) is where the surface gets exercised.
- **New tests:** `scripts/test_libc_stat.py`,
  `scripts/test_libc_dirent.py`,
  `scripts/test_libc_dup2.py`.

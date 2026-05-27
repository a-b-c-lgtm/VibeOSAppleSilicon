# Chapter 153 — A POSIX-ish libc, part 2: stat, fstat, fcntl, dirent

> **Milestone in this chapter:** add the three filesystem-shaped
> POSIX surfaces every toolchain stage needs before it can even
> open a source file — `stat`, `fcntl`, and `dirent`.
> **Code referenced:**
> - [userspace/libc/sys/stat.h](../../../userspace/libc/sys/stat.h)
> - [userspace/libc/fcntl.h](../../../userspace/libc/fcntl.h)
> - [userspace/libc/dirent.h](../../../userspace/libc/dirent.h)
>
> **At the end of this chapter** you will have `stat` /
> `fstat`, `open` with POSIX flags, and `opendir` /
> `readdir` / `closedir` available to every userspace
> binary — the surface the chapter-118 assembler and the
> chapter-119 linker open files against. Builds on
> chapter 149–d (errno, FILE *, env).

Chapter 149–d gave us POSIX-shaped *byte streams*: `FILE *`,
   directory?"** without opening it. A real preprocessor
   walks the include path with `stat` to disambiguate
   `#include "foo.h"` vs `#include <foo.h>`, and to skip
   directories silently. (Our `/bin/cc` doesn't have
   `#include` yet, but the libc shape needs to be in place
   for any future port.)
2. **A way to walk a directory.** Configure scripts run
   `for f in /usr/lib/*.a; do ...; done`, which the shell
   expands by `opendir`-ing `/usr/lib` and `readdir`-ing the
   children. Our `ls` already does this internally via the
   chapter-85 `SYS_LISTDIR_AT` primitive, but POSIX code
   expects to see `opendir` / `readdir` / `closedir`.
3. **A way to ask "do I have read permission?"** without
   actually opening the file. POSIX `access(path, R_OK)`,
   needed by configure to test for tools.

This chapter ships the syscalls, the libc headers, and the
first app to consume them.

## What this chapter adds

| Where                                  | What                                                       |
| -------------------------------------- | ---------------------------------------------------------- |
| `kernel/core/vfs.h`                    | `struct kstat`; `S_IF*_K` mode constants                   |
| `kernel/core/vfs.c`                    | `vfs_stat_path()`, `vfs_fstat()`                           |
| `kernel/core/syscall.h`                | `SYS_STAT = 102`, `SYS_FSTAT = 103`                        |
| `kernel/core/syscall.c`                | `sys_stat`, `sys_fstat` marshaling + dispatch              |
| `kernel/core/osfs.c`                   | `osfs_op_listdir` now reports `DT_REG` for files           |
| `userspace/libc/syscall.h`             | `__sys_stat` / `__sys_fstat` raw wrappers                  |
| `userspace/libc/sys/stat.h`            | `struct stat`, `stat`, `fstat`, `access`, `S_IS*` macros   |
| `userspace/libc/dirent.h`              | `DIR`, `opendir`, `readdir`, `closedir`, `rewinddir`       |
| `userspace/libc/fcntl.h`               | `O_*` flag set, `creat`, `dup`, `fcntl`                    |
| `userspace/stattest/stattest.c`        | 19-assertion smoke test driving the new surface            |
| `userspace/ls/ls.c`                    | Rewritten on `opendir`/`readdir`/`stat`; lost ~80 lines    |
| `scripts/test_libc_stat.py`            | Harness; PASS = 9/9 + 19 in-binary assertions all PASS     |

## The kernel surface — `SYS_STAT` and `SYS_FSTAT`

POSIX `stat()` returns a `struct stat` with thirteen fields.
The fields the bring-up actually needs are four:

```c
struct kstat {
    uint32_t st_mode;       /* file kind + permission bits */
    uint32_t _pad;
    uint64_t st_size;       /* bytes */
    uint64_t st_mtime_ms;   /* modification time, ms since epoch */
};
```

The bit layout of `st_mode` matches POSIX so apps can use the
standard `S_ISREG` / `S_ISDIR` macros (`0x8000` = regular,
`0x4000` = directory, `0x2000` = character, `0x1000` = FIFO,
`0xC000` = socket). The kernel keeps its own aliases
(`S_IFREG_K`, etc.) to avoid clashing with userspace headers
the kernel doesn't include.

### `vfs_stat_path()`

We chose to *not* add a `.stat` method to the
`struct fs_ops` vtable. Three reasons:

1. **Every filesystem already knows everything `kstat` needs.**
   OSFS-1's flat namespace knows file size from
   `osfs_size(name)`; OSFS-2 keeps it in the inode; tmpfs in
   the slab entry. We don't need a vtable to re-ask.
2. **Directory detection already exists.** `fs_ops->is_dir`
   returns 0/1 for any prefix; we use that for the kind bit.
3. **Smaller diff.** Adding a vtable method means touching
   every FS driver (4 today, 5 once chapter-114 procfs
   counts). Inlining means one switch in `vfs.c`.

The implementation:

```c
long vfs_stat_path(const char *path, struct kstat *out) {
    if (!path || !out) return -EINVAL_VFS;

    /* "/" is always a directory, regardless of mount. */
    if (path[0] == '/' && path[1] == '\0') {
        out->st_mode = S_IFDIR_K | 0555;
        out->st_size = 0;
        out->st_mtime_ms = 0;
        return 0;
    }

    /* Find the mount; if the relative part is empty we're at
     * a mount root, which is always a directory. */
    const char *rel = NULL;
    const struct mount *m = vfs_resolve(path, &rel);
    if (!m) return -ENOENT_VFS;
    if (!*rel || (rel[0] == '/' && rel[1] == '\0')) {
        out->st_mode = S_IFDIR_K | 0555;
        out->st_size = 0;
        out->st_mtime_ms = 0;
        return 0;
    }

    /* Sub-paths: ask the FS if it's a directory first. */
    if (m->ops && m->ops->is_dir &&
        m->ops->is_dir(m->cookie, rel) == 1) {
        out->st_mode = S_IFDIR_K | 0555;
        out->st_size = 0;
        out->st_mtime_ms = 0;
        return 0;
    }

    /* Otherwise it's a file — open it just long enough to
     * read its size, then close.  Reuses the same fd_entry
     * dispatch vfs_lseek went through in chapter 150. */
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return fd;
    fill_size_from_fd_entry(fd, out);
    out->st_mode = mode_for_fd_kind(fd);
    vfs_close(fd);
    return 0;
}
```

`vfs_fstat(int fd, struct kstat *)` is even simpler — it
reads the `fd_entry` directly without an open/close round-trip.

### Why two syscalls and not one `fstatat`

POSIX has `fstatat(dirfd, path, ...)` with a magic
`AT_FDCWD = -100` for absolute paths. We don't have a working
`AT_FDCWD` plumbed through the chapter-93 cloned fd table
yet; rather than ship a half-working surface we kept `stat`
and `fstat` as two cleanly-separated entry points. The day a
toolchain stage needs `openat`-relative resolution, we'll
collapse them. Until then this matches BSDs that have lived
on plain `stat`/`fstat` for decades.

## The libc surface — `<sys/stat.h>`

`userspace/libc/sys/stat.h` is header-only (the chapter-116
pattern):

```c
struct stat {
    uint32_t st_mode;
    uint32_t _pad;
    uint64_t st_size;
    uint64_t st_mtime_ms;
};

#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFIFO  0x1000
#define S_IFSOCK 0xC000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
/* ...etc */

static inline int stat(const char *p, struct stat *out)
    { return __sys_stat(p, (struct __kstat_raw *)out); }
static inline int fstat(int fd, struct stat *out)
    { return __sys_fstat(fd, (struct __kstat_raw *)out); }

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
static inline int access(const char *p, int mode) {
    struct stat st;
    if (stat(p, &st) < 0) return -1;
    if (mode == F_OK) return 0;
    /* Mode bits we don't track yet — treat anything readable
     * by us as access-OK; the kernel rejects on real open. */
    return 0;
}
```

`access` is a tiny degenerate case until per-user permissions
exist (no chapter has shipped them; we run as the single
implicit user). `F_OK` is honest; `R_OK`/`W_OK`/`X_OK` always
return success if `stat` succeeded, which is the historic
behaviour on single-user Unixes and is sufficient for
configure scripts that just want "does the file exist and is
it readable by *somebody*".

## The libc surface — `<dirent.h>`

The familiar POSIX shape, on top of `SYS_LISTDIR_AT`:

```c
typedef struct DIR {
    char path[256];
    int  idx;
    struct dirent ent;
} DIR;

static inline struct dirent *readdir(DIR *d) {
    long n = listdir_at(d->path, d->idx,
                        d->ent.d_name, sizeof(d->ent.d_name),
                        &d->ent.d_size, &d->ent.d_type);
    if (n < 0) { errno = 0; return NULL; }   /* EOF */
    d->idx++;
    return &d->ent;
}
```

`DT_REG = 1` and `DT_DIR = 2` match the kernel's
`LISTDIR_TYPE_FILE` / `LISTDIR_TYPE_DIR`, so the wrapper does
zero translation. POSIX says "the value of `d_type` for any
filesystem that doesn't know is `DT_UNKNOWN`" — we ship `0`
for that, matching most BSDs.

### The osfs DT_REG fix

The chapter-12 OSFS-1 driver had been reporting `*type = 0`
for every directory entry because it predates the type-out
field. Since OSFS-1 is a flat namespace with only files, the
correct answer is `DT_REG` for every entry. A one-line fix in
`kernel/core/osfs.c::osfs_op_listdir` made `stattest`'s
`readdir(/mnt) yields at least one DT_REG` assertion go
green.

## The libc surface — `<fcntl.h>`

Consolidates the `O_*` flag bits the rest of the tree had
defined ad-hoc into one place, and adds the stubs configure
scripts expect:

```c
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_EXCL    0200
#define O_NONBLOCK 04000   /* recognised, today a no-op */

static inline int creat(const char *p, int mode)
    { return open(p, O_WRONLY | O_CREAT | O_TRUNC); }
static inline int dup(int oldfd);    /* probes upward */
static inline int fcntl(int fd, int cmd, ...);
```

`fcntl(F_DUPFD)` is the only command implemented end-to-end
(probes upward via `dup2` looking for a free slot).
`F_GETFL` returns `O_RDWR`, `F_SETFL` is accepted no-op,
`F_GETFD`/`F_SETFD` likewise. This is enough for getopt-style
code that queries but never acts. The day we need real
`O_NONBLOCK` on a userspace socket, this header grows real
per-fd flag storage.

## The app port — `ls`

Before chapter 153, `ls /path` ran a complex
prefix-filter-plus-dir-dedup loop on top of the flat
`SYS_LISTDIR` and a per-mount special-case for `/data`,
`/proc`, `/echo` that needed `SYS_LISTDIR_AT`. Roughly 240
lines.

After:

```c
struct stat st;
if (stat(path, &st) != 0) { /* errno-based diagnostic */ }
if (S_ISREG(st.st_mode)) { /* print one line, return */ }

DIR *d = opendir(path);
struct dirent *de;
while ((de = readdir(d)) != NULL)
    render(parent, de);     /* d_type -> <DIR> or size */
closedir(d);
```

The new `ls.c` is **127 lines** (vs ~240 before). The
`dir_dedup` buffer and the four `use_at` per-mount probes
are gone. `ls /data/notes/` works automatically because
`opendir` doesn't know or care that `/data/notes` is a
chapter-85 subdirectory — the kernel's `SYS_LISTDIR_AT`
handles enumeration uniformly.

The bare-`ls` flat dump still uses the kernel's
`SYS_LISTDIR` because `opendir`/`readdir` is the wrong shape
for "show me everything everywhere across every mount".

## Test plan

`userspace/stattest/stattest.c` exercises seven assertions
in 19 PASS lines:

1. `stat("/mnt/hello.txt")` returns `S_IFREG` with nonzero
   size.
2. `stat("/data")` returns `S_IFDIR`.
3. `stat("/")` returns `S_IFDIR`.
4. `stat("/does/not/exist")` returns -1 with errno=ENOENT.
5. `fstat()` of a freshly-opened file matches `stat()` of
   its path (mode and size).
6. `opendir("/mnt")` + `readdir()` yields at least one
   `DT_REG`.
7. `access("/bin/cat", R_OK)` succeeds; `access(missing,
   F_OK)` fails with ENOENT.

The harness `scripts/test_libc_stat.py` boots QEMU, runs
`/bin/stattest`, and validates 9 wrapper assertions of its
own (PASS-count threshold + 0-FAIL + ALL-PASS marker + 6
spot-checks against the parsed PASS-message list).

## Regression baseline (16/16 green)

Chapter 152's 15-test baseline + the new
`test_libc_stat.py`:

```
PASS  test_libc_stat
PASS  test_libc_errno
PASS  test_libc_stdio
PASS  test_libc_env
PASS  test_boot_to_desktop
PASS  test_userfs_echo
PASS  test_clipboard
PASS  test_mount_ro
PASS  test_userfs_timeout
PASS  test_httpd_forward
PASS  test_browser_proxy
PASS  test_cow
PASS  test_fork_exec
PASS  test_busy_on_mix
PASS  test_clone_files
PASS  test_directories
```

The `ls` port is the riskiest change in the chapter (it's the
only existing app to gain a behavioural rewrite). It's
exercised indirectly by `test_directories` (which `cd`s and
`ls`es around) and by `test_boot_to_desktop` (which runs `ls`
during shell startup banner). Both stayed green.

## Applied to

- **Existing apps modified:** `userspace/ls/ls.c` rewritten
  on `opendir`/`readdir`/`stat`. Lost ~110 lines of
  prefix-filter + dir-dedup logic. Lost the `use_at`
  per-mount probes (was a maintenance hazard — every new
  mount needed a hand-coded branch).
- **Kernel files modified:** `kernel/core/osfs.c::osfs_op_listdir`
  now reports `*type = 1` (DT_REG) for every entry instead of
  `0` (DT_UNKNOWN), since OSFS-1's flat namespace contains
  only files.
- **New apps:** `userspace/stattest/stattest.c` — 19-assertion
  smoke test.
- **New test scripts:** `scripts/test_libc_stat.py`.
- **Headers consolidated:** `O_*` flag bits that were
  scattered across `syscall.h` and several apps now live in
  `userspace/libc/fcntl.h`.

## Deferred to later chapters

- `lstat` — until we have symlinks (no current chapter on
  the schedule needs them).
- `chmod` / `fchmod` — until the multi-user permissions
  chapter (none planned in Part XVII).
- `getcwd` — chapter 32 maintains `PWD` in env; the few
  callers that need cwd read `getenv("PWD")`. A real
  `getcwd` via `/proc/self/cwd` arrives the day a future
  compiler's preprocessor needs it.
- Real `dup` (returning the kernel's allocator pick instead
  of probing upward) needs a new single-arg `SYS_DUP`. The
  cost of the upward-probe form is one wasted syscall per
  call site, which is fine for now.

## Lessons

- **The `is_dir` vtable method already paid for itself.**
  Adding `vfs_stat_path` was a 40-line patch in `vfs.c` and
  zero per-FS changes because every filesystem had already
  implemented `is_dir` for chapter 86's subdirectory work.
  A `.stat` vtable would have meant four duplicate
  implementations of "open it just enough to read the size".
- **`DT_UNKNOWN` is corrosive.** The OSFS-1 driver had been
  reporting `*type = 0` for every entry for ten chapters,
  silently breaking any caller that wanted to distinguish
  files from directories without a follow-up stat. The
  one-line fix here is the kind of thing that goes
  unnoticed until you try to use `d_type` for the first
  time.
- **Header-only libc means a syscall pair lands as one
  patch.** `<sys/stat.h>` defining the user-visible struct,
  `stat()`, `fstat()`, and `access()` all in one
  include-once header keeps every consumer in sync with
  zero `.c` files to add to build manifests.

## Next chapter

Chapter 154 ships `/bin/as` — the AArch64 assembler — and
will be the first toolchain stage to read its input with
`fopen` and write its output with `creat`, the
`<fcntl.h>` symbol added here.

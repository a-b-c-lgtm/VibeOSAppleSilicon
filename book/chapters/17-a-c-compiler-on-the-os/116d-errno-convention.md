# Chapter 116d — POSIX `errno` convention + `strerror` + first FILE-* app port

> **Milestone in this chapter:** flip every syscall wrapper
> from the chapter-7 "return `-errno`" convention to the POSIX
> "return `-1`, set `errno`" convention, then walk the ~50
> caller sites that printed the negated kernel code.
> **Code referenced:**
> - [userspace/libc/syscall.h](../../../userspace/libc/syscall.h)
>   (the `_svc*` wrappers + `__svc_check`)
> - [userspace/libc/errno.h](../../../userspace/libc/errno.h)
>   (`strerror`)
> - [userspace/cat/](../../../userspace/cat/),
>   [userspace/wc/](../../../userspace/wc/),
>   [userspace/head/](../../../userspace/head/),
>   [userspace/tail/](../../../userspace/tail/) (rewritten on
>   top of `FILE *`)
>
> **At the end of this chapter** you will have every libc
> entry point obeying the POSIX errno convention, a
> `strerror` that turns kernel codes into strings, and the
> four shortest pipeline tools running on `FILE *`.

This is the small-looking, far-reaching chapter that flips
every syscall wrapper from the "return `-errno`" convention
introduced in chapter 7 to the "return `-1` and set `errno`"
convention POSIX requires -- and then walks the ~50 caller
sites that printed the negated kernel code.

It also rewrites the four shortest pipeline tools (`cat`,
`wc`, `head`, `tail`) on top of the `FILE *` machinery that
chapter 116b put in place, proving that the libc surface is
real enough for non-trivial apps to live on top of.

## Why this chapter exists

any in-guest C compiler (our `/bin/cc`, plus any future
GCC port), `make`, and pretty much every C program written
since 1979 expects the libc surface to look like:

```c
int fd = open(path, O_RDONLY);
if (fd < 0) {                       /* always -1, never -ENOENT */
    perror("open");                 /* reads errno, calls strerror */
    return 1;
}
```

Up to chapter 116a the convention was the opposite: `open()` returned
`-ENOENT` on failure, and the value was *also* mirrored into
`errno`. That dual-channel design was a deliberate
backward-compat shim — chapter 7 to chapter 115 had ~99
caller sites of the shape `printf("err=%d\n", -fd)`, and the
errno chapter (116a) made the new path opt-in.

Chapter 116d removes the shim:

- `__svc_check` (the inline guard inside every `_svc{0..6}`
  helper) now writes `errno` and clamps the return to `-1`.
- `cat`, `wc`, `head`, `tail` move to `FILE *`. The four
  files shrink by ~200 lines collectively and stop
  reimplementing buffered reads each time.
- `strerror` lands as a single-table lookup, so error
  messages stop being "errno=110" and start being
  "Connection timed out".

The shim was useful because it let us roll the errno table
in (116a), the FILE-* layer (116b), and the env arena (116c)
without touching every existing app the same week. By the
time chapter 117 needs `stat` / `dirent` / `getcwd` shaped
the POSIX way, the convention has to be in place — those
callers will assume `-1 + errno` from day one.

## What this ships

| Path | Change |
|---|---|
| [userspace/libc/syscall.h](../../../userspace/libc/syscall.h) | `__svc_check` now returns `-1` on any negative kernel rc and writes `errno`. |
| [userspace/libc/errno.h](../../../userspace/libc/errno.h) | Adds `static inline const char *strerror(int e)` — ~40-entry switch. |
| [userspace/cat/cat.c](../../../userspace/cat/cat.c) | Rewritten on `fopen`/`fread`/`fwrite`/`ferror`/`fclose`. |
| [userspace/wc/wc.c](../../../userspace/wc/wc.c) | Same pattern. `stdin` path uses `FILE *stdin`. |
| [userspace/head/head.c](../../../userspace/head/head.c) | Same pattern. |
| [userspace/tail/tail.c](../../../userspace/tail/tail.c) | Same pattern, plus uses `fseek(f, off, SEEK_SET)` instead of pass-2 read-and-discard. |
| [userspace/sh/sh.c](../../../userspace/sh/sh.c) | All `putd(-rc)` error sites switched to `putd(errno)`. |
| [userspace/init/init.c](../../../userspace/init/init.c) | All 11 spawn-failure `putd(-tid)` sites switched to `putd(errno)`. |
| 15+ other `.c` files | `printf("...errno=%d", -rc)` → `printf("...errno=%d", errno)` across `cssparse.c`, `pngdec.c`, `htmltok.c`, `layout.c`, `htmldom.c`, `grep.c`, `browser.c`, `httpsd.c`, `cookies.c`, `httpd.c`. |
| [userspace/fontd/fontd.c](../../../userspace/fontd/fontd.c) | `rc == -EINTR` → `rc == -1 && errno == EINTR`. |
| [userspace/libgui/wmclient.c](../../../userspace/libgui/wmclient.c) | Same shape for `ENOENT`. |
| [userspace/wsd/wsd.c](../../../userspace/wsd/wsd.c) | Same shape for `EAGAIN` and `EINTR` (2 sites). |
| [userspace/mixtest/mixtest.c](../../../userspace/mixtest/mixtest.c) | `rc == -EBUSY_RC` → `rc != -1 \|\| errno != EBUSY_RC` (3 sites). |
| [userspace/errnotest/errnotest.c](../../../userspace/errnotest/errnotest.c) | Header comment updated to describe the new convention. |
| [scripts/test_libc_errno.py](../../../scripts/test_libc_errno.py) | Final assertion flipped from `rc == -2` to `rc == -1`. |
| [scripts/test_userfs_timeout.py](../../../scripts/test_userfs_timeout.py) | Match-strings changed from `errno=110` to `Connection timed out` (cat now uses `strerror`). |

## The `__svc_check` flip

Before:

```c
static inline long __svc_check(long r) {
    if (r < 0) {
        __errno_value = (int)(-r);
        /* return r unchanged — callers still see -ENOENT etc. */
    }
    return r;
}
```

After:

```c
static inline long __svc_check(long r) {
    if (r < 0) {
        __errno_value = (int)(-r);
        return -1;
    }
    return r;
}
```

That's the entire chapter, semantics-wise. The
single-line clamp turns every wrapper into a POSIX
function. The `errno` plumbing from 116a does the rest.

### MAP_FAILED compatibility

`mmap()` returns `void *`; failure is `(void *)-1`, the
constant POSIX calls `MAP_FAILED`. Our wrapper has always
returned `(void *)(intptr_t)__svc_check(...)`, so when
`__svc_check` returns `-1` the cast gives us
`(void *)-1` — exactly the value callers already compared
against. Nothing in `userspace/` needed to change for
`mmap`. The same logic covers `sbrk`.

## `strerror` as a switch

```c
static inline const char *strerror(int e) {
    switch (e) {
    case 0:        return "Success";
    case EPERM:    return "Operation not permitted";
    case ENOENT:   return "No such file or directory";
    case EINTR:    return "Interrupted system call";
    case EIO:      return "Input/output error";
    case EBADF:    return "Bad file descriptor";
    /* ...40-ish more codes... */
    case ETIMEDOUT:return "Connection timed out";
    default:       return "Unknown error";
    }
}
```

At our scale (~40 codes) a switch is smaller and faster
than the traditional `sys_errlist[]` indirection, and
since `errno.h` is header-only every binary that needs
`strerror` gets its own copy of the table — no static
data lives anywhere else.

## The four-app port

`cat` is the smallest, so the diff tells the story:

Before (chapter 115 shape):

```c
int fd = open(argv[i], 0);
if (fd < 0) {
    write(1, "cat: cannot open ", 17);
    write(1, argv[i], strlen(argv[i]));
    write(1, " (errno=", 8);
    putd(-fd);
    write(1, ")\n", 2);
    continue;
}
char buf[4096];
long n;
while ((n = read(fd, buf, sizeof(buf))) > 0) {
    write(1, buf, (size_t)n);
}
close(fd);
```

After (this chapter):

```c
FILE *f = fopen(argv[i], "r");
if (!f) {
    fprintf(stderr, "cat: cannot open %s: %s\n",
            argv[i], strerror(errno));
    continue;
}
char buf[4096];
size_t n;
while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    fwrite(buf, 1, n, stdout);
}
if (ferror(f))
    fprintf(stderr, "cat: read error: %s\n", strerror(errno));
fclose(f);
```

`wc` / `head` / `tail` follow the same shape; `tail` also
swaps its pass-2 read-and-discard for `fseek(f, off,
SEEK_SET)`, which is the first non-test use of `SEEK_SET`
shipped by chapter 116b.

## Caller migrations — the long tail

The audit was a grep for printable patterns:

```
grep -nE '(-rc|-fd|-cfd|-urc|-n)\)|errno=%d' userspace/**/*.c
```

51 sites in 20 files. The change is mechanical: where
the printf passed `-rc`, pass `errno` instead. Each
wrapper site already set `errno` correctly via the new
`__svc_check`. Where a *comparison* used the old shape
(`if (rc == -EBADF)`), the new shape becomes
`if (rc == -1 && errno == EBADF)`.

Three classes of sites were intentionally **not**
touched:

1. **Userfs daemon return paths.** Files like
   `userspace/clipboardd/clipboardd.c`,
   `userspace/echofs/echofs.c`,
   `userspace/procd/procd.c`,
   `userspace/libfs/userfs.c` return literal `-2 / -9 /
   -22` etc. as part of the userfs protocol — those
   values flow over the IPC channel, not through
   `__svc_check`. They are protocol numbers and stay as
   they are.
2. **`mixtest.c::EBUSY_RC`.** Local alias for the
   protocol number on the mix-audio channel; kept as a
   macro, only the assertion shape changed.
3. **`notepad.c`.** A 700-line GUI app. Porting it to
   `FILE *` was deferred to keep this chapter focused;
   the existing raw-syscall path keeps working because
   the wrappers still return a sensible `-1` and the
   notepad save code already checks `< 0`.

## Test plan

No new test binary — the convention flip is exercised
by every existing regression. The check is the entire
sweep:

| Test | Before | After |
|---|---|---|
| `test_libc_errno` | 9 PASS / 0 FAIL | 9 PASS / 0 FAIL (assertion updated for new rc) |
| `test_libc_stdio` | 9 PASS / 0 FAIL | 9 PASS / 0 FAIL |
| `test_libc_env` | 11 PASS / 0 FAIL | 11 PASS / 0 FAIL |
| `test_boot_to_desktop` | ALL PASSED | ALL PASSED |
| `test_userfs_echo` | 7/0 | 7/0 |
| `test_clipboard` | ALL PASSED | ALL PASSED |
| `test_mount_ro` | 12/0 | 12/0 |
| `test_userfs_timeout` | 6/0 | 6/0 (match strings updated for strerror) |
| `test_httpd_forward` | 12/0 | 12/0 |
| `test_browser_proxy` | 10/0 | 10/0 |
| `test_cow` | PASS | PASS |
| `test_fork_exec` | PASS | PASS |
| `test_busy_on_mix` | 2/0 | 2/0 |
| `test_clone_files` | PASS | PASS |
| `test_directories` | 13/0 | 13/0 |

Two harness files (`test_libc_errno.py`,
`test_userfs_timeout.py`) had embedded assertions about
the *old* error format and needed one-line updates;
neither was a regression, just an updated expectation.

## Applied to

- **Existing apps modified to use the feature**:
  `cat`, `wc`, `head`, `tail` rewritten on `FILE *`;
  `sh`, `init`, `httpd`, `httpsd`, `browser`,
  `cssparse`, `pngdec`, `htmltok`, `htmldom`,
  `layout`, `grep`, `fontd`, `wmclient`, `wsd`,
  `cookies` migrated to read `errno` for error
  reporting; `mixtest` assertions reshaped.
- **New apps**: none — the four FILE-* ports replace
  reimplementations of buffered reads.
- **Existing test scripts upgraded**:
  `test_libc_errno.py`, `test_userfs_timeout.py`.
- **New test scripts**: none — every existing test
  exercises the new convention through normal use.

## Lessons learned

- The convention shim was the right call. Without
  116a's "negative-rc *and* errno" dual-channel design,
  this chapter would have been a 200-file diff that
  broke every existing app in one commit. With it,
  each app only needed the printf flip — never a
  semantic change.
- `strerror` as a header-only switch is fine. The fear
  is "multiple copies of a 40-case table per binary".
  Reality: each table is ~500 bytes of `.rodata`, and
  the .text shrinks because callers stop hand-formatting
  numeric errnos.
- The userfs-daemon protocol numbers (`return -2;` for
  ENOENT etc.) read like errnos but aren't. Keep them
  visually distinct from real syscall returns by
  commenting the daemon side and never running them
  through `__svc_check`.
- Direction-switching FILE-* I/O is the trap that
  surfaced in `tail`: a `FILE *` that just finished
  writing must `lseek` (or `fflush` + buffer reset)
  before reading. `fseek(f, off, SEEK_SET)` is the
  primitive that makes pass-2 read-and-discard
  unnecessary.

## Next

[Chapter 117 — libc, part 2: `stat`, `fcntl`, `dirent`,
`getcwd`](117-libc-stat-fcntl-dirent.md). The next
batch of POSIX functions a real compiler's build process expects.
That chapter adds `SYS_STAT = 102` and `SYS_FSTAT =
103` and lets `find` / `ls -l` shrink to a few lines
each.

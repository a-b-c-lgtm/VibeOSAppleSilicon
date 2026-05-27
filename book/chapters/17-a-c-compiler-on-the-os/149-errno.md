# Chapter 149 — Step 1: errno, populated by every syscall wrapper

Chapter 148 promises a POSIX-shaped libc: real `FILE *`, real
`errno`, real `setenv`. That's enough work that breaking it
into three landings — `149` errno, `150` stdio, `151` env
— keeps each one a sweep-green commit instead of a giant
all-or-nothing PR. This sub-chapter is the foundation: a real
`errno` slot, populated automatically on every failing
syscall, *without* changing the existing return-value contract
that ~99 wrappers and ~20 apps already depend on.

## Why split this out

The chapter 148 strategy doc calls for the wrappers to switch
from "return a negative kernel errno" to "return `-1` and set
`errno`". That's the right POSIX shape, and any future
compiler port (Part XVIII's GCC, or another backend) will
expect exactly that contract.
need it eventually, but doing it in one chunk means:

- Touching every one of ~99 `static inline` wrappers in
  [userspace/libc/syscall.h](../../../userspace/libc/syscall.h).
- Auditing every userspace consumer for the `-rc` pattern.
  Twenty call sites print `errno=%d` via `-fd` today; those
  all need to be re-spelled `errno`.
- Then re-running the full sweep, hoping nothing subtle
  changed.

So 149 does the half that's reversible and risk-free: errno
becomes available alongside the existing convention. Every
existing app keeps compiling and behaving identically. Code
that wants POSIX `errno` semantics — the upcoming `FILE *`
layer, the upcoming `getenv` rewrite, and any future
compiler-port component that
ends up running in-guest — gets them today. The actual flip
from "return negative" to "return `-1`" is deferred to 152,
which is a pure migration with no new behaviour.

## What got added

Three small pieces of code and one regression test:

1. [userspace/libc/errno.h](../../../userspace/libc/errno.h) —
   declares the global `__errno_value`, defines the standard
   `errno` macro, and lists the `E*` constants that match
   `kernel/core/vfs.h`. Values are pinned to Linux's
   `<errno.h>` so a future compiler port doesn't need a translation
   table.
2. A new `.bss` slot in
   [userspace/crt/crt0.S](../../../userspace/crt/crt0.S) for
   `__errno_value`. Putting the storage in `crt0` (instead of
   a fresh `errno.o`) means it ships in every binary
   automatically, without threading an `ERRNO_OBJ` through
   the ~60 `*_OBJS` lists in the Makefile.
3. A `__svc_check(long r)` helper added to
   [userspace/libc/syscall.h](../../../userspace/libc/syscall.h)
   between the `WNOHANG` definition and the `_svc0` family.
   Each `_svc{0..6}` wrapper now returns
   `__svc_check(x0)` instead of bare `x0`. `__svc_check`
   sets `errno = -r` when `r < 0` and returns `r` unchanged.
4. [userspace/errnotest/errnotest.c](../../../userspace/errnotest/errnotest.c)
   and
   [scripts/test_libc_errno.py](../../../scripts/test_libc_errno.py)
   — exercise four cases and assert errno is populated on
   failure but not stomped on success.

## How `__svc_check` works

The whole mechanism fits in five lines:

```c
extern int __errno_value;

static inline long __svc_check(long r)
{
    if (r < 0)
        __errno_value = (int)(-r);
    return r;
}
```

Three things to notice:

- **The return value is unchanged.** Every existing `open()`
  caller that does `if (fd < 0) printf("...errno=%d\n", -fd)`
  still works.  Every caller that switches to checking
  `errno` after the call also works.  Two conventions, one
  call, no breakage.

- **Success leaves errno alone.** POSIX says successful
  syscalls must not clobber a stale errno; this contract
  is what lets idioms like `errno = 0; v = strtol(...); if
  (errno) ...` work.  The four-line if-guard above does
  exactly that, and `test_libc_errno.py`'s fourth assertion
  pins it down.

- **Pointer-returning syscalls don't false-positive.**
  `sbrk` and `mmap` return user pointers in `x0`.  On
  aarch64 user VA is well below 2^48 so the sign bit is
  always clear on success; only `MAP_FAILED == (void *)-1`
  is negative, and callers already special-case that.  No
  pointer return is mistaken for an error.

## Why the slot lives in `crt0.S`

The obvious choice was a tiny `userspace/libc/errno.c` with
`int __errno_value = 0;`. That works but requires every
binary's `*_OBJS` list in [Makefile](../../../Makefile) to
gain an `ERRNO_OBJ` entry — ~60 edits — and any binary that
forgets gets an `undefined reference to '__errno_value'`
link error.

Putting the slot in `crt0.S`:

```asm
.section .bss.__errno_value, "aw", %nobits
.global __errno_value
.balign 4
.type __errno_value, %object
.size __errno_value, 4
__errno_value:
    .skip 4
```

… piggy-backs on the existing crt0.o that every binary
already links. Zero Makefile churn, zero risk of forgetting.
The kernel hands out zeroed pages, so the implicit `0`
initial value is correct without any startup code.

## What the test asserts

`errnotest` runs four assertions; the harness checks each:

| Case             | Returned rc | Expected errno | Why it matters                |
|------------------|------------:|---------------:|-------------------------------|
| `open("/nope")`  |          -2 |              2 | ENOENT reaches userspace      |
| `close(-1)`      |          -9 |              9 | EBADF reaches userspace       |
| `read(-1, ...)`  |          -9 |              9 | EBADF on a different syscall  |
| `getpid()`       |        > 0  |             42 | success preserves stale errno |

All four pass on first run. The `rc == -2 / -9` checks are
the backward-compat guarantee: the wrappers still return what
they always returned. The `errno == ...` checks are the new
POSIX contract.

## What this unlocks

Per the `apps-must-use-features` discipline, every kernel/
runtime feature added needs an existing-app rewrite or a new
app that exercises it. For 149:

- **New app:** [userspace/errnotest/errnotest.c](../../../userspace/errnotest/errnotest.c)
  is the user-visible smoke test. Tiny but real — anyone can
  type `errnotest` at the shell and see the four lines.

- **Existing-app rewrite, deferred:** the 20-odd
  `printf("...errno=%d", -fd)` call sites are *legal POSIX
  consumers today* (they get the same number both via `-fd`
  and via `errno`), but they remain on the old idiom.  When
  152 flips the wrappers to `-1 + errno`, these sites
  become the migration target — each one shrinks from
  `printf("...errno=%d", -fd)` to `printf("...errno=%d",
  errno)` and silently picks up the right number.

- **Upcoming chapters:** 150's `FILE *` layer reads
  `errno` to populate `ferror`; 151's `getenv` rewrite
  uses `EINVAL` on bad input. Without this sub-chapter
  neither layer would have a real errno to talk to.

## What is *not* done yet (the 150/151/152 work)

To keep this commit honest, here's what stayed unchanged:

- **No per-thread errno.** `__errno_value` is process-global.
  Single-threaded apps see correct POSIX semantics; the only
  threaded apps (`threadtest`, the browser's parser thread)
  don't check errno today. Per-thread errno is a future
  upgrade — chapter 92's TLS slot is the obvious home for it
  — but adding it now would require crt0 to set `TPIDR_EL0`
  for the main thread (today it's left as the kernel's
  default 0), and that's a larger change than this
  sub-chapter wants to carry.
- **No wrapper convention flip.** Every wrapper still returns
  `-errno` on failure. The flip is 152's job.
- **No `FILE *`.** That's 150.
- **No `setenv` / `putenv`.** That's 151.
- **No `strerror`.** A trivial helper but unneeded until 152
  switches `perror`-style sites away from the `-fd` idiom.

## Side-effects for next chapters

- 150 can now write `f->error = errno;` after a failing
  `read()` / `write()` on a `FILE *`, instead of inventing
  its own private error code.
- The chapter 148 stub's "thread-local errno" claim is the
  last loose thread between this sub-chapter and the full
  POSIX libc shape. Re-reading the parent chapter's stub:
  it commits to thread-local, and the implementation here is
  process-global.
  That's an honest gap; the parent chapter's index entry
  will need a follow-up note when the upgrade lands.

## Reading flow

```
115 (strategy)
└─ 116 (libc: stdio, errno, env)
   ├─ 149  errno foundation  ← this chapter
   ├─ 150  FILE * + buffered I/O
   ├─ 151  setenv / putenv / environ arena
   └─ 152  -1 + errno convention flip + app migration
```

After 152, chapter 153 (stat / fcntl / dirent / getcwd)
becomes the next milestone; with errno and FILE * both in
place, the libc surface area is finally what an upstream
program expects to find on a POSIX-shaped OS.

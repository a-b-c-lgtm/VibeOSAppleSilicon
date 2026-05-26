# Chapter 116c — `environ[]`, getenv, setenv, unsetenv, putenv

**Status:** Shipped. The POSIX env arena that `make`, any in-guest
C compiler, and every other UNIX-shaped tool expects is now in
place. Substep of
[Chapter 116](116-libc-stdio-and-env.md); follows
[Chapter 116b](116b-stdio.md).

## Why this needs its own chapter

The kernel has carried `SYS_GETENV` / `SYS_SETENV` / `SYS_UNSETENV`
/ `SYS_GETENV_ALL` since chapter 24, but their userspace wrappers
in [`userspace/libc/syscall.h`](../../../userspace/libc/syscall.h)
are not POSIX-shaped:

```c
int getenv(const char *name, char *buf, int buflen);   // copy-out
int setenv(const char *name, const char *val);          // no overwrite flag
```

That signature works for the shell but doesn't work for a real C
compiler or `make`. A toolchain calls C-library `getenv` and expects a
pointer that stays valid until the next mutation against the same
key (POSIX §8.2). Returning a kernel pointer across the syscall
boundary is impossible, so we need a **userspace arena** that
caches the env block and hands out stable pointers into it.

The other axis is the overwrite flag. POSIX `setenv(name, val, 0)`
must leave an existing entry alone; the old two-arg wrapper would
silently clobber it. The whole point of the env *arena* shape is
that the libc owns the storage and can enforce these semantics
without round-tripping to the kernel on every read.

## What this chapter ships

| File | Purpose |
| --- | --- |
| [`userspace/libc/env.h`](../../../userspace/libc/env.h) (new, ~270 lines) | Header-only POSIX env arena: `environ`, `getenv`, `setenv`, `unsetenv`, `putenv`, `clearenv` |
| [`userspace/libc/syscall.h`](../../../userspace/libc/syscall.h) (modified) | Renamed the bare-syscall wrappers `getenv`/`setenv`/`unsetenv`/`getenv_all` to `__sys_getenv`/`__sys_setenv`/`__sys_unsetenv`/`__sys_getenv_all` so env.h owns the POSIX names |
| [`userspace/init/init.c`](../../../userspace/init/init.c), [`userspace/sh/sh.c`](../../../userspace/sh/sh.c), [`userspace/env/env.c`](../../../userspace/env/env.c), [`userspace/proxytest/proxytest.c`](../../../userspace/proxytest/proxytest.c), [`userspace/httpd/httpd.c`](../../../userspace/httpd/httpd.c), [`userspace/browser/browser.c`](../../../userspace/browser/browser.c) (modified) | Migrated all 6 call sites to the new POSIX surface — see *Applied to* below |
| [`userspace/envtest/envtest.c`](../../../userspace/envtest/envtest.c) (new) | 10-test smoke binary |
| [`scripts/test_libc_env.py`](../../../scripts/test_libc_env.py) (new) | Harness, 11 assertions |

## The shape of the arena

```c
static char  g_env_arena[16 * 1024];      /* packed "KEY=VAL\0" records */
static size_t g_env_arena_used;
static char *g_env_envv[256 + 1];          /* pointers into the arena */
static int   g_env_count;

char **environ = g_env_envv;
```

`environ` is a real `char **` aliased to the entry-pointer array.
Every record in the arena is a NUL-terminated `"KEY=VAL"` string;
`g_env_envv[i]` points at the start of the i'th record, and
`g_env_envv[g_env_count] == NULL` so callers can walk it the
canonical UNIX way:

```c
for (char **p = environ; *p; p++) puts(*p);
```

On first call to any env.h function, `_env_init` pulls the entire
env blob from the kernel via `__sys_getenv_all` (one syscall) and
splits the result into arena entries. The kernel-side env table
remains the source of truth across processes; the arena is a
process-local cache.

## Mutation semantics

| Call | What it does |
| --- | --- |
| `getenv(name)` | Lazy-init, look up `name` in `environ[]`, return `pointer + strlen(name) + 1` (i.e. into the arena), or NULL |
| `setenv(name, val, 1)` | Replace existing entry (in place if the new value fits in the old slot, else allocate fresh in the arena and rewrite the `environ[]` pointer). Always write through via `__sys_setenv` |
| `setenv(name, val, 0)` | If `name` already present, return 0 immediately without touching either cache or kernel. Otherwise behave like overwrite=1 |
| `unsetenv(name)` | Drop the entry from `environ[]` (compaction by sliding the tail down). Write through via `__sys_unsetenv` so child processes also lose it |
| `putenv(str)` | Split `str` at the first `'='`, treat the prefix as the key, *take ownership* of the caller's storage (per POSIX), and install it in `environ[]`. The caller MUST pass a writable, lifetime-stable buffer (the test uses `static char kv[] = "MUTEX=42"`) |
| `clearenv()` | Snapshot keys, unsetenv each one (so children also lose them), reset the arena |

`setenv("KEY=NAME", ...)` (an `=` in the key) returns -1 with
`errno = EINVAL`. The kernel-side blob can never carry such a
record, so we reject it before it touches the arena.

## Why rename the bare-syscall wrappers

The four existing wrappers in `syscall.h` had the names POSIX
reserves. Letting env.h define functions with the same names would
either silently shadow the wrappers (if env.h won the include
order) or refuse to compile. The clean fix is to rename:

```c
// syscall.h (chapter 116c rename)
static inline int __sys_getenv(const char *name, char *buf, int buflen);
static inline int __sys_setenv(const char *name, const char *val);
static inline int __sys_unsetenv(const char *name);
static inline int __sys_getenv_all(char *buf, int buflen);
```

The `__sys_` prefix matches every other thin syscall wrapper that
env.h might want to talk to directly (the env arena itself uses
`__sys_setenv` to write through). All six call sites that had been
using the bare names were migrated as part of this chapter.

## Test plan

`scripts/test_libc_env.py` boots the OS and runs `/bin/envtest`,
which exercises:

| Test | What it covers |
| --- | --- |
| T1 | `getenv("PATH")` returns non-NULL (init seeded `/bin`) |
| T2 | `getenv("ZZZ_NEVER_SET")` returns NULL |
| T3 | `setenv("FOO", "bar", 1)` + `getenv` returns `"bar"` |
| T4 | `setenv("FOO", "baz", 0)` leaves `"bar"` in place |
| T5 | `setenv("FOO", "baz", 1)` overwrites to `"baz"` |
| T6 | `unsetenv("FOO")` clears it; `getenv` returns NULL |
| T7 | `putenv("MUTEX=42")` installs |
| T8 | `environ[]` iteration finds PATH and MUTEX |
| T9 | `setenv` with `=` in the name returns -1 with `errno=EINVAL` |
| T10 | `__sys_getenv("MUTEX", ...)` reflects the write-through |

```
$ python3 scripts/test_libc_env.py
[chapter 116c] env.h POSIX surface (getenv/setenv/unsetenv/putenv/environ)
PASS: getenv(PATH) returns non-NULL (init seeded /bin)
PASS: getenv(ZZZ_NEVER_SET) returns NULL
PASS: setenv(FOO,bar,1) + getenv roundtrip
PASS: setenv(FOO,baz,0) preserves existing value
PASS: setenv(FOO,baz,1) overwrites
PASS: unsetenv(FOO) clears; getenv returns NULL
PASS: putenv(MUTEX=42) installs
PASS: environ[] iteration finds PATH and MUTEX
PASS: setenv with '=' in name -> EINVAL
PASS: kernel-side __sys_getenv reflects write-through
PASS: binary printed ALL PASS marker

11 PASS / 0 FAIL
```

Regression sweep after this chapter (all green): `test_libc_errno`
9/0, `test_libc_stdio` 9/0, `test_boot_to_desktop` 5/0,
`test_userfs_echo` 7/0, `test_clipboard` 6/0, `test_mount_ro`
12/0, `test_userfs_timeout` 6/0, `test_httpd_forward` 12/0,
`test_browser_proxy` ALL PASS.

## Applied to

Per the standing rule that OS features must show up in the apps
that need them, this chapter rewrites every existing call site
in the tree, not just adds a new test:

- **`userspace/sh/sh.c`** — `expand_vars` used to declare a
  256-byte local buffer and copy via the old `getenv(name, buf,
  buflen)` wrapper. Now it calls `getenv(name)` and gets a stable
  arena pointer back; the local buffer goes away. PATH lookup
  copies the returned pointer into a local once so that
  intervening `setenv`s don't move it under us. `setenv` is now
  3-arg. `export` and `unset` error paths now use `errno` instead
  of the old `-rc` convention.
- **`userspace/env/env.c`** — was a 30-line program that called
  `getenv_all` into a 512-byte buffer and walked the resulting
  blob by hand. Now it's *eight lines*: `for (char **p = environ;
  *p; p++) printf("%s\n", *p);`. The 512-byte cap is gone — the
  arena is 16 KiB and `printf` doesn't care.
- **`userspace/init/init.c`** — three `setenv` calls (PATH, USER,
  HOME) migrated to 3-arg.
- **`userspace/proxytest/proxytest.c`** — `setenv("HTTPD_UPSTREAM",
  ...)` migrated to 3-arg.
- **`userspace/httpd/httpd.c`** — `load_upstream_from_env` used to
  call `getenv` into a 256-byte stack buffer; now reads the arena
  pointer directly.
- **`userspace/browser/browser.c`** — `BROWSER_PROXY` and
  `BROWSER_TIMING` reads migrated; one fewer stack buffer per
  read.

The compaction across these six files removed ~120 lines of
caller-side bookkeeping and three different "how big should the
caller buffer be?" judgement calls.

## Lessons learned

- **The rename is the unblocker, not the arena.** Once
  `syscall.h` stops squatting on the POSIX names, the arena code
  itself is straightforward. Keeping the old wrappers around
  under `__sys_*` means the few sites that genuinely want
  zero-allocation copy-out (none today; possibly the toolchain
  later) can still bypass the cache.
- **Lazy init is right.** The arena is empty until first call.
  Binaries that never touch env (most of them) pay nothing —
  no syscall, no 16 KiB writeback. Lazy init also lets the
  `__sys_getenv_all` failure mode (kernel blob too big for the
  arena) be detected once at use time rather than at every
  binary's startup.
- **`putenv` is the POSIX trap.** It really does take ownership
  of the caller's string. Tests must use `static char kv[] = ...`
  (writable, address-stable) rather than a stack array or a
  string literal. The env.h `putenv` does not memcpy. The test
  binary documents this convention inline.
- **Compaction beats free-list.** `unsetenv` slides the tail of
  `environ[]` down by one and decrements `g_env_count`. The arena
  doesn't reclaim the freed bytes — at 16 KiB and a turnover rate
  measured in dozens of mutations per process lifetime, it's
  cheaper to never compact. If a long-running daemon ever
  exhausts the arena, the symptom is `setenv` returning -1 with
  `errno = ENOMEM`; the fix is to bump `ENV_ARENA_SIZE`.

## Next

[Chapter 116d](116d-errno-convention.md) — flip every `__svc_*`
wrapper from "return -errno" to "return -1, set errno", port the
twenty-odd `printf("...errno=%d", -fd)` sites in the existing
apps, and migrate `cat` / `wc` / `head` / `tail` / `notepad` to
the buffered `FILE *` layer in the same pass.

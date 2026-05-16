# Chapter 32 — Per-process working directory: `cd`, `pwd`, and a dynamic prompt

This chapter adds the first piece of *mutable* per-process state
since chapter 26's user heap: a current working directory. The
shell prompt becomes `/$`, `/bin$`, `/mnt$` depending on where
you are, and two new syscalls — `chdir` and `getcwd` — let any
program inspect or change its cwd.

Two design knots have to be untangled to get there:

1. **Where the cwd lives** — on the address space (page tables)
   or on the thread? They're independent, and the answer matters
   for inheritance.
2. **How "directories" exist at all** in our flat namespace,
   given that we don't have `inode->i_mode & S_IFDIR` or
   anything like it.

## What it looks like

```
/$ pwd
/
/$ cd /bin
/bin$ pwd
/bin
/bin$ ls
       477  /motd
       234  /README
       113  /mnt/hello.txt
... etc ...
/bin$ cd /mnt
/mnt$ pwd
/mnt
/mnt$ cd /nope
cd: /nope: no such directory (errno=2)
/mnt$ cd
/$
```

Three valid directories: `/`, `/bin`, `/mnt`. Anything else
returns `-ENOENT`. Bare `cd` (no argument) resets to `/`, the
nearest thing we have to a "home directory".

## Cwd lives on the thread, not the AS

I considered both:

- **On `struct address_space`** — pro: matches the "kernel
  resources owned by the process" model. Con: kernel threads
  have `as = NULL`, and the boot thread (which becomes init)
  legitimately needs a cwd to pass to its first child.
- **On `struct thread`** — pro: every thread already has one,
  including the boot thread. Con: in a real OS, `fork()` shares
  the parent's cwd as one struct that *both* processes mutate
  together until one of them calls `chdir()` (POSIX
  copy-on-write). We don't have fork.

Decision: `struct thread` field. Eight bytes of pointer ambiguity
isn't worth optimizing for a system with no `fork`. The field is
copied byte-for-byte when a child is created in
`thread_create` / `user_thread_create`; that's "inheritance"
for our purposes.

```c
#define THREAD_CWD_MAX 96
struct thread {
    /* ... */
    char cwd[THREAD_CWD_MAX];
};
```

96 bytes. The current namespace tops out at 5 characters
(`/mnt/`); 96 is paranoia for a future hierarchical FS.

## Spawn-time inheritance

The boot thread initializes its cwd to `"/"`. Every child
inherits the spawner's cwd via a literal byte copy:

```c
/* in thread_create / user_thread_create */
if (g_current) {
    size_t i = 0;
    while (g_current->cwd[i] && i < THREAD_CWD_MAX - 1) {
        t->cwd[i] = g_current->cwd[i]; i++;
    }
    t->cwd[i] = '\0';
} else {
    t->cwd[0] = '/'; t->cwd[1] = '\0';
}
```

The `g_current` guard exists because `thread_init` runs before
there's a current thread; the boot thread just gets `"/"`.

## `SYS_CHDIR` and the validation question

The interesting design question for `chdir` is **what to
validate**. Real POSIX validates that the path exists, that it's
a directory, and that the caller has search permission. We have
none of those concepts:

- No "does this path exist" predicate (we only know about leaf
  files in two flat tables).
- No "is this a directory" type bit.
- No permissions at all.

Two reasonable approaches:

**Permissive**: accept any absolute path; let it be the user's
problem when subsequent operations fail. Simplest, least useful
— `cd /total_garbage` "succeeds" and the prompt goes wrong.

**Whitelist**: enumerate the three real directories (`/`, `/mnt`,
`/bin`) and reject everything else. Slightly more code, but the
shell can produce a useful error. Picked this.

```c
if (!s_eq(tmp, "/") && !s_eq(tmp, "/mnt") && !s_eq(tmp, "/bin"))
    return -ENOENT_VFS;
```

When we add a hierarchical FS in a future chapter this becomes
"walk the path, fail at the first missing component, fail at the
first non-directory component". The userspace ABI doesn't change.

### Normalization

Trailing slashes get stripped (`cd /bin/` → `/bin`). The leading
slash is preserved for the root case. We do *not* collapse
`//foo`, resolve `..`, or expand `~` — none of those exist in
the namespace yet. Path normalization is a rabbit hole; we'll
do it properly when we have a hierarchical FS to normalize against.

## `SYS_GETCWD` — POSIX-shaped from day one

```c
static long sys_getcwd(long buf_uptr, long cap)
{
    if (cap <= 0) return -EINVAL_VFS;
    struct thread *t = thread_current();
    size_t need = strlen(t->cwd) + 1;
    if (need > (size_t)cap) return -EINVAL_VFS;
    if (uaccess_check(buf_uptr, need) != 0) return -EFAULT;
    if (copy_to_user(buf_uptr, t->cwd, need) < 0) return -EFAULT;
    return (long)need;
}
```

Returns bytes including NUL — same as POSIX `getcwd(3)` returns
the buffer pointer if it fits. The "include NUL in the count"
choice is so callers don't need a second `strlen` to know how
much was written; the prompt code in the shell uses `n - 1` as
the bytes-without-NUL length passed to `write`.

## The shell prompt

The most user-visible change is the prompt. Old:

```c
static void prompt(void) { write(1, "$ ", 2); }
```

New:

```c
static void prompt(void)
{
    char cwd[96];
    long n = getcwd(cwd, sizeof(cwd));
    if (n > 0) write(1, cwd, (size_t)(n - 1));
    write(1, "$ ", 2);
}
```

Three lines added, instantly clearer interaction. Notice we
don't cache the cwd between iterations — the prompt does a
syscall every time. At 100 ms ticks this is invisible; if a
future kernel makes syscalls non-trivial we'd cache it after
each `cd`.

## Builtins added

```c
if (streq(line, "pwd")) { /* read cwd, write to stdout */ continue; }

if (streq(line, "cd"))            { (void)chdir("/"); continue; }
if (starts_with(line, "cd ")) {
    const char *target = line + 3;
    while (*target == ' ' || *target == '\t') target++;
    int rc = chdir(target);
    if (rc != 0) /* print "cd: <path>: no such directory (errno=N)" */;
    continue;
}
```

The builtins live *before* the `time` and PATH-resolution paths,
matching bash semantics: `time cd /bin` would currently fall
through to the spawn path because `time cd` looks up `cd` as a
binary. Fix is one extra check inside the time path; deferred
because no one expects to time a builtin.

## `cd` doesn't (yet) affect path resolution

This is the deliberate compromise of the chapter. Real shells
resolve relative commands against `cwd + ":" + PATH`. Ours
still hardcodes `/bin/`-prepend for bare names regardless of
cwd. So:

```
/mnt$ ls            # works — resolves to /bin/ls
/mnt$ ./hello       # FAILS — we don't honor "./"
/mnt$ /mnt/hello    # works — absolute path
```

Fixing this requires (a) detecting `./` and `../` prefixes and
(b) deciding whether arbitrary cwd should be in the search path
(probably not, for the same security reason real shells don't
include `.` in `PATH`). Deferred to the env-vars chapter where
we'll do real `PATH` walking too.

## What's still missing

- **`./prog` / `../prog`** — relative-to-cwd execution.
- **`mkdir` / `rmdir`** — meaningless until we have a writable
  hierarchical FS.
- **Per-AS cwd inheritance through real `fork`** — POSIX says
  parent and child share the cwd struct until one mutates it
  (copy-on-write). We just deep-copy at spawn time.
- **`PWD` env variable** — there are no env vars yet.
- **Tab completion of directory names.**

## What changed

```
kernel/core/thread.h        THREAD_CWD_MAX, char cwd[] field
kernel/core/thread.c        cwd init in 3 places (boot/k/user)
kernel/core/syscall.{h,c}   SYS_CHDIR=14, SYS_GETCWD=15;
                            handlers + dispatch cases
userspace/libc/syscall.h    chdir(), getcwd() wrappers
userspace/sh/sh.c           prompt() reads cwd; cd/pwd builtins
                            help text updated
kernel/core/main.c          banner -> milestone 23
```

About 120 lines of new code total. First piece of mutable
per-process state since the user heap; foundation for env vars,
relative paths, and hierarchical filesystems.

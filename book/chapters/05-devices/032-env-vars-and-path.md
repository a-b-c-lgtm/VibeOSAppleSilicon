# Chapter 32 — Environment variables and a real PATH walk

The shell from chapter 29 hardcoded `/bin/` as the only place to
find programs. That works for a tiny system but it's not how
Unix actually does this; the conventional answer is a `PATH`
environment variable that the shell *walks* when resolving a
bare command name. To do that properly we need environment
variables at all, which is what this chapter ships.

Five pieces:

1. A 512-byte `env[]` block on every `struct thread`.
2. Four syscalls: `getenv`, `setenv`, `unsetenv`, `getenv_all`.
3. Inheritance through `spawn` (byte-copy, like `cwd`).
4. A `/bin/env` program and `export` / `unset` shell builtins.
5. PATH-walking inside the shell's `resolve_path()`.

## Storage layout

POSIX environments are conceptually `char **environ` — an array
of `KEY=VALUE` strings terminated by `NULL`. We store something
isomorphic but flatter: one packed buffer of NUL-terminated
entries with an extra trailing NUL marking end-of-list.

```
env[] = "PATH=/bin\0HOME=/\0SHELL=/bin/sh\0\0..."
        ^entry 1     ^entry 2 ^entry 3      ^EOL marker
```

Why a flat blob:

- **Single allocation.** No per-entry pointer storage, no
  fragmentation. The whole environment is `THREAD_ENV_MAX = 512`
  bytes regardless of count.
- **Trivial inheritance.** Forking a process is "copy 512 bytes"
  with no pointer rewriting. (Even with our spawn-not-fork
  model, the cleanliness still helps — see
  `kernel/core/thread.c:user_thread_create`.)
- **Cheap walk.** "while (*p) { use p; p += strlen(p)+1; }" —
  one string-walk per entry, no array bounds checks.

The cost is `setenv("KEY", ...)` is O(blob_size) because
replacing an existing entry needs a memmove of the tail down by
the old entry's length. At 512 bytes and 30-some entries this is
not even close to measurable.

## The four syscalls

All four follow the same pattern:

```c
static long sys_getenv(long key_uptr, long buf_uptr, long cap)
{
    char key[THREAD_ENV_MAX];
    long n = copy_string_from_user(key, key_uptr, sizeof(key));
    if (n < 0) return n;

    struct thread *t = thread_current();
    long off = env_find(t->env, key);
    if (off < 0) return -ENOENT_VFS;
    /* ... copy value out via copy_to_user ... */
}
```

`env_find` does a linear walk:

```c
static long env_find(const char *blob, const char *key)
{
    size_t klen = s_len(key);
    for (size_t off = 0; blob[off]; ) {
        const char *e = &blob[off];
        size_t len   = s_len(e);
        int    match = 1;
        for (size_t i = 0; i < klen; i++)
            if (e[i] != key[i]) { match = 0; break; }
        if (match && e[klen] == '=') return (long)off;
        off += len + 1;
    }
    return -1;
}
```

Linear isn't great theoretically, but with 30-ish entries it's
two cache lines worth of scan and not worth a hash table.

Most subtle piece: `setenv` deletes any old entry of the same
name *before* appending the new one, so PATH=foo followed by
PATH=bar leaves only "PATH=bar". The deletion `memmove`s the
tail down and zero-fills the now-vacant tail bytes, so a later
`getenv_all` can't leak stale data.

## `getenv_all` and `/bin/env`

`getenv_all` exists for one reason: implementing `env(1)` in
userspace without four round-trips for every variable. It just
copies the whole `env[]` blob (used bytes + the trailing
end-of-list NUL):

```c
static long sys_getenv_all(long buf_uptr, long cap)
{
    struct thread *t = thread_current();
    size_t used = env_used(t->env);
    size_t need = used + 1;            /* +1 for end marker */
    if (need > (size_t)cap) return -EINVAL_VFS;
    /* ... copy_to_user ... */
    return (long)need;
}
```

The `env` user program is then 25 lines:

```c
char buf[ENV_BUF];
long n = getenv_all(buf, sizeof(buf));
if (n <= 0) return 0;
const char *p = buf;
while (*p) {
    printf("%s\n", p);
    while (*p) p++;
    p++;
}
```

## Shell PATH walking

`resolve_path` in chapter 29 just prepended `/bin/`. Now it does
a real Unix-style search:

```c
static void resolve_path(const char *name, char *out)
{
    if (name[0] == '/') { /* absolute, copy as-is */ ... return; }

    char path_env[256];
    long n = getenv("PATH", path_env, sizeof(path_env));
    if (n <= 0) { prepend_bin(name, out); return; }

    char *start = path_env;
    char  cand[PATH_MAX];
    while (*start) {
        char *colon = start;
        while (*colon && *colon != ':') colon++;
        /* Build "<entry>/<name>" in cand[]. */
        ... copy entry, ensure trailing '/', append name ...
        if (file_exists(cand)) { /* copy to out, return */ }
        start = (*colon == ':') ? colon + 1 : colon;
    }
    /* No entry matched — let spawn() return a real errno. */
    prepend_bin(name, out);
}
```

`file_exists` is `open(path, 0); if (fd >= 0) close(fd); return ...;`
— wasteful on the open path but correct, and we don't have a
`stat` syscall yet.

### Why no fallback to "current directory"

Real shells don't put `.` in `PATH` for security reasons (an
attacker who can write a file named `ls` in your cwd gets to
execute it). We deliberately replicate that.

### Why `unset PATH` falls back to `/bin`

Useful for getting unstuck — if the user clobbers PATH (which is
easy to do interactively) the shell still works. Real bash falls
back to a builtin search list compiled into the binary;
`/bin/<name>` is our two-line equivalent.

## Inheritance through `spawn`

The thread struct now has both `cwd[]` and `env[]` as inline
buffers; both inherit from `g_current` at thread creation time.
The env-copy loop is slightly subtler than the cwd-copy because
of the two-NULs-mark-end-of-list rule:

```c
size_t i = 0;
int    seen_term = 0;
while (i < THREAD_ENV_MAX) {
    t->env[i] = g_current->env[i];
    if (g_current->env[i] == '\0') {
        if (seen_term) { i++; break; }
        seen_term = 1;
    } else {
        seen_term = 0;
    }
    i++;
}
```

Stop after the second consecutive NUL. Otherwise the code would
copy zeros all the way to `THREAD_ENV_MAX`, which is correct but
wasteful, and would miss the early-exit when the parent's env
was empty.

Belt-and-braces: we explicitly write `\0\0` to the last two
bytes after the loop, in case a malformed parent env without a
proper end marker is somehow present (shouldn't be, but cheap).

## `init` seeds the environment

The boot thread's env starts empty; init seeds it before
launching anything else:

```c
setenv("PATH", "/bin");
setenv("HOME", "/");
setenv("SHELL", "/bin/sh");
```

These three vars then propagate into every subsequent process
(hello, sh, sh's children, etc.) via the spawn-time copy.

## What it actually does

```
/$ env
PATH=/bin
HOME=/
SHELL=/bin/sh
/$ hello
hello from EL0!
/$ export FOO=bar
/$ env
PATH=/bin
HOME=/
SHELL=/bin/sh
FOO=bar
/$ export PATH=/mnt:/bin
/$ ls                  # resolved as /mnt/ls (first PATH entry wins)
... files ...
/$ unset PATH
/$ hello               # falls back to /bin/hello
hello from EL0!
```

The "ls resolved as /mnt/ls" line showing up in the kernel
trace as `[sys_exit] thread '/mnt/ls' exited` proves the PATH
walker is actually finding /mnt first.

## What's still missing

- **Variable substitution.** `echo $HOME` doesn't expand `$HOME`
  yet — the shell never parses `$`. We have `getenv` so adding
  it is a parser tweak.
- **`./prog` and `../prog`.** The shell still ignores the
  cwd-relative case. Plumbing it through `resolve_path` is
  another paragraph of code.
- **`.profile` / `.bashrc`.** Init-time scripts. Need a
  not-yet-written shell-parser that can read commands from a
  file instead of stdin.
- **Per-syscall `EXEC` vs `SPAWN`.** Right now the only way to
  start a program is to spawn a *new* thread; there's no
  in-process replacement (`execve`-like) operation. When we add
  it, the env block survives the call by design.
- **String quoting.** `export GREETING="hello world"` would
  set GREETING to `"hello`. We don't tokenize quotes anywhere.
- **`env -u`, `env KEY=VAL prog`.** GNU extensions. Trivial to
  add when the need shows up.

## What changed

```
kernel/core/thread.h        THREAD_ENV_MAX=512, char env[] field
kernel/core/thread.c        env init in 3 places (boot/k/user)
                            two-NUL-walk inheritance copy
kernel/core/syscall.{h,c}   SYS_{GET,SET,UN}ENV(_ALL), 4 numbers
                            env_find / env_delete_at helpers
userspace/libc/syscall.h    getenv/setenv/unsetenv/getenv_all
userspace/env/env.c         NEW — prints all env vars
userspace/init/init.c       seeds PATH=/bin, HOME=/, SHELL=/bin/sh
userspace/sh/sh.c           export/unset builtins; PATH-walking
                            resolve_path with file_exists check
Makefile                    wires env into disk image
```

About 250 lines of new code. The OS now has a real environment.

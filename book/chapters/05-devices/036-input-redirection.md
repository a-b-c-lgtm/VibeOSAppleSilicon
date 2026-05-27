# Chapter 36 — Input redirection

The shell can dispatch programs and the four new tools (`grep`,
`wc`, `head`, `tail`) accept file arguments. But there's no way
to wire their output to each other yet, and there's no way to
ask `wc` to count something that's not on disk under a known
path. Pipes are the eventual answer; this chapter ships the
*easy half* first — input redirection.

After this chapter:

```
/$ wc < /mnt/poem.txt
14 83 569

/$ grep the < /mnt/poem.txt
... matching lines ...

/$ wc < /no/such/file
[sh] redirect: cannot open /no/such/file
```

The `<` token says "open this file as fd 0 in the spawned
process". The program reads from fd 0 instead of from `open()`,
so any utility that writes a stdin path is composable with any
file the FS knows about.

## What needs to change

Three layers, each small:

1. **Kernel:** a new syscall `SYS_SPAWN_REDIR(path, args,
   stdin_path)` that does what `SYS_SPAWN` does and then
   pre-opens `stdin_path` as fd 0 in the new thread.
2. **Libc:** a wrapper `spawn_redir(...)` over the new syscall.
3. **Shell:** parse `<` from the command line; if found, call
   `spawn_redir` instead of `spawn`. Also: utilities that
   previously *required* a path argument (`wc`, `grep`, `head`)
   now fall back to fd 0 when called with no path.

## The kernel: SYS_SPAWN_REDIR = 20

We don't extend `SYS_SPAWN` because:

- the libc wrapper would change shape (3 args instead of 2)
- any existing caller that passed garbage in x2 would suddenly
  start being interpreted as a stdin path

Append-only is simpler. Number 20.

The handler is structurally identical to `sys_spawn` with two
extras:

```c
/* Validate stdin_path BEFORE we burn the cost of an ELF load. */
{
    const char *bare = NULL;
    if (path_starts_with(stdin_path, "/mnt/")) bare = stdin_path + 5;
    else if (path_starts_with(stdin_path, "/bin/")) bare = stdin_path + 5;
    if (bare) {
        uint32_t s, z;
        if (osfs_lookup(bare, &s, &z) != 0) return -ENOENT_VFS;
    } else {
        const uint8_t *d; size_t z;
        if (vfs_lookup(stdin_path, &d, &z) != 0) return -ENOENT_VFS;
    }
}

/* ...same body as sys_spawn... */

/* Install fd 0 from the (already-validated) path. */
(void)vfs_open_into(t, 0, stdin_path, 0);
```

`vfs_open_into(t, fd, path, flags)` is a new variant of
`vfs_open` that writes into a *specific* thread's *specific*
slot, overwriting the existing entry. This is the only kernel
function that can safely populate a not-yet-running thread's
fd table; ordinary `vfs_open` always operates on
`thread_current()`.

We validate first so we don't load a giant ELF, allocate an
address space, walk the program headers, and only *then*
discover the stdin path is bogus. Probing first costs one OSFS
lookup (one disk hit, maybe cached).

The post-create install can technically fail (in theory) but
the lookup tables are global and monotonic, so having
validated a moment ago, the second lookup is guaranteed to
succeed. We trust it.

If `stdin_path` is empty, `sys_spawn_redir` degenerates to
`sys_spawn`. That's so the libc wrapper can pass a NULL/"" for
the redirect path without the kernel needing two code paths.

### Why fd 0 specifically

POSIX says `<` redirects fd 0 (stdin), `>` redirects fd 1
(stdout). We do `<` only because:

- our FS is read-only — no `>` target to write into yet
- the grep/wc/head/tail tools immediately benefit
- the change is small enough to land in a single chapter

Output redirection lands when we have a writable filesystem.

## The shell: parsing `<`

The `<` token can appear anywhere in the command line and can
be glued to the next word (`cmd<file`) or separated by
whitespace (`cmd < file`). The next whitespace-bounded word is
the path; both the `<` and the path are removed from what the
spawned program sees as argv.

```c
const char *redir_in = 0;
char redir_path[PATH_MAX];
{
    char *p = cmd;
    while (*p) {
        if (*p == '<') {
            char *lt = p;
            char *q  = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            char *ps = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '<') q++;
            char *pe = q;
            /* ... copy path, splice out [lt..pe), break ... */
        } else { p++; }
    }
}
```

After splicing, the command line has had `< /mnt/poem.txt`
removed and the surrounding whitespace collapsed. Then the
existing tokenizer runs over what's left as if no redirect had
been there.

### Limitation: `<` is recognized post-expansion

We parse `<` *after* `expand_vars` has already consumed quotes.
That means `echo '<'` doesn't produce a literal `<` — the
quotes are already gone, and the `<` looks like a redirect
operator. This is wrong by POSIX (which says quotes suppress
redirection too) but we ship it anyway because:

- the workaround is "use `\\<`" — but that also gets consumed by
  `expand_vars`...
- ...so honest fix is to recognize `<` *before* quote expansion
  and snip it out, *then* expand.

The honest fix lands when we have a unified tokenizer that
preserves quote state through to redirect parsing. For now,
shipping the 90% case beats waiting for the 100% case.

## The tools: optional fd 0 fallback

`wc`, `grep`, and `head` previously *required* a `PATH`
argument:

```c
if (argc < 2 || !argv[1]) {
    printf("usage: wc PATH\n");
    return 1;
}
```

Now they treat a missing path as "read from fd 0":

```c
int fd;
const char *path;
if (argc < 2 || !argv[1]) {
    fd = 0;
    path = "";
} else {
    path = argv[1];
    fd = open(path, 0);
    if (fd < 0) { ... }
}
/* ...read loop unchanged... */
if (fd != 0) close(fd);
```

`tail` is left PATH-only because it does a two-pass read with a
re-open, and you can't reopen fd 0. (Future fix: buffer the
full stdin into a malloc'd block, then index lines.)

When fd 0 is the console (no `<` in front of the command), the
tools just block forever waiting for input. That's the same
behavior as POSIX `wc` with no args — type your stuff, then
Ctrl-D. We don't have Ctrl-D yet; you'd have to kill the QEMU
process. The intended use is always with a `<` prefix.

## Verification

```
/$ wc < /mnt/poem.txt
14 83 569

/$ grep the < /mnt/poem.txt
If you can read this through the kernel's OSFS-1 mount,
the chain works:
... etc ...

/$ head -3 < /mnt/poem.txt
If you can read this through the kernel's OSFS-1 mount,
the chain works:

/$ wc < /no/such/file
[sh] redirect: cannot open /no/such/file
```

The error path is also better-typed than the old "command not
found" generic — the shell knows the failure mode is "redirect
failed" because it's the one who asked for the redirect.

## What this unlocks

`<` is half of pipes. The other half — `>` — needs a writable
filesystem. The remaining half — `|` — needs:

- `THREAD_BLOCKED` state (blocked on pipe-empty / pipe-full)
- in-kernel pipe object with two ends
- `dup2(oldfd, newfd)` to wire pipe ends to stdin/stdout
- per-thread fd table inheritance through `spawn`
- shell parsing for `cmd1 | cmd2` that calls `pipe()` then
  spawns both children with the right fd 0 / fd 1 wired

That's its own multi-chapter arc. Today's chapter is the easy
half: one new syscall, ~50 lines of kernel, ~50 lines of
shell, ~40 lines split across three tools.

## What changed

```
kernel/core/syscall.h     +SYS_SPAWN_REDIR=20
kernel/core/syscall.c     +sys_spawn_redir handler (~70 lines)
                          +#include "osfs.h"
kernel/core/vfs.h         +vfs_open_into(t, fd, name, flags)
kernel/core/vfs.c         +vfs_open_into impl (~30 lines)
userspace/libc/syscall.h  +SYS_SPAWN_REDIR=20, +spawn_redir wrapper
userspace/sh/sh.c         +`<` parser, dispatch through spawn_redir,
                           dual-message error reporting
userspace/wc/wc.c         fd 0 fallback when argc<2
userspace/grep/grep.c     fd 0 fallback when argc<3
userspace/head/head.c     fd 0 fallback when no path
```

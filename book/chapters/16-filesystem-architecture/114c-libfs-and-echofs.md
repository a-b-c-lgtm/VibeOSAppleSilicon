# Chapter 114c — Step 3: `libfs` and `/bin/echofs`

Steps 1 and 2 ([114a](114a-kernel-userfs-module.md),
[114b](114b-sys-mount-umount.md)) put the kernel half in
place: a process can now call `mount_kernel("/echo", fds)`,
receive a request fd and a reply fd, and every other process
that opens `/echo/whatever` lands in `g_userfs_ops.open`
against that channel. What the kernel can't do is *serve*
those requests — that's a daemon's job. This chapter writes
the smallest interesting daemon, plus the library every
future userfs daemon will be built on.

By the end of the chapter the regression sweep has one new
entry, `scripts/test_userfs_echo.py`, that boots the OS,
spawns `/bin/echofs &`, mounts `/echo`, and exercises the
whole RPC end to end with `cat /echo/hello`, write-then-read
on `/echo/buf`, and `ls /echo`. If that test goes green,
chapters 114d and 114e (real daemons: clipboardd, procd)
have nothing new to debug at the protocol layer.

## What lives where

Two new source trees under `userspace/`:

- [`userspace/libfs/userfs.h`](../../../userspace/libfs/userfs.h) +
  [`userspace/libfs/userfs.c`](../../../userspace/libfs/userfs.c) —
  the boilerplate. A daemon fills in a `struct
  userfs_handler` with up to eight callbacks and calls
  `userfs_serve("/some/prefix", &h)`. The library handles
  every wire detail.
- [`userspace/echofs/echofs.c`](../../../userspace/echofs/echofs.c) —
  the smallest interesting daemon. Mounts `/echo`, serves
  three files (`hello`, `buf`, `echo`), exits when the
  channel closes.

Plus three Makefile entries — a `LIBFS_OBJ` group, an
`ECHOFS_*` group, and the disk-image inclusion line so the
binary actually shows up in `/bin/`.

## What `libfs` is for

The temptation when writing the second daemon (clipboardd,
chapter 114d) would be to copy the protocol decode loop out
of `echofs.c` verbatim. Two daemons later you've got three
slightly-different decoders and you're chasing the same bug
in two of them. Pulling the loop into a library on day one
is the cheap path.

The interface is a struct of function pointers:

```c
struct userfs_handler {
    int (*on_open)   (void *ud, const char *path, int flags,
                      uint32_t *handle_out);
    int (*on_read)   (void *ud, uint32_t handle, uint64_t off,
                      void *buf, uint32_t cap);
    int (*on_write)  (void *ud, uint32_t handle, uint64_t off,
                      const void *buf, uint32_t n);
    int (*on_close)  (void *ud, uint32_t handle);
    int (*on_listdir)(void *ud, const char *path, int idx,
                      char *name, uint32_t cap, uint32_t *type);
    int (*on_unlink) (void *ud, const char *path);
    int (*on_mkdir)  (void *ud, const char *path);
    int (*on_is_dir) (void *ud, const char *path);
    void *userdata;
};
```

Any callback can be NULL; the library replies with `-ENOSYS`
(38) for unhandled ops. `userdata` is the cookie the daemon
gets back on every callback — a `clipboardd` would point it
at its slot table, a `procd` at its thread-snapshot cache.

The single entry point is:

```c
int userfs_serve(const char *prefix, const struct userfs_handler *h);
```

It calls `mount_kernel`, runs the serve loop forever, and
returns when the kernel tears the channel down (umount, or
the mount being kicked because of a malformed message).

## The serve loop

The whole loop fits on one screen:

```c
for (;;) {
    struct p9_msg req;
    if (read_full(req_fd, &req, sizeof req) < 0) break;

    if (req.length > P9_MAX_PAYLOAD) {
        (void)drain(req_fd, req.length);
        (void)send_reply(rsp_fd, req.op, req.tag, 0, 0, -22, NULL, 0);
        continue;
    }
    if (req.length > 0) {
        if (read_full(req_fd, req_payload, req.length) < 0) break;
    }
    req_payload[req.length] = 0;  /* NUL-terminate paths in place */
    const char *path = (const char *)req_payload;

    switch (req.op) {
    case P9_OP_OPEN:    /* ...call on_open, send reply... */
    case P9_OP_READ:    /* ...call on_read, send reply with payload... */
    case P9_OP_WRITE:   /* ...call on_write, send reply... */
    /* ... etc ... */
    }
}
```

Three small subtleties:

- **NUL-termination in place.** Request payloads carrying a
  path don't include a NUL — the wire format uses the
  `length` field. The library reserves `req_payload[P9_MAX_PAYLOAD
  + 1]` so it can drop a `'\0'` byte past the last payload
  byte before invoking the callback. The callback can treat
  `path` as a C string without copying.

- **Reading the cap from `flags` on READ.** The chapter-114a
  protocol convention is that `P9_OP_READ` requests carry no
  payload (`length = 0`); the desired byte count rides in
  `flags`. The library recovers it and passes it as `cap`
  to `on_read`; the callback returns the byte count
  actually filled.

- **Best-effort recovery from oversized requests.** If a
  request claims a payload bigger than `P9_MAX_PAYLOAD`,
  the library drains the bytes anyway (so the next
  request stays aligned) and replies with `-EINVAL`. Without
  the drain a corrupted header would desync the channel
  permanently.

The two short helpers `read_full` and `write_full` loop
until the slice is drained, just like the kernel side. Pipe
short reads are normal under cross-thread scheduling; the
loop turns them into a single logical message read.

## `/bin/echofs` — the smallest interesting daemon

`echofs.c` is fifty lines plus three callback bodies. It
mounts `/echo` and exposes three files:

| Path           | Behaviour                                          |
| -------------- | -------------------------------------------------- |
| `/echo/hello`  | Read-only. Returns `"hello from echofs\n"`.        |
| `/echo/buf`    | Read+write. 4 KiB in-memory scratch buffer.        |
| `/echo/echo`   | Write-only. Bytes written are printed to stdout.   |

The callbacks are about what you'd expect. `on_open` maps
the path to one of three integer handles; `on_read` consults
the handle and either returns the canned hello string, the
scratch buffer, or EOF; `on_write` either appends to the
scratch buffer, prints to stdout, or returns `-EACCES`.

The handler struct is field-initialised (not aggregate
`= {0}` — see `/memories/freestanding-c-memset-trap.md`) and
passed to `userfs_serve`:

```c
struct userfs_handler h;
h.on_open    = on_open;
h.on_read    = on_read;
h.on_write   = on_write;
h.on_close   = on_close;
h.on_listdir = on_listdir;
h.on_unlink  = (int (*)(void *, const char *))0;
h.on_mkdir   = (int (*)(void *, const char *))0;
h.on_is_dir  = on_is_dir;
h.userdata   = (void *)0;

int r = userfs_serve("/echo", &h);
```

The two NULLs (`on_unlink`, `on_mkdir`) cause the library to
return `-ENOSYS` for the matching ops. echofs is read-write
on `/echo/buf` but not on the directory itself; making the
directory mutable would mean handling `mkdir /echo/foo`,
which is more complexity than this proof-of-concept warrants.

## Wiring into the Makefile

The pattern matches every other userspace daemon. From
[`Makefile`](../../../Makefile):

```make
LIBFS_OBJ := $(BUILD)/userspace/libfs/userfs.o

ECHOFS_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(LIBFS_OBJ) \
               $(BUILD)/userspace/echofs/echofs.o
ECHOFS_ELF  := $(BUILD)/userspace/echofs/echofs.elf
ECHOFS_STRIPPED := $(BUILD)/userspace/echofs/echofs.stripped.elf
```

Plus the per-target link rule, the strip rule, and inclusion
in `OSFS_BIN_FILES` and the disk-image manifest. The generic
`$(BUILD)/userspace/%.o: userspace/%.c` pattern rule from
chapter 17 handles compilation; we don't need a custom rule.

`LIBFS_OBJ` is a single object today and will gain more if
the library grows — a directory-listing helper for daemons
that want O(n) on_listdir lookups, say, or a generic
fixed-handle-table macro. Pulling it out as its own
variable now means future daemons just append it to their
own `_OBJS` lists.

## The test

[`scripts/test_userfs_echo.py`](../../../scripts/test_userfs_echo.py)
mirrors the chapter 113f mount test in structure: boot,
attach to the serial socket, drain to the shell prompt, run
commands, assert on output. The four assertions are:

1. The libfs `"/echo mounted as id N (req=R rsp=S)"` line
   appears on serial within ten seconds of `echofs &`.
2. `/bin/mount` lists `/echo` in the table.
3. `cat /echo/hello` returns `"hello from echofs"`.
4. `echo userfs-write-test > /echo/buf` followed by
   `cat /echo/buf` returns the written string.

Plus three `expect`s on `ls /echo` for each of the three
child files.

There's a one-line race-window guard between the mount-log
detection and the first `/echo/` command:

```python
wait_for(s, PROMPT, timeout=5.0)
```

The mount log line is printed *after* libfs has the request
fd open and is sitting in `read`, but the kernel's mount
table insert is sequenced with the daemon's first
`pipe_read`. There's a tiny race where `cat /echo/hello`
beats the mount entry into the table by a few hundred
microseconds. Waiting for the next shell prompt drains the
race.

## What we found along the way

The biggest discovery in this chapter was the
double-unref bug in `userfs_channel_destroy`. The first
draft dropped both `PIPE_REF_R` and `PIPE_REF_W` on each of
`req_pipe` and `rsp_pipe`. That overcounts — the kernel
half owns only the writer end of req_pipe (because the
daemon's fd table owns the reader end after `sys_mount`)
and only the reader end of rsp_pipe (because the daemon owns
the writer end). Dropping all four refs would underflow
the pipe's refcount once the daemon exited and free the
pipe while the channel was still pointing at it. We caught
the bug by reading the refcount transitions on paper
(`pipe_alloc` returns 1+1, hand-off transfers 1+1 to the
daemon, channel retains 1+1, so destroy must drop 1+1, not
2+2) and added the comment that's now in the file:

> Drop only the refs we own — dropping the daemon's side
> here would double-unref and corrupt the pipe refcount.

The half-allocated-cleanup branch in
`userfs_channel_create` *does* drop both refs on the
request pipe, but only because in that path the reply pipe
allocation failed before the hand-off ever ran. Both
branches look superficially the same; the comment keeps
them honest.

### The WRITE reply protocol mismatch

The first time the test tried `echo userfs-write-test > /echo/buf`
the shell hung. Adding `[ufs] enter` / `[ufs] exit` traces
around `userfs_call` showed an op=3 (`P9_OP_WRITE`) that
entered but never exited, while the daemon's `on_write`
handler had clearly returned. The kernel was blocked in the
reply-payload drain loop, reading bytes the daemon was never
going to send.

The cause: libfs's WRITE handler had stuffed the byte count
into `rsp.length`. But `rsp.length` is the *length of the
trailing payload*, not the result of the operation —
`userfs_call` reads `rsp.length` bytes off the reply pipe
after the header. Since the daemon had sent no payload, the
read blocked forever.

Fix: the WRITE byte count rides in `rsp.flags` (an op-specific
scalar field), and `rsp.length` stays 0. The kernel-side
`userfs_op_write` reads `(long)rsp.flags` for the result.

Rule:

> For any RPC reply in the userfs scheme, `rsp.length` is
> ALWAYS the number of payload bytes the daemon will write
> after the header. Numeric results that fit in 32 bits ride
> the op-specific scalar (`rsp.flags` for WRITE, `rsp.handle`
> for OPEN, etc.).

### The state-overwritten-in-vfs_close_all bug

Even with the WRITE protocol fixed, the test still hung at
the *next* command: `echo > /echo/buf` returned, but the
following `cat /echo/buf` produced no output. The shell was
stuck in `wait()` even though the child had completed every
single RPC and called `thread_exit`.

Tracing `thread_waitpid`'s scan showed something impossible:
the just-exited child was visible as the shell's child
(`has_match=1`), but its state was `0x0` (THREAD_READY), not
`0x5` (THREAD_EXITED). The shell's reap loop only takes
children whose state is THREAD_EXITED, so it kept finding
"there's a child but it's not ready," set itself back to
WAITING, and yielded — into a sleep nothing would wake.

The root cause was an ordering bug in `thread_exit` that had
been latent since chapter 11 — invisible because no
filesystem close path had ever blocked before:

```c
void thread_exit(int code) {
    g_current->state = THREAD_EXITED;   /* WRONG: too early   */
    ...
    vfs_close_all(g_current);           /* userfs close blocks */
    ...
}
```

`vfs_close_all` for a userfs fd goes through
`userfs_op_close` → `userfs_call` → `pipe_read`, and
`pipe_read` calls `thread_block_on`, which unconditionally
sets `g_current->state = THREAD_BLOCKED`. The matching
`thread_wake_blocked` then sets it to `THREAD_READY`. The
earlier `THREAD_EXITED` is silently lost. Every other close
path (pipe, pty, ramfs, tmpfs) is non-blocking, so the early
state assignment had never been observed to matter.

Fix: move `g_current->state = THREAD_EXITED` to AFTER
`vfs_close_all` returns, right before the parent-wake
section. The rule:

> Any "terminal" state assignment (thread_exit, fatal signal
> delivery) must come AFTER all blocking operations in that
> path. Setting it early is a "set and forget" trap because
> any block/wake cycle resets the state.

Both diagnostic helpers used to find the bug are kept in the
repo per the debug-scripts-policy:
[`scripts/_dbg_userfs_seq.py`](../../../scripts/_dbg_userfs_seq.py)
boots the OS and walks through `echofs &`, a read, a write
redirect, and a follow-up read, printing the serial output
per command.

### `c->open_fds` inheritance bumping

A latent counter bug also surfaced. `userfs_channel.open_fds`
counts the live `FD_USERFS_FILE` entries that reference the
channel. Both `thread_inherit_fds` (used by `sys_fork` and
spawn) and `dup_parent_fd_into_child` (used by `sys_spawn_pipe`)
copied the `fd_entry` byte-for-byte without bumping
`open_fds`. The child's eventual close would then decrement
past the bumps the parent's open had made — clamped to zero
by the `> 0` guard in the close path, but logically wrong
and a trap waiting for any "did we hit zero?" cleanup logic
to be added later. Both sites now bump `open_fds` for
`FD_USERFS_FILE` slots; `dup_parent_fd_into_child` also
decrements when it tears down a pre-existing userfs slot it
is about to overwrite.

## What got verified

- `make` succeeds with the new userspace targets. The
  disk image gains one file (`echofs`), confirming the
  `OSFS_BIN_FILES` and manifest edits are wired right.
- `scripts/test_userfs_echo.py` passes locally:
  - libfs prints `"/echo mounted as id N (req=R rsp=S)"`.
  - `/bin/mount` lists `/echo`.
  - `cat /echo/hello` returns the canned text.
  - Write-then-read on `/echo/buf` round-trips through
    the daemon.
  - `ls /echo` shows all three child files.
- The full regression sweep stays green. No existing test
  uses `/echo/` so nothing else is touched.

## Applied to

- **Existing apps modified**: none in this chapter — the
  daemon-side machinery is freshly born and there are no
  /echo callers in the existing app suite.
- **New apps added**:
  - [`userspace/echofs/echofs.c`](../../../userspace/echofs/echofs.c)
    — a real binary that demonstrates the protocol end to
    end, runnable from the shell (`echofs &`).
- **Tests added**:
  - [`scripts/test_userfs_echo.py`](../../../scripts/test_userfs_echo.py)
    — boots the OS, spawns the daemon, runs the four
    assertions above.

The first production user of `libfs` lands in
[chapter 114d](114d-clipboardd-port.md), which rewrites
`/bin/clipboardd` to serve `/clipboard/` instead of binding
`/srv/clipboard` over the chapter-108 IPC bus. notepad's
Ctrl-C / Ctrl-V become two-line additions: open the file,
read or write, close.

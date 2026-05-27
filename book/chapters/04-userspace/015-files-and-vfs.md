# Chapter 15 — Files, VFS, and a tiny ramfs

> **Where the code lives.**
> VFS interface: [kernel/core/vfs.h](../../../kernel/core/vfs.h)
> Ramfs implementation: [kernel/core/vfs.c](../../../kernel/core/vfs.c)
> File-descriptor table on threads: [kernel/core/thread.h](../../../kernel/core/thread.h)
> Syscalls: [kernel/core/syscall.c](../../../kernel/core/syscall.c) (SYS_OPEN/READ/CLOSE)
> Userspace wrappers: [userspace/libc/syscall.h](../../../userspace/libc/syscall.h)
> First file consumer: [userspace/cat/cat.c](../../../userspace/cat/cat.c)
> Embedded file content: [assets/ramfs/](../../../assets/ramfs)

[Chapter 14](014-elf-and-first-user-program.md) gave us a user
program that runs at EL0 and exits cleanly. Useful, but that user
has no way to *read state*. Even something as simple as "print the
contents of the message of the day" requires the kernel to expose
files: a name space, a way to open by name, a way to copy bytes
out, and a way to release the resource.

This chapter adds a tiny VFS layer and a ramfs that serves a
fixed set of files baked into the kernel image. Three new
syscalls (`SYS_OPEN`, `SYS_READ`, `SYS_CLOSE`) connect the
EL0 user to the EL1 file system. The capstone is `userspace/cat`,
which reads `/motd` and prints it to the console.

## Why a VFS at all when there's only one FS?

The whole layer is barely 200 lines of C. We could wire ramfs
directly into the syscall dispatcher with no abstraction and save
a function call per `read`. So why not?

Two reasons:

1. **Indirection has near-zero cost when it's a thin enum.** The
   dispatcher writes one virtual call (`vfs_read`) which the
   compiler can often inline anyway. The cost is one extra stack
   frame and a pointer comparison.

2. **The chapter that adds a disk-backed FS lands without
   touching the syscall layer.** When chapter 17 (or wherever
   virtio-blk lives) introduces a real filesystem, it slots in
   as a second `vfs_ops` and the syscall code is unchanged. The
   alternative — peppering syscalls with `if (path starts with
   /ramfs/) ... else ...` — is exactly the maintenance pit
   real OSes spent the 1980s climbing out of.

That said, our VFS is *minimal*: there is no
`vfs_ops` struct, no mount table, no path walker. There's a
single function — `vfs_open` — that knows about exactly one
file system. The structure is *almost* there: the per-fd
`ramfs_index` field is the only thing tying us to ramfs, and
swapping it for an opaque pointer + a function table is a
mechanical refactor. We'll do it the day it pays for itself.

## What ramfs is (and isn't)

`ramfs` is a flat namespace of read-only files whose contents
live in the kernel image. Each file is wrapped at link time by
`objcopy -I binary` into an ELF object exposing
`_binary_<name>_start/_end` symbols. The kernel link script
captures those into `.rodata.embedded_user` (yes, the same
section we used for embedded user binaries — the section name is
generic on purpose, and the linker just concatenates everything
that lands there).

```c
extern char _binary_motd_txt_start[];
extern char _binary_motd_txt_end[];

struct ramfs_file {
    const char    *name;
    const uint8_t *data;
    const uint8_t *end;     /* size = end - data */
};

#define RAMFS_FILE(symbol_base, fname) \
    { .name = (fname), \
      .data = (const uint8_t *)_binary_##symbol_base##_start, \
      .end  = (const uint8_t *)_binary_##symbol_base##_end }

static struct ramfs_file g_ramfs[] = {
    RAMFS_FILE(motd_txt,   "/motd"),
    RAMFS_FILE(README_txt, "/README"),
};
```

Two C subtleties worth noting:

- **The size is computed lazily at runtime, not stored as a
  field.** We tried `.size = (size_t)((uintptr_t)end -
  (uintptr_t)start)` first; GCC rejects it because subtracting
  two extern symbols isn't a compile-time constant. Storing the
  `_end` symbol and computing size at use is the standard
  workaround.
- **Filenames embed the leading `/`.** No path walker means we
  match the whole string verbatim. When chapter 16 adds a
  directory hierarchy this becomes a real path walk.

ramfs is *read-only*: there's no `vfs_write` for ramfs files,
and the dispatcher rejects writes to fd ≥ 3 with `-EROFS`. The
console (fd 1, fd 2) still accepts writes through `SYS_WRITE`.

## The fd table

Per-thread fd tables live inside `struct thread`:

```c
#define FD_TABLE_SIZE   16

struct fd_entry {
    int        in_use;
    uint64_t   offset;        /* current read position */
    int        ramfs_index;   /* -1 means "console" for fd 0/1/2 */
};

struct thread {
    /* ... existing fields ... */
    struct fd_entry  fds[FD_TABLE_SIZE];
};
```

Two design choices:

- **The fd table lives on the thread, not on a separate "process"
  object.** We don't have processes yet. When chapter 16
  introduces `fork`/`execve`, the fd table will move to a per-
  process structure with reference counting. For now,
  thread = process and the fd table goes wherever the thread
  goes.
- **The lowest free slot starts at 3.** POSIX semantics: fd 0/1/2
  are reserved as stdin/stdout/stderr and pre-occupied. `vfs_open`
  scans from index 3 up, so the first user-opened file is fd 3
  on every thread.

`vfs_init_fdtable(struct thread *t)` is called from
`thread_init`, `thread_create`, and `user_thread_create`. It
zeroes everything, then plants the three console entries.

## The three syscalls

```c
case SYS_OPEN:   ret = sys_open(a0, a1);      break;
case SYS_READ:   ret = sys_read(a0, a1, a2);  break;
case SYS_CLOSE:  ret = sys_close(a0);         break;
```

Each of these is a one-line trampoline into the VFS:

```c
static long sys_open(long name_ptr, long flags)
{
    /* TODO: copy_from_user with bounds check + len cap. */
    const char *name = (const char *)(uintptr_t)name_ptr;
    return vfs_open(name, (int)flags);
}
```

The pointer is passed verbatim. That's a known shortcut — the
kernel trusts the user's pointer because there's no per-process
isolation yet. A real OS would `copy_from_user` the path into a
kernel buffer, with a length cap to defend against runaway
strings. Chapter 16, when it splits address spaces, has to add
this validation everywhere a user pointer crosses the syscall
boundary; until then we keep things simple and document the gap.

## How `vfs_read` produces bytes

```c
long vfs_read(int fd, void *buf, size_t len)
{
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    if (!buf) return -EINVAL_VFS;

    struct thread *t = thread_current();
    struct fd_entry *e = &t->fds[fd];
    if (!e->in_use) return -EBADF;

    if (e->ramfs_index < 0) return 0;   /* console = EOF for now */

    const struct ramfs_file *f = &g_ramfs[e->ramfs_index];
    size_t f_size = ramfs_size(f);
    if (e->offset >= f_size) return 0;

    size_t remaining = f_size - (size_t)e->offset;
    size_t to_copy   = len < remaining ? len : remaining;
    uint8_t *dst = (uint8_t *)buf;
    const uint8_t *src = f->data + e->offset;
    for (size_t i = 0; i < to_copy; i++) dst[i] = src[i];

    e->offset += to_copy;
    return (long)to_copy;
}
```

Three semantically interesting bits:

- **EOF is `0`**, not `-1`. POSIX says `read()` returns 0 to
  signal end-of-file; negative means error. User code therefore
  loops while `n > 0`.
- **Reading from `console` (fd 0) returns 0 immediately.** That's
  not POSIX, but it's the cheapest sensible behavior pre-
  keyboard. Chapter 21 (virtio-input) wires the keyboard ring
  buffer through here so `read(0, ...)` blocks until the user
  types.
- **No partial-page concerns.** ramfs files are bytes inside a
  flat blob, so the loop just walks byte-by-byte. When chapter
  18 introduces a real disk-backed FS, the loop becomes "read
  blocks into a kernel page-cache page, then copy out the slice
  the caller asked for".

## The `cat` user program

```c
#include "../libc/syscall.h"

int main(void)
{
    puts("[cat] opening /motd ...");
    int fd = open("/motd", 0);
    if (fd < 0) {
        write(1, "[cat] open failed: errno=", 25);
        putn(-fd);
        write(1, "\n", 1);
        return 1;
    }

    char buf[256];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);

    int bad = open("/nope", 0);
    write(1, "[cat] open(/nope) returned ", 27);
    putn(bad);
    write(1, "\n", 1);
    return 0;
}
```

`cat` is six syscalls' worth of code. The loop pattern (read into
a fixed buffer, write what you got, repeat until EOF) is the
exact pattern every Unix utility has used since 1971. It exists
here as both a smoke test and a recipe for the next user program.

The error-path test at the bottom — opening a file that doesn't
exist and printing the returned errno — proves the negative-return
convention works end-to-end. The kernel returned `-2` (`-ENOENT`),
the userspace inline-asm wrapper preserved it, the `cat`
formatter printed it.

## Adding a new file in three steps

1. Drop a file at `assets/ramfs/<name>`.
2. Append it to `RAMFS_SRCS` in the Makefile. The pattern rule
   `$(BUILD)/ramfs/%.o: assets/ramfs/%` already wraps it and
   exposes `_binary_<name>_<ext>_start/_end`.
3. Add a corresponding `extern char _binary_<name>_<ext>_start[]`
   pair in `kernel/core/vfs.c` and an entry in `g_ramfs[]`.

A dynamic registration mechanism (a constructor section that
auto-registers each file) would shave step 3 off, but it's three
lines per file as it stands, which is hardly worth a clever
macro.

## Verification

Expected serial output once the cat thread runs:

```
[user] loading cat (0x1438 bytes)
[user] entry = 0x22fff2000, sp = 0x22fff2000
[user] spawned pid 0x4
[cat] opening /motd ...
Welcome to the hobby OS.

This file lives in the kernel-embedded ramfs.
...
[cat] /motd done.
[cat] open(/nope) returned -2
[sys_exit] thread 'cat' exited with code 0x0
[user] cat exited cleanly
```

That single block exercises every path in this chapter:

- `vfs_init_fdtable` for the new user thread (slots 0/1/2
  pre-allocated to console).
- `SYS_OPEN("/motd")` → `vfs_open` → `ramfs_lookup` → fd 3.
- `SYS_READ(3, buf, 256)` → `vfs_read` → memcpy of the next slice
  of `/motd`'s bytes → `SYS_WRITE(1, buf, n)` to the serial
  console.
- Loop until `vfs_read` returns 0 (EOF).
- `SYS_CLOSE(3)` → fd-table slot freed.
- `SYS_OPEN("/nope")` → `ramfs_lookup` returns -1 → `-ENOENT`.

If any one of these were broken, the trace would stop short.

## What we *didn't* build

This chapter is, again, the smallest end-to-end VFS that lets a
user read a file. Several things you might expect from a real
file system are deferred:

- **Writes.** ramfs is read-only by construction. The `O_RDWR`
  flag and `SYS_WRITE` to fd ≥ 3 both fail today.
- **Directories.** There is one flat namespace. No `opendir`,
  no `getdents`. Chapter 17's disk-backed FS introduces a real
  hierarchy.
- **Permission checks.** Every file is world-readable. We don't
  even have user IDs.
- **Symlinks, hardlinks, mknod, mmap.** Future chapters as the
  need arises.
- **A copy_to_user / copy_from_user helper.** User pointers are
  trusted verbatim. The TODO comments mark the spots.

## Summary

- The VFS is a thin layer between the syscall dispatcher and
  whichever file system implementation owns a path.
- Ramfs serves a fixed set of files baked into the kernel image
  via `objcopy -I binary`.
- File descriptors live in a per-thread `fds[FD_TABLE_SIZE]`
  table; `vfs_init_fdtable` plants the three console fds at
  thread creation.
- Three new syscalls — `SYS_OPEN`, `SYS_READ`, `SYS_CLOSE` —
  are one-line trampolines into `vfs_*`.
- The `cat` user program reads `/motd` and prints it,
  exercising the entire path in a dozen lines of code.

Next chapter: [init, spawn, wait](016-init-spawn-wait.md). The
fork/execve story arrives later, in
[Chapter 72 — AArch64 fork and address-space copy](../09-process-model/072-aarch64-fork-and-as-copy.md),
once each process has its own L1 page table.

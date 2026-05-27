# Chapter 20 — A read-only on-disk filesystem and `cat /mnt/...`

## Where we are

Chapter 19 left us able to read and write 512-byte sectors of a real
disk. That is the lowest interesting layer. To make it actually
useful — for either users or programs — we need three more things:

1. An *on-disk format* that says where files live and how big they
   are. (Otherwise a "disk" is just a 1 MiB blob; nothing knows
   what's in it.)
2. A way to *mount* that format under our existing VFS so the
   pre-existing `vfs_open` / `vfs_read` syscall path works
   uniformly for both disk-backed and ramfs-backed files.
3. A way for the shell to pass *arguments* to a program — because
   right now `/bin/cat` is hardcoded to read `/motd`, and the
   whole point of disk-backed files is that the user picks one.

This chapter does all three. The on-disk format is a tiny
purpose-built FS we'll call **OSFS-1**. The mount point is `/mnt`.
The argument-passing mechanism is two new syscalls: `SYS_SPAWN`
gains a second argument, and a new `SYS_GETARGS` lets the child
read it.

By the end you can sit at the shell prompt and type:

```
$ /bin/cat /mnt/poem.txt
```

and watch the bytes flow:

```
read(0)
  -> /bin/sh parses "/bin/cat" + " /mnt/poem.txt"
  -> SYS_SPAWN("/bin/cat", "/mnt/poem.txt")
  -> /bin/cat starts, calls SYS_GETARGS, gets "/mnt/poem.txt"
  -> SYS_OPEN("/mnt/poem.txt")
  -> vfs_open sees /mnt/ prefix, dispatches to osfs_lookup
  -> osfs_lookup hits dir entry: start_sector=3, size=569
  -> SYS_READ(fd, buf, 256)
  -> osfs_read -> virtio_blk_read(LBA=3, sector_buf)
  -> virtio queue 0 -> QEMU virtio-blk -> build/disk.img
  -> bytes copied back through the chain
  -> SYS_WRITE(1, buf, n) -> PL011 -> your terminal
```

## OSFS-1: a deliberately tiny on-disk format

FAT12 was a candidate. It would have been honest -- FAT is everywhere --
but it has too many quirks for this chapter to be about *layering*.
12-bit packed FAT entries split across byte boundaries. Three
reserved/EOC value ranges (`0xFF8`-`0xFFF` for end-of-chain,
`0xFF7` for bad). Variable cluster sizes. The
`FAT_ID` byte that's-not-quite-an-entry at the start of the FAT.
8.3 short names with case bits. None of that is the lesson here.

So OSFS-1 instead — the smallest format that lets us do directory
lookup and byte-range reads:

```
sector 0  superblock (512 B)
    0x00  magic[8] = "OSFS-001"
    0x08  file_count : u32 (little-endian)
    0x0C  reserved...

sector 1  directory: array of 32-byte struct osfs_dirent, capped
          at OSFS_MAX_FILES = 16 (one full directory sector)

    struct osfs_dirent {       // 32 bytes
        char     name[20];      // NUL-padded; no '/'
        uint32_t start_sector;  // absolute LBA
        uint32_t size_bytes;
        uint32_t reserved;
    };

sector 2..N  file data, sector-aligned, packed end-to-end
```

That's it. No FAT. No free-space map. No subdirectories. Files
cannot be extended past their initially-allocated sector range.
Writes are not supported at all. The kernel side, in
[`kernel/core/osfs.c`](../../../kernel/core/osfs.c), is about 130
lines.

What you get for that 130 lines:

- Mount at boot via `osfs_init()`: read sector 0, validate magic,
  read sector 1, copy out the directory.
- O(N) lookup over up to 16 entries (small enough to be silly to
  index).
- Byte-range read through `osfs_read(start_sector, size, offset,
  buf, len)`, which translates into one or more `virtio_blk_read`
  calls on whatever sectors the request straddles.

The cost is real but bounded. Adding a real format later — FAT,
ext2, our own with directories — is a swap-in: it has to provide
the same `osfs_lookup`-style API and the rest of the stack is
unchanged.

## Building the disk image

The image is built host-side from files under `assets/osfs/` by a
small Python script:

```sh
$ scripts/mkosfs.py build/disk.img \
      hello.txt=assets/osfs/hello.txt \
      poem.txt=assets/osfs/poem.txt
  hello.txt            sector    2  size    113 bytes (spans 1 sectors)
  poem.txt             sector    3  size    569 bytes (spans 2 sectors)
wrote build/disk.img (1048576 bytes, 2 files, 3 sectors used)
```

The script ([scripts/mkosfs.py](../../../scripts/mkosfs.py)) is
~70 lines. It packs the same `osfs_dirent` struct the kernel reads,
which is what makes the format trivial to evolve: change the struct
in the kernel, change the `struct.pack` format string in the
script, rebuild the image, done.

The Makefile target depends on the script and the input files:

```make
$(DISK): scripts/mkosfs.py $(OSFS_FILES)
	python3 scripts/mkosfs.py $(DISK) \
	    hello.txt=assets/osfs/hello.txt \
	    poem.txt=assets/osfs/poem.txt
```

Edit a file under `assets/osfs/`, run `make all`, and the disk
image is regenerated automatically before QEMU starts.

## Mounting under the VFS

The VFS in chapter 15 had one job: walk a static `g_ramfs[]` table
and serve the file. Adding OSFS means the VFS has to *dispatch* to
one of two filesystems at `vfs_open` time. The discriminator is the
path prefix:

- Path begins with `/mnt/` → strip the prefix, look up in OSFS.
- Anything else → look up in ramfs.

We could have built a generic `struct vfs_ops { lookup, read, ... }`
table indexed by mount point. We didn't — there is exactly one disk
mount and exactly one ramfs, so the cost of the abstraction
outweighs the benefit. When we eventually mount a second disk or
add `/proc` we'll cross that bridge.

The fd table grows two new fields:

```c
struct fd_entry {
    int        in_use;
    uint64_t   offset;
    int        ramfs_index;
    uint32_t   osfs_start;     // LBA of file's first sector
    uint32_t   osfs_size;      // file size in bytes
};
```

`osfs_size != 0` ⇒ this fd reads from the disk mount; otherwise it
reads from ramfs (or is a console fd). `vfs_read` checks `osfs_size`
first and routes accordingly.

## SYS_SPAWN gains an args string

Up until this chapter `spawn(path)` was a one-argument syscall: it
took the binary's path and that was it. The child started with no
information about *why* it had been spawned.

For `cat`, the obvious thing is to tell it which file to print. The
classical Unix solution is `argv[]` on the new process's stack. We
can't quite ship that yet — argv lives at a known location in the
new process's address space, and we still have one global address
space, so picking that location safely needs more thought.

What we *can* do today is much smaller and gets us 90% of the
practical benefit:

- `SYS_SPAWN(path, args)` — second argument is a NUL-terminated
  string. The kernel copies it (truncating at 127 bytes) into a
  fixed buffer on the new `struct thread`.
- `SYS_GETARGS(buf, len)` — the child reads its args buffer into
  user space.

The kernel side of both is short. `sys_spawn` allocates the thread
as before, then walks the args string and copies it into
`t->args[]`. `sys_getargs` copies `t->args` back to user space and
returns the byte count.

The shell side is even shorter. After parsing the first
whitespace-separated token as the path, everything after becomes
the args:

```c
char *args = line;
while (*args && *args != ' ' && *args != '\t') args++;
if (*args) {
    *args++ = '\0';
    while (*args == ' ' || *args == '\t') args++;
}
int tid = spawn(line, args);
```

`/bin/cat` consumes them with the same simplicity:

```c
char target[64];
long got = getargs(target, sizeof(target));
if (got <= 0) target = "/motd";   // default when invoked with no args
int fd = open(target, 0);
...
```

A real argv would let `cat` take multiple files (`cat a b c`), would
let programs see `argv[0]` (their own name), and so on. We will get
there. The current single-string mechanism gets the shell-driven
file-printing demo working today and is small enough that nobody
will mourn it when it is replaced.

## What the kernel did NOT do that you might assume it did

- **No block cache.** `osfs_read` calls `virtio_blk_read` once per
  sector it touches. Reading all 569 bytes of `poem.txt` takes two
  polled virtio requests. For interactive use this is fine. When
  the FS gets larger and we have something other than `cat`
  reading from it (say, an ELF loader pulling binaries off disk),
  a 16-slot LRU page cache lands in front. Sketch:

  ```
  struct cache_slot { uint32_t lba; uint8_t  data[512]; uint32_t ts; bool dirty; }
  cache_slot[16];
  blk_cached_read(lba): hit? -> memcpy; miss? -> evict LRU + fetch + memcpy
  ```

  Write-through is easy (also issue the blk_write on dirty path).
  Write-back means we need a flush-on-shutdown story.

- **No copy_from_user / copy_to_user.** Both `sys_spawn` and
  `sys_getargs` trust the user pointers. A malicious user program
  could pass a pointer into the kernel's address space and we'd
  happily copy from / to it. We map kernel RAM as EL1-only so a
  *read* would fault, but the kernel-mode handler is the one
  faulting, which is much worse than returning -EFAULT. Adding
  copy_from_user is its own milestone — needs per-process page
  tables to be useful.

- **No path normalisation.** `/mnt/hello.txt`, `/mnt//hello.txt`,
  and `/mnt/./hello.txt` are not all the same path to us — only the
  first works. A real VFS canonicalises before dispatch.

- **No error reporting beyond errno.** `cat: cannot open
  /mnt/missing: errno=2` is what you'll see. Mapping 2 to
  `"No such file or directory"` would need a `strerror` table —
  small, easy, deferred.

## Verification

Built fresh:

```
$ make all
... build chatter ...
$ printf '/bin/cat /mnt/hello.txt\n/bin/cat /mnt/poem.txt\nexit\n' \
      | timeout 8 make run
```

Trimmed output:

```
mounting OSFS-1 from disk ... [osfs] mounted, 2 files:
       /mnt/hello.txt (113 bytes @ sector 2)
       /mnt/poem.txt (569 bytes @ sector 3)
ok

[sh] tiny shell ready.  type 'help' for a list of commands.
$ /bin/cat /mnt/hello.txt
hello from disk!

This file was read from sector 2 of the virtio-blk device,
not from the kernel-embedded ramfs.

$ /bin/cat /mnt/poem.txt
If you can read this through the kernel's OSFS-1 mount,
the chain works:

  /bin/sh -> read(0) line "/bin/cat /mnt/poem.txt"
  /bin/cat -> SYS_OPEN("/mnt/poem.txt")
  vfs_open -> osfs_lookup -> hits sector N
  vfs_read -> osfs_read -> virtio_blk_read(N)
  ...
```

For the price of ~250 lines of new C and a small Python script,
the kernel now serves arbitrary files that live on a real disk
through the same syscall path it always had.

## What this unlocks

The next big payoff is loading user binaries from disk instead of
the embedded ramfs. Today the ramfs has six entries (`/motd`,
`/README`, `/bin/hello`, `/bin/cat`, `/bin/init`, `/bin/sh`) and
the kernel has to be relinked every time you change one. With OSFS
we can move `/bin/*` to disk and stop relinking the kernel for
every user-program edit.

That requires:
- An ELF loader that reads from a `vfs_read` stream rather than a
  pre-mapped buffer (or just calls `osfs_read` straight, which is
  smaller).
- A `SYS_SPAWN` extension that recognises `/mnt/...` paths.
- A `/bin -> /mnt/bin` style namespace mapping, or — simpler —
  shell-side path resolution.

That's the next chapter. After that, the world starts to look like a
real Unix.

Chapter 21 next: virtio-console, which gets us serial output through
the same transport instead of the boot UART, freeing the UART for
debugging and clearing the way for multiple consoles in the GUI.

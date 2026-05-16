# Chapter 22 — Loading user binaries from disk

## What we just stopped doing

Up through chapter 21 every user program was *embedded in the
kernel image*. The Makefile would link `init.elf`, `sh.elf`,
`cat.elf`, and `hello.elf`, then run each one through
`objcopy -I binary -O elf64-littleaarch64` to wrap the stripped
ELF as a `.o` file with `_binary_<name>_elf_bin_start/_end`
symbols, then link those objects into the kernel. The VFS held
a static table that mapped paths like `/bin/sh` to those
embedded blobs.

It worked. It also meant:

- **Every user-program edit relinked the kernel.** Typo in
  `cat.c`? Recompile cat, regenerate cat's wrapper object, relink
  the kernel, reboot QEMU. The kernel image is in the loop for
  changes that have nothing to do with the kernel.
- **The kernel image carried four ELFs in `.rodata`.** ~21 KiB of
  user binary inside the kernel's own address space.
- **There was no path from "we have a disk now" to "use it for
  binaries."** The whole point of milestone 12 was to make a disk
  usable; reading text files off it is a demo, but the practical
  payoff is binaries.

This chapter cuts the embedding. After it, the kernel knows
nothing about which user programs exist. They live on the disk
image. The kernel grows by zero bytes when you add a fifth
user program; it grows by zero bytes when you change `sh.c`.

## The shape of the change

Three pieces:

1. **`vfs_load(path, **buf, *size)`** — a new VFS entry point
   that returns an entire file as a freshly-allocated kheap
   buffer. Caller `kfree`s. Internally dispatches just like
   `vfs_open`:
   - `/mnt/<name>` or `/bin/<name>` → OSFS lookup, allocate buffer,
     `osfs_read` the whole file in.
   - anything else → ramfs lookup, `kmalloc` + `memcpy`.
2. **`sys_spawn` switches to `vfs_load`.** It used to call
   `vfs_lookup`, which was ramfs-only. Now it calls `vfs_load`,
   passes the buffer to `elf_load_user` (which reads PT_LOAD
   segments out of it into newly-allocated user pages), then
   immediately `kfree`s the buffer. The user's mapped pages are
   independent.
3. **`mkosfs.py` adds the binaries to the disk image.** The
   Makefile target now lists them as inputs:

   ```make
   OSFS_BIN_FILES := $(INIT_STRIPPED) $(SH_STRIPPED) \
                     $(CAT_STRIPPED) $(HELLO_STRIPPED)

   $(DISK): scripts/mkosfs.py $(OSFS_FILES) $(OSFS_BIN_FILES)
       python3 scripts/mkosfs.py $(DISK) \
           hello.txt=assets/osfs/hello.txt \
           poem.txt=assets/osfs/poem.txt \
           init=$(INIT_STRIPPED) \
           sh=$(SH_STRIPPED) \
           cat=$(CAT_STRIPPED) \
           hello=$(HELLO_STRIPPED)
   ```

   Five lines. Each binary becomes a 32-byte directory entry plus
   sectors of payload.

The boot path also flips: the kernel's hand-written init loader in
`main.c::userspace_demo` used to call `vfs_lookup("/bin/init", ...)`
against the ramfs table. It now calls `vfs_load("/bin/init", ...)`,
which dispatches to OSFS, reads the binary off the disk, runs the
ELF loader, and `kfree`s.

## Why the heap-buffer story matters

The old `vfs_lookup` returned a `const uint8_t *` straight into the
embedded ramfs blob. That works fine for ramfs because the bytes
are already in the kernel's address space and live forever. It
would not work for OSFS, where the bytes are on disk and we'd have
to either:
- pre-load the entire file into a kheap buffer at `lookup` time
  (and then who frees it?), or
- have the loader do incremental sector reads as it walks the ELF
  headers (gross — many small virtio requests, complicated
  bounds tracking).

`vfs_load` makes the lifetime explicit: caller owns the buffer,
caller `kfree`s. ELF loading is a one-shot operation — load
PT_LOAD segments into user pages, capture the entry vaddr, done —
so the buffer's life is just the duration of `elf_load_user`. Total
wall-clock cost of allocating + freeing on a few-kilobyte buffer is
negligible compared to the actual sector reads.

## The /bin/ dispatch is not a real namespace

You'll notice the dispatch:

```c
if (path_starts_with(path, "/mnt/")) bare = path + 5;
else if (path_starts_with(path, "/bin/")) bare = path + 5;
```

Both prefixes go to OSFS, and both strip 5 characters. So `/bin/sh`
and `/mnt/sh` resolve to the same on-disk file. That's a *feature*
in the strict "we have one filesystem with one flat directory"
sense, but it's also a hack. A real OS distinguishes:

- `/bin` — system binaries
- `/mnt` — a mount point for an external filesystem
- `/usr/bin`, `/sbin`, etc.

We don't have directories yet. OSFS is flat. So we're using path
*prefixes* as a poor-man's namespace router. When OSFS grows
subdirectory support (or we mount a real filesystem), this code
becomes a path-walk and a per-mount-point dispatch table. Until
then, two prefixes that map to the same files is honest about what
the kernel can and can't do.

## What the kernel still embeds

Just the ramfs files (motd, README), and only because they were
already there and they're tiny. Nothing prevents moving them to
disk too — `vfs_load("/motd")` would still find them in ramfs
since the disk-prefix dispatch only catches `/mnt/` and `/bin/`.
Moving them is a one-line change to mkosfs.py and a deletion of
the ramfs entries; we'll do it when something other than the
chapter-8 demo cares.

## Verification

```
$ make clean && make all
... user programs build, get stripped, get baked into disk.img ...

  hello.txt            sector    2  size    113 bytes (spans 1 sectors)
  poem.txt             sector    3  size    569 bytes (spans 2 sectors)
  init                 sector    5  size   5352 bytes (spans 11 sectors)
  sh                   sector   16  size   5720 bytes (spans 12 sectors)
  cat                  sector   28  size   5032 bytes (spans 10 sectors)
  hello                sector   38  size   4656 bytes (spans 10 sectors)
wrote build/disk.img (1048576 bytes, 6 files, 46 sectors used)
```

Then:

```
$ printf '/bin/cat /mnt/hello.txt\n/bin/hello\nexit\n' | make run
[user] loading /bin/init (5352 bytes)        <- loaded from disk!
[user] entry = 0x22fffa000, sp = 0x22fffa000
[init] starting (pid 1)
[init] spawn /bin/hello -> tid=4             <- /bin/hello is on disk too
hello from EL0!
...
$ /bin/cat /mnt/hello.txt
hello from disk!
...
$ /bin/hello
hello from EL0!
...
$ exit
```

Every user binary in that trace was read off the disk and copied
into freshly-allocated user pages by `elf_load_user`. The kernel
image itself contains zero user-program bytes.

To prove it: edit `userspace/sh/sh.c` (e.g. add a banner), run
`make all`, run `make run`. The kernel ELF is byte-identical;
only `build/disk.img` changed. The change-and-test loop is now
*just the program* you changed.

## The cost (which is real but bounded)

Loading init from disk is now N polled virtio reads instead of a
free pointer dereference. For our four small binaries:

| binary | size | sectors | virtio reads to load |
|--------|-----:|--------:|---------------------:|
| init   | 5352 |      11 |                   11 |
| sh     | 5720 |      12 |                   12 |
| cat    | 5032 |      10 |                   10 |
| hello  | 4656 |      10 |                   10 |

So spawning a program is now ~10 polled disk reads. On HVF this is
imperceptible — `/bin/cat` still feels instant from the shell.
Once binaries get larger this becomes worth caching, which is
exactly what a block cache is for. Sketch:

```
struct cache_slot {
    uint32_t lba;
    uint8_t  data[512];
    uint32_t ts;       // for LRU
    bool     dirty;    // for write-back, if we ever do that
};
struct cache_slot slots[16];

blk_cached_read(lba, dst):
    if hit: memcpy + bump ts; return
    evict LRU, virtio_blk_read(lba, slot), memcpy, set ts
```

Drop that in front of `virtio_blk_read` and `osfs_read` is suddenly
free for re-reads of the same sectors (very common in ELF loading
because the program header table and the first PT_LOAD segment are
in the same sector). That's a future milestone — small,
self-contained, immediate win.

## What's left embedded; what's deferred

Embedded:
- `motd`, `README` — ramfs tiny demo files.

Deferred:
- **A real argv.** `cat` still receives a single args string via
  `SYS_GETARGS`; the shell can't pass `cat a b c` as three separate
  arguments. Fixing this needs per-process address spaces (so we
  can safely place argv on the user stack at a known location).
- **A block cache.** Spawn cost grows linearly with binary size.
- **Subdirectories in OSFS.** Currently a flat 16-entry namespace.
  Once we add a third user program category (services? libraries?)
  the flat namespace gets old.
- **Loader optimization.** `elf_load_user` reads the entire file
  into a kheap buffer even though it only needs the ELF header,
  the program header table, and the file-resident bytes of each
  PT_LOAD. For our small binaries this costs maybe 1 KiB of
  unused data per spawn; for a megabyte binary it'd be wasteful.
  Cleaner approach is a `vfs_pread(fd, buf, len, offset)` API and
  an ELF loader that walks the file via that.

## What chapter 23 is about

The next thing that matters is **input**. Right now keyboard input
goes through the PL011 UART, which is the boot serial console.
That's fine for a single-user single-terminal demo, but it
conflates the debug console with the user keyboard. The next
device on the virtio bus is `virtio-input`, which gives us a
proper keyboard event stream we can route to whichever process
"owns" the foreground (which in our world means `/bin/sh`, but
when the GUI lands it'll mean the focused window).

After that comes virtio-gpu, and we have a framebuffer, and we
can stop pretending the whole kernel is a serial-line demo.

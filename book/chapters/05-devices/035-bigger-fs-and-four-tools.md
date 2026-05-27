# Chapter 35 — Bigger filesystem and four classic tools

After chapters 29–34 the system can change directories, set
environment variables, expand them, and quote them. The shell
is shaped right; the toolbox is empty. The OSFS image holds 14
files and is one slot from the 16-file ceiling. Time to grow
the filesystem and fill it.

This chapter does two things:

1. Bump `OSFS_MAX_FILES` from 16 to 32 by giving the directory
   a second sector.
2. Ship four classic Unix utilities — `grep`, `wc`, `head`,
   `tail` — that operate on file arguments. Without pipes they
   can't yet compose, but each is useful directly today and
   composable for free as soon as pipes land.

## OSFS-1 layout, then and now

OSFS-1 carved up the 1 MiB image like this in chapter 11:

```
sector 0   superblock      ("OSFS-001" magic, file_count u32)
sector 1   directory       (16 dirents × 32 B = exactly 512 B)
sector 2..N  file data
```

A dirent is 32 bytes (`name[20]`, three u32s), and a sector is
512 bytes, so a single-sector directory holds exactly 16
entries. Keeping the directory to one sector kept the on-disk
parsing trivial — read sector 1 into a 512-byte stack buffer,
walk it, done.

Now there are 12 user binaries plus 2 text files and the system
is at 14/16. Every new tool requires an OSFS bump first. The
choice is *how* to grow:

- **Wider dirent.** No — would break every cached layout in the
  reader and the format is already 32 B / dirent.
- **Variable-length entries.** No — would require a name-table
  region and walking-by-byte parsing. OSFS is supposed to be
  unsubtle.
- **More directory sectors.** Yes. Read sector 1 *and* sector 2,
  bump first-data to sector 3.

The new layout:

```
sector 0     superblock
sector 1..2  directory       (32 dirents × 32 B = exactly 1024 B)
sector 3..N  file data
```

The kernel side only needs:

- `OSFS_MAX_FILES = 32`
- `OSFS_DIR_SECTORS = 2` (new constant)
- `OSFS_FIRST_DATA_SECTOR = 3` (new constant)
- A small loop in `osfs_init` that reads `OSFS_DIR_SECTORS`
  sectors instead of one:

```c
static uint8_t dir[SECTOR * OSFS_DIR_SECTORS];
for (uint32_t s = 0; s < OSFS_DIR_SECTORS; s++) {
    if (blk_cache_read(OSFS_DIR_SECTOR + s, dir + s * SECTOR) != 0) {
        serial_puts("[osfs] directory read failed\n");
        return -1;
    }
}
```

`mkosfs.py` mirrors the change: `DIR_SECTORS = 2`,
`FIRST_DATA_SECTOR = 3`, `MAX_FILES = (SECTOR * DIR_SECTORS) /
DIRENT.size`. Same layout on both sides. Total cost: about ten
lines.

### Forward compatibility

Old images (built with the chapter-12 layout) start their data
at sector 2 and have file_count ≤ 16. The new reader will:
- read sector 1 (correct) and sector 2 (would have been file
  data in the old format) and try to interpret the second
  sector as 16 more dirents.
- Some of those bytes happen to look like garbage dirents whose
  names contain non-NUL noise. That's fine because we cap
  iteration at `file_count`.

So the new reader is backward-compatible with old images
*because we trust file_count*. No magic bump needed.

## The four tools

Each follows the chapter-13 user-binary template (`crt0.o` +
`linker_user.ld`, embedded into the OSFS image, looked up via
`/bin/<name>`).

### `grep PATTERN PATH`

Substring match, line at a time. Reads in 256-byte chunks,
buffers a line of up to 512 bytes, prints lines that contain
PATTERN as a literal substring. No regex — that's a separate
chapter (state machines or Aho-Corasick). No `-i`, `-v`, `-n`,
no recursion.

```c
static int contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}
```

Naive O(n·m) but everything in OSFS is < 64 KiB so it doesn't
matter.

### `wc PATH`

Single-pass: count bytes (always), newlines (`'\n'`), and word
*starts* (transitions from whitespace to non-whitespace).
Output mimics POSIX:

```
14 83 569 /mnt/poem.txt
```

No `-l`, `-w`, `-c` flags — the user can squint at three
columns.

### `head [-N] PATH`

Streams the file, prints bytes to stdout until it has emitted N
newlines. Stops reading from the FS as soon as the count is hit
— so on a giant file you don't pay for the tail. (Sized in
terms of "lines we've completed by emitting their `\n`", which
matches POSIX.)

### `tail [-N] PATH`

The interesting one. Without `lseek`, we can't seek to the end.
Strategy:

- **Pass 1**: stream the whole file, recording the byte offsets
  of the last (N+1) newlines in a small ring buffer of
  `long`s. Also note `total_lines` and whether the file ends in
  `\n`.
- **Pass 2**: open the file again, skip-read until the chosen
  start offset, then dump the rest to stdout.

Reading the file twice is fine for our < 64 KiB files. When we
get a real `lseek` (or `mmap`), the first pass becomes a
backwards scan.

The ring buffer is sized at `MAX_N + 1` so that we always have
the offset of the line *before* the first kept line — that's
where playback begins. Edge cases:

- File has fewer lines than N: start at offset 0 (print
  everything).
- File doesn't end in `\n`: count one extra "logical line" for
  the trailing fragment, and write a `\n` ourselves at the end
  so the prompt lands on a fresh line. Same dance as `cat`.

## Verification

```
/$ ls
... 18 files including grep, wc, head, tail ...

/$ wc /mnt/poem.txt
14 83 569 /mnt/poem.txt

/$ grep the /mnt/poem.txt
If you can read this through the kernel's OSFS-1 mount,
the chain works:
  back up the chain to write(1) -> PL011 -> your terminal.
For the moment everything is read-only; write support
needs a free-space map and a way to grow files past their

/$ head -3 /mnt/poem.txt
If you can read this through the kernel's OSFS-1 mount,
the chain works:

/$ tail -3 /mnt/poem.txt
For the moment everything is read-only; write support
needs a free-space map and a way to grow files past their
initially-allocated sector range.
```

`ls` shows 18 files. `wc` matches the FS-reported size of
`poem.txt` (569 bytes). `grep` finds five lines, `head -3`
prints three, `tail -3` prints three.

## What this unlocks

Pipes, when they arrive, will turn each of these into building
blocks for arbitrary text-shaping pipelines. `grep PAT *.txt |
wc -l` is the obvious next step. Today these tools are useful
in their own right; tomorrow they'll be glue.

## What's still missing

- **Pipes** (`a | b`). Needs `THREAD_BLOCKED`, fd inheritance
  through spawn, `dup2` to wire pipe ends to stdin/stdout.
  Multi-day effort.
- **stdin from a non-TTY.** Even without pipes, redirection
  from a file (`cmd < /motd`) would give these tools a way to
  read from non-arg paths.
- **Regex.** `grep -E` needs an NFA/DFA pass; deferred.
- **`-l/-w/-c` flags on wc, `-n/-v/-i` on grep, `-c` on
  head/tail.** All trivial to add when needed.
- **Writable FS.** `tail -f` is meaningless if files can't grow.

## What changed

```
kernel/core/osfs.h             OSFS_MAX_FILES 16->32, +OSFS_DIR_SECTORS,
                                +OSFS_FIRST_DATA_SECTOR
kernel/core/osfs.c             read OSFS_DIR_SECTORS in osfs_init
scripts/mkosfs.py              DIR_SECTORS=2, FIRST_DATA_SECTOR=3,
                                MAX_FILES derived from layout
userspace/grep/grep.c          new (~70 LOC)
userspace/wc/wc.c              new (~50 LOC)
userspace/head/head.c          new (~70 LOC)
userspace/tail/tail.c          new (~140 LOC)
userspace/sh/sh.c              help text mentions new tools
Makefile                       OBJS/ELF/STRIPPED/OSFS for the four
                                new binaries
```

Eighteen files in the OSFS image now, room for fourteen more.

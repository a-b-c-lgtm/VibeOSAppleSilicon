#!/usr/bin/env python3
"""scripts/mkosfs2.py - format an OSFS-2 disk image.

OSFS-2 is the writable on-disk filesystem introduced in chapter 81.
This tool produces a freshly-formatted image suitable for the kernel
to mount on the second virtio-blk device (`build/data.img`).

Layout (matches kernel/core/osfs2.h):

    Block size = 4096 bytes (eight 512-byte virtio sectors).

    Block 0           superblock
    Block 1           block bitmap (1 bit per block)
    Block 2           inode bitmap (1 bit per inode)
    Block 3..66       inode table (64 blocks * 32 inodes/block = 2048 inodes)
    Block 67          journal header                       (chapter 83)
    Block 68..99      journal data slots (32 × 4 KiB)      (chapter 83)
    Block 100..N-1    data blocks

Inode (128 bytes):
    u32 type            0=free, 1=file, 2=dir
    u32 size            in bytes
    u32 nlink           hard-link count (always 1 in chapter 81)
    u32 mode            permission bits (cosmetic)
    u32 ctime_ms
    u32 mtime_ms
    u32 direct[16]      direct block pointers (each = 4 KiB)
    u32 indirect        single-indirect block (1024 ptrs)
    u8  reserved[36]    pad to 128

Dirent (64 bytes):
    u32  ino            0 = empty slot
    char name[60]       NUL-padded

The root directory is inode 1 with one preallocated data block holding
up to 64 dirents.  The tool can optionally seed files into the root via
`name=path` arguments, mirroring the mkosfs.py interface.

Usage:
    scripts/mkosfs2.py OUTPUT.img [name1=path1 name2=path2 ...]

A formatted-but-empty image is produced when no `name=path` pairs are
given.  Names must be <= 59 bytes (room for the NUL).
"""

import struct
import sys
from pathlib import Path

BLOCK_SIZE         = 4096
TOTAL_BLOCKS       = 16384            # 64 MiB image
INODE_COUNT        = 2048
INODE_SIZE         = 128
INODES_PER_BLOCK   = BLOCK_SIZE // INODE_SIZE     # 32
INODE_TABLE_BLOCKS = INODE_COUNT // INODES_PER_BLOCK   # 64

BLOCK_BITMAP_BLOCK = 1
INODE_BITMAP_BLOCK = 2
INODE_TABLE_BLOCK  = 3
# Chapter 83 — single-active-transaction physical-block journal.
# One header block + one data slot per cache slot (32, see
# kernel/core/osfs2_cache.c).  Sized to match exactly the maximum
# transaction the cache can present at flush time.
JOURNAL_HEADER_BLOCK = INODE_TABLE_BLOCK + INODE_TABLE_BLOCKS  # 67
JOURNAL_DATA_BLOCKS  = 32
JOURNAL_TOTAL_BLOCKS = 1 + JOURNAL_DATA_BLOCKS                  # 33
DATA_START_BLOCK   = JOURNAL_HEADER_BLOCK + JOURNAL_TOTAL_BLOCKS  # 100
ROOT_INODE         = 1

DIRENT_SIZE        = 64
DIRENTS_PER_BLOCK  = BLOCK_SIZE // DIRENT_SIZE    # 64

# Superblock layout (chapter 83 extended): magic[8] + 12 × u32.
#   block_size, total_blocks, inode_count,
#   block_bitmap_block, inode_bitmap_block,
#   inode_table_block,  inode_table_blocks,
#   data_start_block,   root_inode,
#   journal_header_block, journal_data_blocks, reserved
SUPERBLOCK_PACK = struct.Struct("<8s12I")

# Inode 'type' field encoding (matches kernel/core/osfs2.h enum).
T_FREE = 0
T_FILE = 1
T_DIR  = 2

# Per-spec packing.  The inode is 5 u32 prefix + 16 + 1 u32 + 36 pad.
INODE_FMT = "<IIIIIII16II36s"  # type,size,nlink,mode,ctime,mtime,_align... no
# Actually let's be explicit:
#   type, size, nlink, mode, ctime_ms, mtime_ms : 6 u32 = 24 bytes
#   direct[16]                                  : 16 u32 = 64 bytes -> 88
#   indirect                                    : 1 u32  =  4 bytes -> 92
#   reserved[36]                                : 36 bytes        -> 128
INODE_PACK = struct.Struct("<6I16II36s")
DIRENT_PACK = struct.Struct("<I60s")


def format_image(out: Path, files):
    if len(files) > DIRENTS_PER_BLOCK:
        sys.exit(f"too many seed files (max {DIRENTS_PER_BLOCK})")

    image = bytearray(TOTAL_BLOCKS * BLOCK_SIZE)

    # Superblock (block 0).
    sb = SUPERBLOCK_PACK.pack(
        b"OSFS-002",
        BLOCK_SIZE,
        TOTAL_BLOCKS,
        INODE_COUNT,
        BLOCK_BITMAP_BLOCK,
        INODE_BITMAP_BLOCK,
        INODE_TABLE_BLOCK,
        INODE_TABLE_BLOCKS,
        DATA_START_BLOCK,
        ROOT_INODE,
        JOURNAL_HEADER_BLOCK,    # chapter 83
        JOURNAL_DATA_BLOCKS,     # chapter 83
        0,                       # reserved
    )
    image[0 : len(sb)] = sb

    # Block bitmap (block 1).  Bit i = block i is allocated.
    block_bitmap = bytearray(BLOCK_SIZE)

    def alloc_block(b):
        block_bitmap[b // 8] |= (1 << (b % 8))

    # Mark metadata blocks as allocated.
    for b in range(0, DATA_START_BLOCK):
        alloc_block(b)

    # Reserve the root directory's first data block.
    root_block = DATA_START_BLOCK
    alloc_block(root_block)
    next_data_block = root_block + 1

    # Inode bitmap (block 2).  Bit i = inode i is allocated.
    # Inode 0 is reserved as the null sentinel.
    inode_bitmap = bytearray(BLOCK_SIZE)

    def alloc_inode(i):
        inode_bitmap[i // 8] |= (1 << (i % 8))

    alloc_inode(0)            # null sentinel
    alloc_inode(ROOT_INODE)   # root directory

    # Inode table.  We'll fill entries as we go.
    inode_table = bytearray(INODE_TABLE_BLOCKS * BLOCK_SIZE)

    def write_inode(ino, inode_bytes):
        assert len(inode_bytes) == INODE_SIZE
        off = ino * INODE_SIZE
        inode_table[off : off + INODE_SIZE] = inode_bytes

    # Build the root directory dirent buffer.  We accumulate up to 64
    # entries and write the block at the end.
    root_dirents = bytearray(BLOCK_SIZE)
    dirent_index = 0

    def append_dirent(ino, name):
        nonlocal dirent_index
        if dirent_index >= DIRENTS_PER_BLOCK:
            sys.exit("root directory full")
        name_bytes = name.encode()
        if len(name_bytes) >= 60:
            sys.exit(f"name too long (>= 60 bytes): {name}")
        ent = DIRENT_PACK.pack(ino, name_bytes.ljust(60, b"\0"))
        off = dirent_index * DIRENT_SIZE
        root_dirents[off : off + DIRENT_SIZE] = ent
        dirent_index += 1

    # Walk seed files, allocate inodes + data blocks for each.
    next_inode = ROOT_INODE + 1
    for spec in files:
        if "=" not in spec:
            sys.exit(f"bad spec {spec!r}, expected name=path")
        name, src_path = spec.split("=", 1)
        data = Path(src_path).read_bytes()
        if len(data) > 16 * BLOCK_SIZE + 1024 * BLOCK_SIZE:
            sys.exit(f"file too large for OSFS-2: {name} ({len(data)} bytes)")

        ino = next_inode
        next_inode += 1
        if ino >= INODE_COUNT:
            sys.exit("ran out of inodes")
        alloc_inode(ino)

        # Place data into direct blocks first, then indirect.
        direct = [0] * 16
        indirect = 0
        bytes_left = len(data)
        cursor = 0
        # Direct blocks.
        for i in range(16):
            if bytes_left <= 0:
                break
            blk = next_data_block
            next_data_block += 1
            if next_data_block > TOTAL_BLOCKS:
                sys.exit("data section full")
            alloc_block(blk)
            chunk = data[cursor : cursor + BLOCK_SIZE]
            image[blk * BLOCK_SIZE : blk * BLOCK_SIZE + len(chunk)] = chunk
            direct[i] = blk
            cursor += len(chunk)
            bytes_left -= len(chunk)
        # Indirect block (if needed).
        if bytes_left > 0:
            ind_blk = next_data_block
            next_data_block += 1
            alloc_block(ind_blk)
            ind_table = bytearray(BLOCK_SIZE)
            ind_idx = 0
            while bytes_left > 0:
                if ind_idx >= 1024:
                    sys.exit(f"file too large for single-indirect: {name}")
                blk = next_data_block
                next_data_block += 1
                if next_data_block > TOTAL_BLOCKS:
                    sys.exit("data section full")
                alloc_block(blk)
                chunk = data[cursor : cursor + BLOCK_SIZE]
                image[blk * BLOCK_SIZE : blk * BLOCK_SIZE + len(chunk)] = chunk
                struct.pack_into("<I", ind_table, ind_idx * 4, blk)
                cursor += len(chunk)
                bytes_left -= len(chunk)
                ind_idx += 1
            image[ind_blk * BLOCK_SIZE : ind_blk * BLOCK_SIZE + BLOCK_SIZE] = ind_table
            indirect = ind_blk

        # Write the inode.
        write_inode(ino, INODE_PACK.pack(
            T_FILE, len(data), 1, 0o644, 0, 0,
            *direct, indirect, b"\0" * 36,
        ))

        append_dirent(ino, name)
        print(f"  ino {ino:4d}  {name:30s}  size {len(data):8d} bytes")

    # Write the root directory inode now that we know how many dirents
    # are present.  Root directory size is dirents * DIRENT_SIZE.
    root_size = dirent_index * DIRENT_SIZE
    root_direct = [0] * 16
    root_direct[0] = root_block
    write_inode(ROOT_INODE, INODE_PACK.pack(
        T_DIR, root_size, 1, 0o755, 0, 0,
        *root_direct, 0, b"\0" * 36,
    ))

    # Place root dirents block.
    image[root_block * BLOCK_SIZE : root_block * BLOCK_SIZE + BLOCK_SIZE] = root_dirents

    # Place the bitmaps and inode table.
    image[BLOCK_BITMAP_BLOCK * BLOCK_SIZE : BLOCK_BITMAP_BLOCK * BLOCK_SIZE + BLOCK_SIZE] = block_bitmap
    image[INODE_BITMAP_BLOCK * BLOCK_SIZE : INODE_BITMAP_BLOCK * BLOCK_SIZE + BLOCK_SIZE] = inode_bitmap
    image[INODE_TABLE_BLOCK * BLOCK_SIZE :
          INODE_TABLE_BLOCK * BLOCK_SIZE + INODE_TABLE_BLOCKS * BLOCK_SIZE] = inode_table

    out.write_bytes(image)
    used_data = next_data_block - DATA_START_BLOCK
    print(f"wrote {out} ({TOTAL_BLOCKS * BLOCK_SIZE} bytes, "
          f"{dirent_index} files, {used_data}/{TOTAL_BLOCKS - DATA_START_BLOCK} data blocks used)")


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(1)
    out = Path(sys.argv[1])
    files = sys.argv[2:]
    format_image(out, files)


if __name__ == "__main__":
    main()

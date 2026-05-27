#!/usr/bin/env python3
"""scripts/mkosfs.py — build a 1 MiB OSFS-1 image from a list of input files.

Layout (matches kernel/core/osfs.h):

    sector 0  superblock:
        0x00  magic     = b"OSFS-001"  (8 bytes)
        0x08  file_count: u32 le
        0x0C..end       = 0

    sector 1..8  directory: array of struct osfs_dirent (32 bytes each),
                 eight sectors -> 128 entries max (chapter 110 bump;
                 was 4 sectors / 64 entries through ch106a,
                 and 2 / 32 before that)
        char     name[20]      (NUL-padded)
        uint32_t start_sector  (LBA)
        uint32_t size_bytes
        uint32_t reserved      = 0

    sector 9..N  file data, each file starts on a sector boundary.

Usage:
    scripts/mkosfs.py OUTPUT.img name1=path1 name2=path2 ...

Names must be <= 19 bytes (room for the NUL).
"""

import struct
import sys
from pathlib import Path

SECTOR = 512
TOTAL_SECTORS = 524288    # 256 MiB image.  Chapter 180 swapped
                          # the toy 12 KiB /bin/as + 7 KiB /bin/ld
                          # for the real GNU binutils gas-new
                          # (~3.3 MiB) + ld-new (~2.9 MiB), and
                          # together with the 8.3 MiB
                          # wallpaper.bgra that pushed the
                          # previous 16 MiB image over the line.
                          # Chapter 186 bumped 32 -> 256 MiB to make
                          # room for /bin/gcc (3 MiB xgcc driver) +
                          # /bin/cc1 (43 MiB stripped, the C frontend
                          # + middle/back-end + libbackend.a code).
                          # lto1 and lto-dump (~52 MiB each stripped)
                          # are NOT shipped — they're only useful when
                          # building gcc itself with -flto, which we
                          # don't do in-guest.
DIRENT = struct.Struct("<20sIII")
DIR_SECTORS = 16          # sixteen sectors of dirents -> 256 files
                          # (was 2 / 32 early on, 4 / 64 through ch106a,
                          # bumped in chapter 110 for proxytest, then
                          # again in chapter 189 so the libc headers
                          # could ship on /bin for in-guest gcc)
FIRST_DATA_SECTOR = 17    # superblock = 0, directory = 1..16, data = 17..
MAX_FILES = (SECTOR * DIR_SECTORS) // DIRENT.size  # 256

def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        sys.exit(1)

    out = Path(sys.argv[1])
    files = []
    for spec in sys.argv[2:]:
        if "=" not in spec:
            sys.exit(f"bad spec {spec!r}, expected name=path")
        name, path = spec.split("=", 1)
        if len(name.encode()) > 19:
            sys.exit(f"name too long (> 19 bytes): {name}")
        files.append((name, Path(path)))

    if len(files) > MAX_FILES:
        sys.exit(f"too many files (max {MAX_FILES})")

    image = bytearray(TOTAL_SECTORS * SECTOR)

    # Superblock.
    image[0:8] = b"OSFS-001"
    struct.pack_into("<I", image, 8, len(files))

    # Place files starting at sector 2.
    cursor = FIRST_DATA_SECTOR
    dir_off = SECTOR  # sector 1
    for i, (name, path) in enumerate(files):
        data = path.read_bytes()
        nsect = (len(data) + SECTOR - 1) // SECTOR
        if cursor + nsect > TOTAL_SECTORS:
            sys.exit(f"image full: cannot fit {name} ({len(data)} bytes)")
        # Write data.
        image[cursor*SECTOR:cursor*SECTOR + len(data)] = data
        # Write directory entry.
        ent = DIRENT.pack(
            name.encode().ljust(20, b"\0"),
            cursor,
            len(data),
            0,
        )
        image[dir_off + i*DIRENT.size : dir_off + (i+1)*DIRENT.size] = ent
        print(f"  {name:20s} sector {cursor:4d}  size {len(data):6d} bytes "
              f"(spans {nsect} sectors)")
        cursor += nsect

    out.write_bytes(image)
    print(f"wrote {out} ({TOTAL_SECTORS*SECTOR} bytes, "
          f"{len(files)} files, {cursor-FIRST_DATA_SECTOR} sectors used)")

if __name__ == "__main__":
    main()

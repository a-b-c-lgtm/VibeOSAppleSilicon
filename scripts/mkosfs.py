#!/usr/bin/env python3
"""scripts/mkosfs.py — build a 1 MiB OSFS-1 image from a list of input files.

Layout (matches kernel/core/osfs.h):

    sector 0  superblock:
        0x00  magic     = b"OSFS-001"  (8 bytes)
        0x08  file_count: u32 le
        0x0C..end       = 0

    sector 1..2  directory: array of struct osfs_dirent (32 bytes each),
                 two sectors -> 32 entries max
        char     name[20]      (NUL-padded)
        uint32_t start_sector  (LBA)
        uint32_t size_bytes
        uint32_t reserved      = 0

    sector 3..N  file data, each file starts on a sector boundary.

Usage:
    scripts/mkosfs.py OUTPUT.img name1=path1 name2=path2 ...

Names must be <= 19 bytes (room for the NUL).
"""

import struct
import sys
from pathlib import Path

SECTOR = 512
TOTAL_SECTORS = 32768     # 16 MiB image (room for binaries + a
                          # 1920x1080 wallpaper.bgra ~= 8.3 MB
                          # with header; bump higher if you add
                          # more large data files)
DIRENT = struct.Struct("<20sIII")
DIR_SECTORS = 4           # four sectors of dirents -> 64 files (was 2 / 32 pre-M60)
FIRST_DATA_SECTOR = 5     # superblock = 0, directory = 1..4, data = 5..
MAX_FILES = (SECTOR * DIR_SECTORS) // DIRENT.size  # 64

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

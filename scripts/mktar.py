#!/usr/bin/env python3
"""scripts/mktar.py -- build a ustar archive from a directory tree.

Chapter 133a.  Produces tarballs that /bin/tar can extract
in-guest: ustar format, no GNU extensions, no compression.

Why not just shell out to /usr/bin/tar?

  * macOS ships BSD tar by default, Linux ships GNU tar;
    their default header dialect (ustar vs. pax) differs.
    We want a single deterministic byte layout regardless of
    the build host.

  * Python's tarfile module supports `format=USTAR_FORMAT`
    explicitly, which gives us the simplest possible header
    layout (no PAX records, no '@LongLink' splits) -- the
    same layout /bin/tar parses.

  * We can normalise owner/group/mtime to fixed values so the
    archive is bit-identical across rebuilds, which keeps
    smoke-test golden-byte comparisons stable.

Usage:
    scripts/mktar.py OUT.tar SRCDIR [name-in-archive]
"""

import sys
import tarfile
from pathlib import Path


def build(out: Path, src: Path, name: str) -> int:
    if not src.is_dir():
        sys.exit(f"mktar: source {src!r} is not a directory")

    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as t:
        # `recursive=True` is the default but spell it out for the
        # reader.  Sort entries so the archive layout is stable
        # across filesystems that hand back inodes in different
        # orders (notably APFS vs ext4 vs HFS+).
        entries = sorted(src.rglob("*"))
        # First the root entry, then everything underneath.
        root = tarfile.TarInfo(name=name)
        root.type = tarfile.DIRTYPE
        root.mode = 0o755
        root.mtime = 0
        root.uid = 0
        root.gid = 0
        root.uname = ""
        root.gname = ""
        t.addfile(root)

        for p in entries:
            rel = p.relative_to(src)
            arcname = f"{name}/{rel.as_posix()}"
            info = t.gettarinfo(p, arcname=arcname)
            # Strip variable metadata for determinism.
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            if info.isreg():
                info.mode = 0o644
                with p.open("rb") as fh:
                    t.addfile(info, fh)
            elif info.isdir():
                info.mode = 0o755
                t.addfile(info)
            else:
                # Symlinks, devices, etc. -- skip silently.  The
                # doomgeneric source tree doesn't have any.
                print(f"mktar: skipping non-regular {arcname}",
                      file=sys.stderr)
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) not in (3, 4):
        print("usage: mktar.py OUT.tar SRCDIR [name-in-archive]",
              file=sys.stderr)
        return 2
    out = Path(argv[1])
    src = Path(argv[2])
    name = argv[3] if len(argv) == 4 else src.name
    return build(out, src, name)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

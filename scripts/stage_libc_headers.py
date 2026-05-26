#!/usr/bin/env python3
"""scripts/stage_libc_headers.py -- chapter 132j staging tool.

OSFS-1 is a flat namespace, but our user-facing libc headers
in `userspace/libc/sys/*.h` use `#include "../foo.h"` to reach
their siblings.  When those files are shipped on `/bin/sys/`
(via the literal-name `sys/foo.h` trick), `"../foo.h"` would
resolve to `/bin/sys/../foo.h` -- which our kernel's path
resolver does not normalise.

Solution: stage the headers into `build/staged-libc-headers/`
with `"../foo.h"` rewritten to `<foo.h>`.  Angle-bracket
includes go through cpp's system search path (which is
`-isystem /bin`), so they resolve cleanly to `/bin/foo.h`
without any `..` traversal.

This script is invoked from the Makefile.  Source headers are
left untouched; only the staged copies are rewritten.

Usage:
    python3 scripts/stage_libc_headers.py SRC DST

Rewrites #include "../<file>" -> #include <<file>>.  All
other includes (and source content) are copied verbatim.
"""

import re
import sys
from pathlib import Path

# Matches:  #include "../foo.h"
#          ^                 ^^
# Captures the bare filename (no '..', no quotes).
_REL_PARENT = re.compile(r'#(\s*)include\s+"\.\./([^"/]+)"')


def stage(src_path: Path, dst_path: Path) -> None:
    text = src_path.read_text(encoding="utf-8")
    out = _REL_PARENT.sub(r'#\1include <\2>', text)
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(out, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    stage(Path(sys.argv[1]), Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

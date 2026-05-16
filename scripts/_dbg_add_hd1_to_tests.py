#!/usr/bin/env python3
"""scripts/_dbg_add_hd1_to_tests.py — chapter 81 helper.

One-off mass edit that added the OSFS-2 disk (`build/data.img`) as
the second virtio-blk device (hd1) to every test/snoop/dbg script
that already attaches the OSFS-1 disk as hd0.  Kept in tree per
the project's "always keep your debug scripts" policy so future
chapters can repeat the same surgery if we ever add a third disk.

Idempotent: scripts that already mention `virtio-blk-device,drive=hd1`
are skipped.  Run from the repo root.
"""
import re
import subprocess
import sys


def main():
    out = subprocess.check_output(
        ["grep", "-l", "virtio-blk-device,drive=hd0", "-r", "scripts"]
    ).decode().splitlines()
    print(f"files matching: {len(out)}")
    inserted = 0
    for f in out:
        src = open(f).read()
        if "virtio-blk-device,drive=hd1" in src:
            print(f"  skip (already has hd1): {f}")
            continue
        # Match the existing "-device", "virtio-blk-device,drive=hd0",
        # line and preserve the leading whitespace, then insert two
        # new lines for the second drive immediately after it.
        new = re.sub(
            r'^([ \t]*)"-device"\s*,\s*"virtio-blk-device,drive=hd0",\s*\n',
            r'\1"-device", "virtio-blk-device,drive=hd0",\n'
            r'\1"-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",\n'
            r'\1"-device", "virtio-blk-device,drive=hd1",\n',
            src,
            flags=re.MULTILINE,
        )
        if new != src:
            open(f, "w").write(new)
            inserted += 1
            print(f"  updated: {f}")
        else:
            print(f"  NO CHANGE: {f}")
    print(f"updated {inserted} files")


if __name__ == "__main__":
    sys.exit(main())

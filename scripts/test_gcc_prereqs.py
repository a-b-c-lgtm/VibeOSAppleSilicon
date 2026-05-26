#!/usr/bin/env python3
# scripts/test_gcc_prereqs.py — chapter 132b host smoke test.
#
# NOT added to scripts/sweep.sh because this only exercises
# host-side vendoring.  Run manually after editing
# scripts/fetch_gcc_prereqs.sh or any of the pinned tarball
# versions.
#
# What it does:
#   1. Skip with a clear message if vendor/gcc-14.2.0/.prereqs-osdev
#      is absent (developer hasn't run fetch_gcc_prereqs.sh yet).
#   2. Assert that vendor/gcc-14.2.0/{gmp,mpfr,mpc} all exist and
#      are symlinks pointing at the expected vendor/<pkg>-<ver>/
#      directories.
#   3. Assert each linked directory contains a `configure` script
#      (i.e. it's a real autotools project, not a stale shell).
#   4. Assert each pinned tarball still has the expected sha256
#      (catches silent corruption / partial downloads).
#   5. Print PASS, exit 0.  Any mismatch exits 1 with FAIL.
#
# We deliberately don't try to ./configure or build the libs
# here — that's chapter 132c's job (it happens automatically as
# part of `make all-gcc` once xgcc is built in-tree).

import hashlib
import os
import sys

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GCC_SRC  = os.path.join(ROOT, "vendor", "gcc-14.2.0")
MARKER   = os.path.join(GCC_SRC, ".prereqs-osdev")

# (link name in gcc-src, expected target rel-path, tarball, sha256)
PKGS = [
    ("gmp",  "../gmp-6.2.1",
     "gmp-6.2.1.tar.bz2",
     "eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c"),
    ("mpfr", "../mpfr-4.1.0",
     "mpfr-4.1.0.tar.bz2",
     "feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926"),
    ("mpc",  "../mpc-1.2.1",
     "mpc-1.2.1.tar.gz",
     "17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459"),
]


def fail(msg: str) -> None:
    print(f"gcc_prereqs: FAIL — {msg}", file=sys.stderr)
    sys.exit(1)


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    if not os.path.exists(MARKER):
        print(f"gcc_prereqs: SKIP — {MARKER} not present "
              f"(run `bash scripts/fetch_gcc_prereqs.sh` first)")
        sys.exit(0)

    for linkname, expected_target, tarball, want_sha in PKGS:
        link = os.path.join(GCC_SRC, linkname)
        if not os.path.islink(link):
            fail(f"{link} is not a symlink (expected -> {expected_target})")
        actual_target = os.readlink(link)
        if actual_target != expected_target:
            fail(f"{link} -> {actual_target!r}, want {expected_target!r}")

        ext_dir = os.path.join(GCC_SRC, expected_target)
        if not os.path.isdir(ext_dir):
            fail(f"symlink target {ext_dir} is not a directory")
        if not os.path.isfile(os.path.join(ext_dir, "configure")):
            fail(f"{ext_dir} has no `configure` script — bad extract?")

        tar_path = os.path.join(ROOT, "vendor", tarball)
        if not os.path.isfile(tar_path):
            fail(f"missing tarball {tar_path}")
        got_sha = sha256(tar_path)
        if got_sha != want_sha:
            fail(f"{tarball} sha256 {got_sha}, want {want_sha}")

    print("gcc_prereqs: PASS — gmp / mpfr / mpc tarballs verified, "
          "extracted, and symlinked into vendor/gcc-14.2.0/")
    sys.exit(0)


if __name__ == "__main__":
    main()

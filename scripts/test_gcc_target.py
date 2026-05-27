#!/usr/bin/env python3
# scripts/test_gcc_target.py — chapter 181 host-tool smoke test.
#
# NOT added to scripts/sweep.sh because this exercises host
# source files, not the kernel.  Run manually when you suspect
# vendor/gcc-aarch64-osdev.patch is broken (after a fresh
# clone + `bash scripts/fetch_gcc.sh`, or after editing the
# patch).
#
# What it does, in order:
#   1. Skip with a clear message if vendor/gcc-14.2.0/.patched-osdev
#      doesn't exist (developer hasn't fetched + patched the
#      source tree yet).
#   2. Run the patched gcc/config.sub against `aarch64-osdev` and
#      assert it canonicalises to `aarch64-unknown-osdev`.  This
#      is the same gate fetch_gcc.sh enforces, but having it in
#      a regression script means future patch breakage shows up
#      here without needing a re-fetch.
#   3. Read gcc/config.gcc and assert the `aarch64-*-osdev*)` arm
#      is present, with the tm_file chain pointing at our
#      `aarch64/aarch64-osdev.h`.
#   4. Read libgcc/config.host and assert the `aarch64*-*-osdev*)`
#      arm is present.
#   5. Assert gcc/config/aarch64/aarch64-osdev.h exists and
#      contains TARGET_OS_CPP_BUILTINS with __osdev__.
#   6. Print PASS and exit 0.  Anything else exits 1 with FAIL.
#
# We don't try to actually configure or build the cross compiler
# here — that's chapter 182's job (it needs GMP/MPFR/MPC, which
# 132a deliberately defers).  Chapter 181's contract is only
# that the source tree's configure machinery KNOWS the triple.

import os
import subprocess
import sys

ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC        = os.path.join(ROOT, "vendor", "gcc-14.2.0")
MARKER     = os.path.join(SRC, ".patched-osdev")
CONFIG_SUB = os.path.join(SRC, "config.sub")
CONFIG_GCC = os.path.join(SRC, "gcc", "config.gcc")
CONFIG_HST = os.path.join(SRC, "libgcc", "config.host")
OSDEV_H    = os.path.join(SRC, "gcc", "config", "aarch64", "aarch64-osdev.h")


def fail(msg: str) -> None:
    print(f"gcc_target: FAIL — {msg}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    if not os.path.exists(MARKER):
        print(f"gcc_target: SKIP — {MARKER} not present "
              f"(run `bash scripts/fetch_gcc.sh` first)")
        sys.exit(0)

    # 1. config.sub canonicalisation.
    r = subprocess.run(["bash", CONFIG_SUB, "aarch64-osdev"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"config.sub aarch64-osdev rc={r.returncode}: "
             f"{r.stderr.strip()}")
    got = r.stdout.strip()
    if got != "aarch64-unknown-osdev":
        fail(f"config.sub canonical form {got!r}, "
             f"want 'aarch64-unknown-osdev'")

    # 2. gcc/config.gcc arm.
    with open(CONFIG_GCC) as f:
        cg = f.read()
    if "aarch64-*-osdev*)" not in cg:
        fail("gcc/config.gcc lacks the aarch64-*-osdev*) arm")
    if "aarch64/aarch64-osdev.h" not in cg:
        fail("gcc/config.gcc does not reference aarch64-osdev.h "
             "in its tm_file chain")

    # 3. libgcc/config.host arm.
    with open(CONFIG_HST) as f:
        ch = f.read()
    if "aarch64*-*-osdev*)" not in ch:
        fail("libgcc/config.host lacks the aarch64*-*-osdev*) arm")

    # 4. New header file.
    if not os.path.exists(OSDEV_H):
        fail(f"missing new header {OSDEV_H}")
    with open(OSDEV_H) as f:
        oh = f.read()
    for needle in ("TARGET_OS_CPP_BUILTINS",
                   "__osdev__",
                   "system=osdev",
                   "system=unix"):
        if needle not in oh:
            fail(f"aarch64-osdev.h missing required text {needle!r}")

    print("gcc_target: PASS — config.sub canonicalises "
          "aarch64-osdev → aarch64-unknown-osdev; gcc/config.gcc + "
          "libgcc/config.host carry the aarch64-osdev arm; "
          "aarch64-osdev.h defines __osdev__")
    sys.exit(0)


if __name__ == "__main__":
    main()

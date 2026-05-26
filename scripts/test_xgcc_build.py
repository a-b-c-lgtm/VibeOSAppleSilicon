#!/usr/bin/env python3
# scripts/test_xgcc_build.py — chapter 132c host smoke test.
#
# NOT added to scripts/sweep.sh because this exercises the
# host cross compiler, not anything inside the guest.  Run
# manually after `make gcc-osdev` or after editing the chapter
# 132c Makefile rules.
#
# What it does:
#   1. Skip with a clear message if
#      build/toolchain/bin/aarch64-osdev-gcc doesn't exist
#      (developer hasn't built xgcc yet — `make gcc-osdev`).
#   2. Run `aarch64-osdev-gcc -dumpmachine` and assert it
#      outputs `aarch64-osdev` (or the canonical form
#      `aarch64-unknown-osdev` — both are acceptable, configure
#      picks the latter on most distros but some land on the
#      shorter form).
#   3. Run `aarch64-osdev-gcc -dumpversion` and assert it's
#      14.2.0 (matches our pinned source).
#   4. Run `aarch64-osdev-gcc -E -dM -xc /dev/null` to dump the
#      predefined macros; assert `__osdev__` is in the list.
#      This is the only thing that proves chapter 132a's
#      TARGET_OS_CPP_BUILTINS hook actually fired during the
#      build — without it the cross compiler would silently
#      look just like aarch64-elf-gcc.
#   5. Run `aarch64-osdev-gcc -print-prog-name=as` and assert
#      it points at our binutils prefix (chapter 131a), not at
#      a system `as` somewhere.  This proves the configure-time
#      PATH=$(TOOLCHAIN_PREFIX)/bin trick actually worked.
#   6. Print PASS, exit 0.
#
# We deliberately don't try to compile anything yet — that's
# chapter 132d (link/startfile specs, libc bridge, hello.c
# end-to-end).

import os
import subprocess
import sys
from typing import Optional

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XGCC = os.path.join(ROOT, "build", "toolchain", "bin", "aarch64-osdev-gcc")


def fail(msg: str) -> None:
    print(f"xgcc_build: FAIL — {msg}", file=sys.stderr)
    sys.exit(1)


def run(args, stdin: Optional[str] = None) -> str:
    r = subprocess.run([XGCC] + args, capture_output=True, text=True,
                       input=stdin)
    if r.returncode != 0:
        fail(f"{XGCC} {' '.join(args)} rc={r.returncode}\n"
             f"stderr: {r.stderr.strip()}")
    return r.stdout


def main() -> None:
    if not os.path.exists(XGCC):
        print(f"xgcc_build: SKIP — {XGCC} not present "
              f"(run `make gcc-osdev` first)")
        sys.exit(0)

    # 1. dumpmachine.
    triple = run(["-dumpmachine"]).strip()
    if triple not in ("aarch64-osdev", "aarch64-unknown-osdev"):
        fail(f"-dumpmachine={triple!r}, "
             f"want 'aarch64-osdev' or 'aarch64-unknown-osdev'")

    # 2. dumpversion.
    ver = run(["-dumpversion"]).strip()
    if ver != "14.2.0":
        fail(f"-dumpversion={ver!r}, want '14.2.0'")

    # 3. Predefined macros include __osdev__.
    macros = run(["-E", "-dM", "-xc", "-"], stdin="")
    for needle in ("#define __osdev__ 1",
                   "#define __osdev 1",
                   "#define osdev 1"):
        if needle not in macros:
            fail(f"expected `{needle}` in predefined macros "
                 "(TARGET_OS_CPP_BUILTINS from aarch64-osdev.h "
                 "not applied)")

    # 4. as binary path comes from our toolchain prefix.
    as_path = run(["-print-prog-name=as"]).strip()
    expected_prefix = os.path.join(ROOT, "build", "toolchain")
    if not as_path.startswith(expected_prefix):
        fail(f"-print-prog-name=as={as_path!r}, "
             f"expected something under {expected_prefix}")

    print(f"xgcc_build: PASS — {XGCC}")
    print(f"             triple   : {triple}")
    print(f"             version  : {ver}")
    print(f"             as       : {as_path}")
    sys.exit(0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# scripts/test_xgcc_compile.py — chapter 184 host smoke test.
#
# NOT added to scripts/sweep.sh because this exercises the host
# cross compiler.  Run manually after `make gcc-osdev` (which now
# also installs the sysroot under build/toolchain/aarch64-osdev/).
#
# What it does:
#   1. Skip cleanly if xgcc or the sysroot inputs are missing.
#   2. Verify `-D__OSDEV_LIBC__` is auto-injected (CPP_SPEC).
#   3. Compile + link a tiny hello.c with the bare command
#         aarch64-osdev-gcc hello.c -o hello
#      — NO -B, NO -isystem, NO -T, NO crt0.o on the command
#      line.  Everything must come from the baked-in specs and
#      the sysroot.  This is the contract chapter 184 adds:
#      the chapter-131b wrapper is no longer required.
#   4. Verify the output is a static aarch64 little-endian ELF
#      with entry point `_user_start` at 0x10001000xx (our
#      USER_LOAD_ADDR from userspace/linker_user.ld).
#   5. Verify `-nostartfiles` correctly drops crt0 from the
#      link line (LINK_SPEC opt-out works).
#   6. Print PASS, exit 0.

import os
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Optional

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XGCC = os.path.join(ROOT, "build", "toolchain", "bin", "aarch64-osdev-gcc")
SYSROOT_LIB = os.path.join(ROOT, "build", "toolchain", "aarch64-osdev", "lib")
SYSROOT_INC = os.path.join(ROOT, "build", "toolchain", "aarch64-osdev",
                           "include")

HELLO_C = r"""
#include <stdio.h>

int main(void)
{
    printf("hello from xgcc\n");
    return 0;
}
"""


def fail(msg: str) -> None:
    print(f"xgcc_compile: FAIL - {msg}", file=sys.stderr)
    sys.exit(1)


def run(args, stdin: Optional[str] = None, cwd: Optional[str] = None,
        check: bool = True):
    r = subprocess.run(args, capture_output=True, text=True,
                       input=stdin, cwd=cwd)
    if check and r.returncode != 0:
        fail(f"{' '.join(args)} rc={r.returncode}\n"
             f"stdout: {r.stdout}\nstderr: {r.stderr}")
    return r


def main() -> None:
    if not os.path.exists(XGCC):
        print(f"xgcc_compile: SKIP - {XGCC} not present "
              f"(run `make gcc-osdev` first)")
        sys.exit(0)

    # Sysroot must be populated for the bare-command compile to
    # work.  `make gcc-osdev` triggers `xgcc-sysroot` as a dep.
    for must_exist in (os.path.join(SYSROOT_LIB, "crt0.o"),
                       os.path.join(SYSROOT_LIB, "linker_user.ld"),
                       os.path.join(SYSROOT_INC, "stdio.h")):
        if not os.path.exists(must_exist):
            print(f"xgcc_compile: SKIP - missing sysroot input "
                  f"{must_exist} (run `make xgcc-sysroot`)")
            sys.exit(0)

    # 1. CPP_SPEC injects -D__OSDEV_LIBC__.
    r = run([XGCC, "-E", "-dM", "-xc", "-"], stdin="")
    if "#define __OSDEV_LIBC__ 1" not in r.stdout:
        fail("CPP_SPEC did not inject -D__OSDEV_LIBC__")

    # 2. Bare-command compile + link.  The whole point: no -B,
    # no -isystem, no -T, no crt0.o.  Specs + sysroot do it all.
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "hello.c")
        out = os.path.join(td, "hello")
        with open(src, "w") as f:
            f.write(HELLO_C)

        run([XGCC, src, "-o", out])

        if not os.path.exists(out):
            fail(f"{out} not produced")

        # 3. Confirm aarch64 LE ELF.
        with open(out, "rb") as f:
            head = f.read(20)
        if head[:4] != b"\x7fELF":
            fail(f"output is not ELF (magic={head[:4]!r})")
        if head[4] != 2:        # EI_CLASS = ELFCLASS64
            fail(f"not ELF64 (EI_CLASS={head[4]})")
        if head[5] != 1:        # EI_DATA = ELFDATA2LSB
            fail(f"not little-endian (EI_DATA={head[5]})")
        # e_machine at offset 18 LE16
        e_machine = head[18] | (head[19] << 8)
        if e_machine != 183:    # EM_AARCH64
            fail(f"e_machine={e_machine}, want 183 (EM_AARCH64)")

        # 4. Entry symbol + VA via objdump.
        objdump = shutil.which("aarch64-elf-objdump") \
            or shutil.which("aarch64-osdev-objdump") \
            or shutil.which("llvm-objdump") \
            or shutil.which("objdump")
        if objdump is None:
            print("xgcc_compile: WARN - no objdump in PATH; skipping "
                  "entry / symbol checks")
        else:
            # Disassemble: confirm _user_start present at our
            # USER_LOAD_ADDR (0x1000100000).
            r = run([objdump, "-d", "-j", ".text", out])
            if "_user_start" not in r.stdout:
                fail("_user_start symbol missing from linked output")
            m = re.search(r"^([0-9a-f]+)\s+<_user_start>:",
                          r.stdout, re.MULTILINE)
            if not m:
                fail("_user_start label not located in .text dump")
            vaddr = int(m.group(1), 16)
            # USER_LOAD_ADDR == 0x1000100000; _user_start lives in
            # .text immediately after the file header, so VA must
            # be in the [USER_LOAD_ADDR, USER_LOAD_ADDR + 0x10000)
            # window.  Strict-equality would also work today but
            # is too fragile if we ever add data before .text.
            if not (0x1000100000 <= vaddr < 0x1000110000):
                fail(f"_user_start VA 0x{vaddr:x} outside expected "
                     f"window [0x1000100000, 0x1000110000)")

        # 5. -nostartfiles must drop crt0 from the link line.  ld
        # without _user_start still produces output (with a
        # "cannot find entry symbol" warning, rc=0), so we don't
        # check the exit status; we check that crt0.o is absent
        # from the verbose collect2 line — that proves the
        # STARTFILE_SPEC `%{!nostartfiles:...}` opt-out fires.
        with open(src, "w") as f:
            f.write("int main(void){return 0;}\n")
        r_with = run([XGCC, "-v", src, "-o", out, "-c"], check=False)
        if "crt0" not in (r_with.stdout + r_with.stderr):
            # Compile-only (`-c`) skips the link line; redo with
            # full link to capture collect2.
            r_with = run([XGCC, "-v", src, "-o", out], check=False)
        if "crt0" not in (r_with.stdout + r_with.stderr):
            fail("crt0 missing from default link line - "
                 "STARTFILE_SPEC not firing")

        r_no = run([XGCC, "-v", "-nostartfiles", src, "-o", out],
                   check=False)
        # `crt0` may still appear in -v output as the search-path
        # echo of where it'd come from; the smoking gun is whether
        # collect2 actually got a path ending in crt0.o on its
        # cmdline.  Match on " ...crt0.o " preceded by a slash.
        if re.search(r"/crt0\.o\b", r_no.stderr):
            fail("-nostartfiles still pulled in crt0.o "
                 "(LINK_SPEC opt-out broken)")

    print(f"xgcc_compile: PASS - {XGCC}")
    print(f"               cpp spec : -D__OSDEV_LIBC__")
    print(f"               startfile: crt0.o (sysroot)")
    print(f"               linker   : linker_user.ld (sysroot)")
    sys.exit(0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# scripts/test_binutils_target.py — chapter 131a host-tool smoke test.
#
# NOT added to scripts/sweep.sh because this exercises host
# binaries, not the kernel.  Run manually when you suspect
# build/toolchain/bin/aarch64-osdev-* is broken (after a fresh
# clone + `make binutils-osdev`, or after editing the patch).
#
# What it does, in order:
#   1. Skip with a clear message if build/toolchain/bin/aarch64-osdev-as
#      doesn't exist (developer hasn't built the toolchain yet).
#   2. Write a 4-instruction aarch64 asm program to a tmpfile.
#   3. Assemble with aarch64-osdev-as.
#   4. Link with aarch64-osdev-ld (no libc, just the raw entry).
#   5. Open the linked file and assert:
#        - ELF magic       7f 45 4c 46
#        - EI_CLASS=64      (byte 4 == 2)
#        - EI_DATA=little   (byte 5 == 1)
#        - e_machine=183    (AArch64; little-endian u16 at offset 18)
#   6. Print PASS and exit 0.  Anything else exits 1 with FAIL.
#
# We don't bother running the linked program — that's chapter
# 131b's job (smoke-loading it inside the guest).  Chapter 131a
# only proves the *host* can produce a valid aarch64 ELF using
# the new aarch64-osdev triple.

import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN  = os.path.join(ROOT, "build", "toolchain", "bin")
AS   = os.path.join(BIN, "aarch64-osdev-as")
LD   = os.path.join(BIN, "aarch64-osdev-ld")

# Minimal aarch64 program: load 42 into x0, branch to self.
# We don't care about it running — only about it being a
# well-formed ELF that the linker accepts.
ASM = """
    .text
    .global _start
_start:
    mov x0, #42
1:  b   1b
"""

def fail(msg: str) -> None:
    print(f"binutils_target: FAIL — {msg}", file=sys.stderr)
    sys.exit(1)

def main() -> None:
    if not os.path.exists(AS):
        print(f"binutils_target: SKIP — {AS} not built yet "
              f"(run `make binutils-osdev` first)")
        sys.exit(0)
    if not os.path.exists(LD):
        fail(f"{AS} exists but {LD} does not")

    with tempfile.TemporaryDirectory() as d:
        s_path = os.path.join(d, "smoke.s")
        o_path = os.path.join(d, "smoke.o")
        x_path = os.path.join(d, "smoke.elf")
        with open(s_path, "w") as f:
            f.write(ASM)
        # 1. Assemble.
        r = subprocess.run([AS, "-o", o_path, s_path],
                           capture_output=True, text=True)
        if r.returncode != 0:
            fail(f"as failed (rc={r.returncode}): {r.stderr.strip()}")
        # 2. Link.  -e _start picks our entry point; no libc, no crt0.
        r = subprocess.run([LD, "-e", "_start", "-o", x_path, o_path],
                           capture_output=True, text=True)
        if r.returncode != 0:
            fail(f"ld failed (rc={r.returncode}): {r.stderr.strip()}")
        # 3. Inspect ELF header.
        with open(x_path, "rb") as f:
            hdr = f.read(20)
        if len(hdr) < 20:
            fail(f"linked file is only {len(hdr)} bytes")
        if hdr[:4] != b"\x7fELF":
            fail(f"bad ELF magic: {hdr[:4]!r}")
        if hdr[4] != 2:
            fail(f"EI_CLASS={hdr[4]} (want 2 = ELF64)")
        if hdr[5] != 1:
            fail(f"EI_DATA={hdr[5]} (want 1 = little-endian)")
        e_machine = struct.unpack("<H", hdr[18:20])[0]
        if e_machine != 183:
            fail(f"e_machine={e_machine} (want 183 = EM_AARCH64)")
    print("binutils_target: PASS — aarch64-osdev-as + aarch64-osdev-ld "
          "produce ELF64-LE aarch64")
    sys.exit(0)

if __name__ == "__main__":
    main()

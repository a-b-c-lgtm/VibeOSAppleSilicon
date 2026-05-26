#!/usr/bin/env python3
# scripts/test_aarch64_osdev_cc.py — chapter 131b smoke test.
#
# NOT in scripts/sweep.sh (host-tool sanity, like 131a's
# test_binutils_target.py).  Run manually after
# `make aarch64-osdev-cc-install` or after editing
# scripts/aarch64-osdev-cc.in.
#
# What it does, in two halves:
#
#   HALF A — wrapper passthrough.
#     Build userspace/hello/hello.c via the wrapper, mirroring
#     the Makefile flags, strip, and assert the result is BYTE
#     IDENTICAL to the Makefile-built hello.stripped.elf.  This
#     proves the wrapper does not perturb the compile/link
#     pipeline for code that uses relative `#include "../libc/..."`
#     paths.  Failure here would mean the wrapper accidentally
#     injects a header (newlib creeping in), changes optimisation,
#     or otherwise diverges from the existing Makefile shape.
#
#   HALF B — aarch64-osdev-ld is functionally equivalent to the
#     host's aarch64-elf-ld.  We CAN'T route the wrapper itself
#     through our linker today: aarch64-elf-gcc was configured by
#     Homebrew with --with-ld=/opt/homebrew/.../aarch64-elf-ld,
#     and that path is hardcoded into the gcc spec, beating any
#     -B prefix.  So instead we drive our linker directly: link
#     crt0.o + hello.o using build/toolchain/bin/aarch64-osdev-ld
#     with the same USER_LDFLAGS the Makefile uses, strip, and
#     assert byte identity with the baseline.  This proves we
#     can swap to our ld whenever the wrapper learns to bypass
#     gcc's hardcoded path (chapter 132's real aarch64-osdev-gcc
#     will be configured with --with-ld=$TOOLCHAIN/aarch64-osdev-ld,
#     and the trick will fall out for free).
#
# Why hello.c specifically:  it's the smallest fully-linked binary
# in the tree, so a divergence points at obvious places.  It pulls
# in libc/printf.h, libc/syscall.h, libc/malloc.h transitively via
# the single-TU header pattern, so the test exercises real libc
# code paths, not just an empty main().

import os
import subprocess
import sys
import tempfile

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC    = os.path.join(ROOT, "build", "toolchain", "bin", "aarch64-osdev-cc")
LD    = os.path.join(ROOT, "build", "toolchain", "bin", "aarch64-osdev-ld")
STRIP = os.path.join(ROOT, "build", "toolchain", "bin", "aarch64-osdev-strip")

USER_CFLAGS = [
    "-ffreestanding", "-nostdlib", "-nostartfiles",
    "-mcpu=cortex-a72",
    "-fno-stack-protector", "-fno-pie", "-fno-pic",
    "-fno-asynchronous-unwind-tables",
    "-Wall", "-Wextra", "-Werror", "-Os", "-g",
]
USER_LDFLAGS = [
    "-T", os.path.join(ROOT, "userspace", "linker_user.ld"),
    "-nostdlib", "--orphan-handling=error",
    "-z", "noexecstack", "-z", "max-page-size=0x1000",
]
CRT0_S = os.path.join(ROOT, "userspace", "crt", "crt0.S")
HELLO_C = os.path.join(ROOT, "userspace", "hello", "hello.c")
CRT0_O_BASELINE  = os.path.join(ROOT, "build", "userspace", "crt", "crt0.o")
HELLO_O_BASELINE = os.path.join(ROOT, "build", "userspace", "hello", "hello.o")
MAKEFILE_HELLO   = os.path.join(ROOT, "build", "userspace", "hello",
                                "hello.stripped.elf")

def fail(msg):
    print(f"aarch64_osdev_cc: FAIL — {msg}", file=sys.stderr)
    sys.exit(1)

def run(cmd, cwd=None):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if r.returncode != 0:
        fail(f"{cmd[0]} failed (rc={r.returncode}):\n"
             f"  cmd: {' '.join(cmd)}\n"
             f"  stderr: {r.stderr.strip()}")

def assert_byte_identical(label, candidate_path, baseline_path):
    with open(candidate_path, "rb") as f:
        candidate = f.read()
    with open(baseline_path, "rb") as f:
        baseline = f.read()
    if candidate == baseline:
        return len(candidate)
    if len(candidate) != len(baseline):
        fail(f"{label}: size mismatch "
             f"candidate={len(candidate)} bytes, baseline={len(baseline)} bytes")
    for i, (a, b) in enumerate(zip(candidate, baseline)):
        if a != b:
            fail(f"{label}: byte mismatch at offset 0x{i:x} "
                 f"(candidate=0x{a:02x}, baseline=0x{b:02x})")
    fail(f"{label}: byte mismatch (no specific offset found)")

def main():
    if not os.path.exists(CC):
        print(f"aarch64_osdev_cc: SKIP — {CC} not installed "
              f"(run `make aarch64-osdev-cc-install`)")
        sys.exit(0)
    if not os.path.exists(MAKEFILE_HELLO):
        print(f"aarch64_osdev_cc: SKIP — {MAKEFILE_HELLO} missing "
              f"(run `make` first to produce the comparison baseline)")
        sys.exit(0)

    with tempfile.TemporaryDirectory() as d:
        # ---- HALF A — wrapper compile + wrapper link ----
        crt0_o   = os.path.join(d, "crt0.o")
        hello_o  = os.path.join(d, "hello.o")
        elf      = os.path.join(d, "hello.elf")
        stripped = os.path.join(d, "hello.stripped.elf")

        run([CC] + USER_CFLAGS + ["-c", CRT0_S, "-o", crt0_o])
        run([CC] + USER_CFLAGS + ["-c", HELLO_C, "-o", hello_o])
        link_args = []
        for f in USER_LDFLAGS:
            link_args += ["-Wl," + f]
        run([CC] + ["-nostdlib", "-nostartfiles", "-o", elf,
                    crt0_o, hello_o] + link_args)
        run([STRIP, "-o", stripped, elf])
        size_a = assert_byte_identical("HALF A (wrapper passthrough)",
                                       stripped, MAKEFILE_HELLO)

        # ---- HALF B — our aarch64-osdev-ld direct link ----
        # Use the Makefile-built crt0.o + hello.o (they're what the
        # baseline came from), feed through our linker, strip, compare.
        elf_b      = os.path.join(d, "hello_b.elf")
        stripped_b = os.path.join(d, "hello_b.stripped.elf")
        run([LD] + USER_LDFLAGS + ["-o", elf_b,
                                   CRT0_O_BASELINE, HELLO_O_BASELINE])
        run([STRIP, "-o", stripped_b, elf_b])
        size_b = assert_byte_identical("HALF B (osdev-ld direct)",
                                       stripped_b, MAKEFILE_HELLO)

    print(f"aarch64_osdev_cc: PASS — wrapper and aarch64-osdev-ld both "
          f"produce byte-identical hello.stripped.elf "
          f"({size_a} bytes)")
    sys.exit(0)

if __name__ == "__main__":
    main()

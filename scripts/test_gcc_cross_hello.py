#!/usr/bin/env python3
"""scripts/test_gcc_cross_hello.py — chapter 122 smoke test.

Validates that the host-resident aarch64-elf-gcc cross-toolchain,
configured exactly the way chapter 122 specifies (USER_CFLAGS +
USER_LDFLAGS + crt0.o + linker_user.ld), produces an ELF that
runs unmodified inside the OS.

We use the host toolchain that ships with the book today
(`aarch64-elf-gcc`).  Chapter 122 documents how a target-renamed
`aarch64-none-osdev-gcc` would slot in at the same invariants.

Test approach:
  1. Cross-compile a tiny hello.c on the host.
  2. Stage the binary into a /data/ramfs file via the build's mkosfs.
  3. Boot the OS, run the binary, assert stdout + exit code.

This proves the cross-build CONTRACT, not a particular compiler
binary.  If a real /bin/gcc lands later, swapping CC=
aarch64-none-osdev-gcc and re-running this test is the bring-up
gate.
"""
import os, sys, socket, subprocess, time, select, signal, tempfile, shutil

ROOT = "/Users/seusher/Desktop/osdev"
sys.path.insert(0, os.path.join(ROOT, "scripts"))

# Reuse the proven harness pieces from test_bin_ld_ar.
from test_bin_ld_ar import reformat_data, boot, conn, wait_for, send_cmd, hard_kill, drain  # type: ignore

PROMPT = b"/$ "

HELLO_C = r"""
/* chapter 122 smoke source */
#include <stddef.h>
#include <stdint.h>

#define SYS_WRITE 1
#define SYS_EXIT  2

static long sys_write(int fd, const void *buf, unsigned long n) {
    register long  x0 __asm__("x0") = (long)fd;
    register long  x1 __asm__("x1") = (long)buf;
    register long  x2 __asm__("x2") = (long)n;
    register long  x8 __asm__("x8") = SYS_WRITE;
    register long  x0o __asm__("x0");
    __asm__ volatile("svc #0"
                     : "=r"(x0o)
                     : "r"(x0), "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
    return x0o;
}

static void sys_exit(int code) {
    register long  x0 __asm__("x0") = (long)code;
    register long  x8 __asm__("x8") = SYS_EXIT;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x8));
    __builtin_unreachable();
}

static unsigned long str_len(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *msg = "M122-CROSS-OK\n";
    sys_write(1, msg, str_len(msg));
    sys_exit(7);
    return 0;
}
"""

def expect(cond, msg):
    print(("PASS: " if cond else "FAIL: ") + msg)
    return 1 if cond else 0

def main():
    print("[chapter 122] host cross-toolchain end-to-end smoke")

    # 1. Cross-compile on the host using the SAME invariants as
    #    every other userspace binary in the book.
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "hello.c")
        obj = os.path.join(td, "hello.o")
        elf = os.path.join(td, "hello.elf")
        stripped = os.path.join(td, "hello.stripped")
        with open(src, "w") as f:
            f.write(HELLO_C)
        cflags = [
            "-ffreestanding", "-nostdlib", "-nostartfiles",
            "-mcpu=cortex-a72", "-mgeneral-regs-only",
            "-fno-stack-protector", "-fno-pie", "-fno-pic",
            "-fno-asynchronous-unwind-tables",
            "-Wall", "-Wextra", "-Werror", "-Os",
        ]
        ldflags = [
            "-T", os.path.join(ROOT, "userspace/linker_user.ld"),
            "-nostdlib", "--orphan-handling=error",
            "-z", "noexecstack", "-z", "max-page-size=0x1000",
        ]
        cc = "aarch64-elf-gcc"
        ld = "aarch64-elf-ld"
        objcopy = "aarch64-elf-objcopy"
        crt0 = os.path.join(ROOT, "build/userspace/crt/crt0.o")

        if shutil.which(cc) is None:
            print(f"FAIL: host cross-toolchain '{cc}' missing")
            return 1
        if not os.path.exists(crt0):
            # Ensure crt0 is built (it always is after `make` but
            # be explicit for fresh checkouts).
            subprocess.check_call(["make", "-C", ROOT, crt0],
                                   stdout=subprocess.DEVNULL)

        subprocess.check_call([cc, *cflags, "-c", src, "-o", obj])
        subprocess.check_call([ld, *ldflags, "-o", elf, crt0, obj])
        subprocess.check_call([objcopy, "--strip-all", elf, stripped])
        size = os.path.getsize(stripped)
        print(f"  host-cross compile OK: {size} bytes ELF")
        host_pass = 0
        host_pass += expect(size > 0 and size < 32768,
               "cross-built ELF has reasonable size")
        with open(stripped, "rb") as f:
            ehdr = f.read(20)
        host_pass += expect(ehdr.startswith(b"\x7fELF"),
               "cross-built file starts with ELF magic")
        host_pass += expect(ehdr[18:20] == b"\xb7\x00",
               "cross-built file is EM_AARCH64")

        # 2. Stage the binary into the OSFS-2 data partition as
        #    a seed file (mkosfs2 accepts name=path arguments).
        subprocess.check_call(["python3",
                               os.path.join(ROOT, "scripts/mkosfs2.py"),
                               os.path.join(ROOT, "build/data.img"),
                               f"cross_hello={stripped}"],
                              stdout=subprocess.DEVNULL)

    # 3. Boot the OS and run the cross-built binary.
    q = boot()
    s = conn()
    pass_count = host_pass
    try:
        wait_for(s, PROMPT, timeout=20.0)
        # Let the boot log + WM settle so kernel noise doesn't
        # eat our subsequent prompt detection.
        time.sleep(2.0)
        drain(s, time.time() + 0.5)

        # The /data/ mount should have our staged binary.
        out = send_cmd(s, "ls /data", timeout=10.0)
        pass_count += expect(b"cross_hello" in out,
                             "/data/cross_hello visible on disk")

        # Copy it into a runnable spot (tmpfs is exec-friendly).
        out = send_cmd(s, "cat /data/cross_hello > /tmp/cross_hello",
                       timeout=15.0)
        out = send_cmd(s, "/tmp/cross_hello", timeout=10.0)
        pass_count += expect(b"M122-CROSS-OK" in out,
                             "cross-built binary printed its marker")
        pass_count += expect(b"exited with code 0x0000000000000007" in out
                              or b"exit code 0x7" in out
                              or b"exited with code 0x7" in out,
                             "cross-built binary exited with code 7")
    finally:
        s.close()
        hard_kill(q)
        # Reset data.img back to empty so subsequent tests start clean.
        try:
            subprocess.check_call(["python3",
                                   os.path.join(ROOT, "scripts/mkosfs2.py"),
                                   os.path.join(ROOT, "build/data.img")],
                                  stdout=subprocess.DEVNULL)
        except Exception:
            pass

    print()
    # We declared 6 expect() calls above (3 host, 3 guest).
    total = 6
    fail_count = total - pass_count
    print(f"{pass_count} PASS / {fail_count} FAIL")
    return 0 if fail_count == 0 else 1

if __name__ == "__main__":
    sys.exit(main())

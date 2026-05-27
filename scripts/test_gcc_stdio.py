#!/usr/bin/env python3
"""scripts/test_gcc_stdio.py -- chapter 189 in-guest #include <stdio.h>.

Chapter 188 compiled a freestanding C program with extern decls
for every libosdevc.a symbol it used (no #include).  That worked
but is not how anybody actually writes C.

Chapter 189 ships the 24 user-facing libc headers onto /bin and
adds `-isystem /bin` to the /bin/gcc shim so the in-guest gcc
can resolve `#include <stdio.h>` and friends.

Test ladder:

  1. /bin/gcc -E /bin/stdio_test.c  -- cpp finds <stdio.h> via -isystem
  2. /bin/gcc /bin/stdio_test.c -o /tmp/stdio_test  -- full compile+link
  3. /tmp/stdio_test                                 -- run, expect output

The body of the test program uses:
  - printf with both %s and %d format specifiers
  - puts (returns newline-terminated string)
  - Exit code via main return

Any of those failing means a libc header didn't compose cleanly
with the in-guest cc1 (likely candidates: missing types, missing
helper symbols, freestanding-mode trap).

Source is shipped at /bin/stdio_test.c via mkosfs.py rather than
staged via echo at runtime, because the source contains `<`, `>`,
`%`, `,`, and `"` characters that our shell's quoting doesn't
handle cleanly.  This matches the chapter-132h bf.c pattern --
ship the source on disk, compile it in-guest.
"""

import os
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-gcc-stdio.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

STDIO_C = r"""#include <stdio.h>
int main(void) {
    printf("hello from %s, answer=%d\n", "in-guest gcc", 42);
    puts("puts works too");
    return 7;
}
"""

# Path to the source on the OSFS image.  Shipped by the Makefile
# under assets/osfs/stdio_test.c.
GUEST_SRC = "/bin/stdio_test.c"


def cleanup_sock():
    try:
        os.unlink(SERIAL_SOCK)
    except FileNotFoundError:
        pass


def reformat_data():
    subprocess.check_call(
        ["python3", f"{ROOT}/scripts/mkosfs2.py", DATA_IMG],
        stdout=subprocess.DEVNULL,
    )


def boot():
    cleanup_sock()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={DATA_IMG},format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def hard_kill(q):
    try:
        q.send_signal(signal.SIGKILL)
        q.wait(timeout=3)
    except Exception:
        pass
    cleanup_sock()


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SERIAL_SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(65536)
            if not c:
                break
            out += c
        elif out:
            break
    return out


def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def send_cmd(s, cmd, timeout=60.0):
    if isinstance(cmd, str):
        cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


PASSES, FAILS = [], []


def expect(cond, msg):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        FAILS.append(msg)


def main():
    print("[chapter 189] in-guest #include <stdio.h>")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        # --- sanity: source is on the disk -----------------
        out = send_cmd(s, f"cat {GUEST_SRC}", timeout=10.0)
        expect(b"#include <stdio.h>" in out and b"printf" in out,
               "sanity: /bin/stdio_test.c shipped on OSFS")

        # --- step 1: cpp finds stdio.h via -isystem /bin ----
        out = send_cmd(s,
                       f"/bin/gcc -E {GUEST_SRC} "
                       "> /tmp/stdio_test.i",
                       timeout=120.0)
        sys.stdout.write("--- gcc -E stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------\n")
        out2 = send_cmd(s, "cat /tmp/stdio_test.i", timeout=20.0)
        # The preprocessed file should contain something from
        # stdio.h's transitive expansion (a function prototype).
        expect(b"printf" in out2,
               "step 1: cpp expands <stdio.h> (printf visible)")
        expect(b"fatal error" not in out and b"No such file" not in out,
               "step 1: no missing-header errors")

        # --- step 2: full compile + link --------------------
        out = send_cmd(s,
                       f"/bin/gcc {GUEST_SRC} -o /tmp/stdio_test",
                       timeout=240.0)
        sys.stdout.write("--- gcc full build stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n-----------------------------\n")
        out2 = send_cmd(s, "cat /tmp/stdio_test", timeout=20.0)
        built = b"\x7FELF" in out2
        expect(built,
               "step 2: in-guest gcc produced /tmp/stdio_test ELF")

        if not built:
            print("\nBuild failed; skipping run.")
            return _report()

        # --- step 3: run + check output + exit code ---------
        out = send_cmd(s, "/tmp/stdio_test", timeout=20.0)
        out += send_cmd(s, "echo exit=$?", timeout=10.0)
        sys.stdout.write("--- /tmp/stdio_test run ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------------\n")
        expect(b"hello from in-guest gcc, answer=42" in out,
               "step 3: printf format string + %s + %d resolves correctly")
        expect(b"puts works too" in out,
               "step 3: puts() works")
        expect(b"exit=7" in out or b"0x07" in out
               or b"0x0000000000000007" in out,
               "step 3: main returned 7 via stdio.h-compiled program")

    finally:
        hard_kill(q)

    return _report()


def _report():
    print()
    print(f"PASS: {len(PASSES)}")
    print(f"FAIL: {len(FAILS)}")
    if FAILS:
        for f in FAILS:
            print(f"  - {f}")
        sys.exit(1)


if __name__ == "__main__":
    main()

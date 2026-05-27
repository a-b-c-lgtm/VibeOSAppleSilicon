#!/usr/bin/env python3
"""scripts/test_gcc_sys_stat.py -- chapter 190 sys/ headers on /bin.

Chapter 189 shipped the 24 user-facing libc headers + 16 GCC
freestanding headers on /bin and proved `#include <stdio.h>`
works in-guest.  But `unistd.h` and `sys/*.h` were left out
because the OSFS-1 directory had no subdirectory support.

Chapter 190 takes the simplest viable path: keep OSFS-1's flat
namespace, but allow `/` inside the 20-byte name field.  An
entry literally named `sys/stat.h` is byte-exact-matched by the
kernel's path resolver when cpp opens `/bin/sys/stat.h`.

The complication: our `userspace/libc/sys/*.h` headers use
`#include "../foo.h"` to reach siblings.  In-guest that would
resolve to `/bin/sys/../foo.h`, which our kernel does not
normalise.  So we stage the headers through
`scripts/stage_libc_headers.py` first, rewriting `"../foo.h"`
to `<foo.h>` -- angle-bracket lookups go through `-isystem /bin`
which finds `/bin/foo.h` directly.

Test ladder (3 steps, 5 expectations):

  1. cpp finds <sys/stat.h> via -isystem /bin
  2. /bin/gcc compiles + links a program using stat() + access()
  3. Run the program against a known file (/bin/stdio_test.c)
     and check the reported size matches `wc -c`.

If step 1 fails: a sys/ header wasn't shipped, or wasn't named
correctly on disk.  If step 2 fails: probably a relative-include
in a staged header still pointing at `../` (look at the staging
script regex).  If step 3 fails: the libc stat()/access() wiring
broke.
"""

import os
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-gcc-sysstat.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

GUEST_SRC = "/bin/sys_stat_test.c"


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
    print("[chapter 190] in-guest #include <sys/stat.h>")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        # --- sanity: source + headers shipped ---------------
        out = send_cmd(s, f"cat {GUEST_SRC}", timeout=10.0)
        expect(b"#include <sys/stat.h>" in out and b"stat(" in out,
               "sanity: /bin/sys_stat_test.c shipped on OSFS")

        out = send_cmd(s, "cat /bin/sys/stat.h", timeout=10.0)
        expect(b"struct stat" in out and b"S_ISREG" in out,
               "sanity: /bin/sys/stat.h shipped on OSFS")
        # The staging rewrite should have converted `"../syscall.h"`
        # to `<syscall.h>`; confirm no parent-relative includes
        # leaked into the on-disk copy.
        expect(b"\"../" not in out,
               "sanity: staging rewrote ../foo.h -> <foo.h>")

        # --- step 1: cpp finds <sys/stat.h> -----------------
        out = send_cmd(s,
                       f"/bin/gcc -E {GUEST_SRC} "
                       "> /tmp/sys_stat_test.i",
                       timeout=120.0)
        sys.stdout.write("--- gcc -E stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------\n")
        expect(b"fatal error" not in out and b"No such file" not in out,
               "step 1: cpp finds <sys/stat.h>, <unistd.h>, "
               "<sys/types.h>")

        # --- step 2: full compile + link --------------------
        out = send_cmd(s,
                       f"/bin/gcc {GUEST_SRC} -o /tmp/sys_stat_test",
                       timeout=240.0)
        sys.stdout.write("--- gcc full build stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n-----------------------------\n")
        out2 = send_cmd(s, "cat /tmp/sys_stat_test", timeout=20.0)
        built = b"\x7FELF" in out2
        expect(built,
               "step 2: /bin/gcc compiles sys_stat_test.c to ELF")

        if not built:
            print("\nBuild failed; skipping run.")
            return _report()

        # --- step 3: run + check output ---------------------
        out = send_cmd(s, "/tmp/sys_stat_test", timeout=20.0)
        out += send_cmd(s, "echo exit=$?", timeout=10.0)
        sys.stdout.write("--- /tmp/sys_stat_test run ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n------------------------------\n")
        expect(b"sys_stat_test OK" in out,
               "step 3: stat() + access() + S_ISREG all succeeded")

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

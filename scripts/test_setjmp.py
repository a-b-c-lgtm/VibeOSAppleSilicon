#!/usr/bin/env python3
"""scripts/test_setjmp.py — chapter 128a setjmp/longjmp regression.

Boots the kernel headless over a unix-socket serial pipe (same
shape as scripts/test_printftest.py), waits for the shell
prompt, runs `setjmptest`, and asserts the test program printed
"all checks passed".  Any "FAIL:" line, any guest panic, or the
absence of the success marker fails the test.

What this covers (mirrors the asserts in setjmptest.c):

  - setjmp(env) returns 0 on the first call.
  - longjmp(env, N) for N != 0 makes setjmp return N.
  - longjmp(env, 0) makes setjmp return 1 (C99 7.13.2.1#3).
  - A local variable kept in a callee-saved register (x19..x28)
    survives the round trip through inner() and the longjmp,
    which is the whole reason the aarch64 asm in
    userspace/libc/setjmp.S saves x19..x28+x29+x30+sp.

Roughly 4 assertions, ~10 s wall.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-setjmp.sock"


def boot():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def read_until(ser, needles, timeout, prior=b""):
    if isinstance(needles, (bytes, str)):
        needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray(prior)
    if any(n in buf for n in needles):
        return bytes(buf)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            c = ser.recv(8192)
            if not c:
                break
            buf.extend(c)
            if any(n in buf for n in needles):
                return bytes(buf)
    return bytes(buf)


def wait_for_new(ser, needles, timeout, log):
    """Wait for any needle to appear in bytes received AFTER the
    current end-of-`log`.  Same helper as test_printftest.py;
    prevents the stale shell '$ ' from short-circuiting the read.
    """
    if isinstance(needles, (bytes, str)):
        needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    cutoff = len(log)
    buf = bytearray(log)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(buf.find(n, cutoff) >= 0 for n in needles):
            return bytes(buf)
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            c = ser.recv(8192)
            if not c:
                break
            buf.extend(c)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: shell prompt never appeared")
            return 1

        ser.sendall(b"setjmptest\n")
        log = wait_for_new(
            ser,
            [b"all checks passed", b"FAIL:", b"PANIC", b"$ "],
            30.0,
            log,
        )

        idx = log.rfind(b"setjmptest\r\n")
        if idx < 0:
            idx = log.rfind(b"setjmptest\n")
        section = (
            log[idx:].decode("ascii", "replace")
            if idx >= 0
            else log[-2000:].decode("ascii", "replace")
        )
        print("--- setjmptest output: ---")
        print(section)

        if b"FAIL:" in log:
            print("FAIL: test program reported a FAIL line")
            return 1
        if b"PANIC" in log:
            print("FAIL: guest panicked")
            return 1
        if b"all checks passed" not in log:
            print("FAIL: success marker missing")
            return 1
        return 0
    finally:
        q.kill()
        q.wait()
        try:
            os.unlink(SOCK)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())

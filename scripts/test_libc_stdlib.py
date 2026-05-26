#!/usr/bin/env python3
"""scripts/test_libc_stdlib.py — chapter 128e regression for
<stdlib.h>: qsort / bsearch / strtol family / atol / atoll / abs /
div / getopt.

Single in-guest binary: `stdlibtest`.  Boots the kernel, waits
for the shell prompt, runs the binary, and looks for the marker
"all checks passed".  No individual "FAIL " line may appear
either.  Same shape as scripts/test_libc_string.py and
scripts/test_libc_time.py.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-libcstdlib.sock"


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

        ser.sendall(b"stdlibtest\n")
        log = wait_for_new(
            ser,
            [b"all checks passed", b"FAIL", b"PANIC"],
            30.0,
            log,
        )
        if b"PANIC" in log:
            print("FAIL: guest panicked during stdlibtest")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        if b"all checks passed" not in log:
            print("FAIL: stdlibtest didn't reach the success marker")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        # Same dance as test_libc_string: ensure no individual CHECK
        # printed a FAIL line *before* the success marker.
        if b"FAIL " in log or b"FAIL:" in log[:log.rfind(b"all checks passed")]:
            print("FAIL: stdlibtest reported individual CHECK failures")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("stdlibtest: PASS")
        return 0

    finally:
        try: q.terminate()
        except Exception: pass
        try: q.wait(timeout=5)
        except Exception:
            try: q.kill()
            except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

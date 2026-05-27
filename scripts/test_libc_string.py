#!/usr/bin/env python3
"""scripts/test_libc_string.py — chapter 167 regression for
ctype.h / string.h / assert.h.

Two binaries:
  - strtest    -- a few dozen CHECK()s on ctype + string.
                  Expects marker "all checks passed".
  - assertfail -- calls assert(0).  Harness expects:
                  * the standard glibc/musl-shaped diagnostic
                    "assertfail.c:NN: main: Assertion `..' failed."
                  * shell $? == 134 (== 128 + SIGABRT).
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-libcstr.sock"


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

        # Test 1: strtest
        ser.sendall(b"strtest\n")
        log = wait_for_new(
            ser,
            [b"all checks passed", b"FAIL", b"PANIC"],
            30.0,
            log,
        )
        if b"PANIC" in log:
            print("FAIL: guest panicked during strtest")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        if b"all checks passed" not in log:
            print("FAIL: strtest didn't reach the success marker")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        # Make sure no CHECK() FAILed -- strtest only prints
        # "all checks passed" on g_fail==0 but we also want to
        # surface any FAIL lines for diagnosis.
        if b"FAIL " in log or b"FAIL:" in log[:log.rfind(b"all checks passed")]:
            print("FAIL: strtest reported individual CHECK failures")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("strtest: PASS")

        # Wait for prompt before next command.
        log = wait_for_new(ser, [b"$ "], 5.0, log)

        # Test 2: assertfail
        ser.sendall(b"assertfail\n")
        log = wait_for_new(ser, [b"about to assert"], 10.0, log)
        # The diagnostic line should arrive next.  It's written
        # to fd 2 which on our cooked-mode TTY is the same UART
        # as fd 1, so it interleaves with stdout naturally.
        log = wait_for_new(
            ser,
            [b"Assertion", b"PANIC"],
            10.0,
            log,
        )
        if b"PANIC" in log:
            print("FAIL: guest panicked during assertfail")
            return 1
        if b"Assertion" not in log:
            print("FAIL: never saw assert diagnostic")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        # Wait for the prompt that comes back after the process dies.
        log = wait_for_new(ser, [b"$ "], 10.0, log)
        # Probe $? using the same marker dance as test_signal_raise.py:
        # the shell echoes typed bytes, so the LAST occurrence of the
        # marker is the expanded one.
        ser.sendall(b"echo assertfail_status=$?\n")
        log = wait_for_new(ser, [b"$ "], 10.0, log)
        positions = []
        i = 0
        while True:
            j = log.find(b"assertfail_status=", i)
            if j < 0:
                break
            positions.append(j)
            i = j + 1
        if len(positions) < 2:
            print("FAIL: only saw echoed command, no shell expansion")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        tail = log[positions[-1] + len(b"assertfail_status="):].decode(
            "ascii", "replace")
        digits = ""
        for c in tail:
            if c.isdigit():
                digits += c
            elif digits:
                break
        if not digits:
            print("FAIL: couldn't parse $? from shell output")
            print(tail[:200])
            return 1
        status = int(digits)
        if status != 134:
            print(f"FAIL: assert(0) exit status {status} (want 134)")
            return 1
        print(f"assertfail: PASS (exit {status} == 128 + SIGABRT, diagnostic present)")

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

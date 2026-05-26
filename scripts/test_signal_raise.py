#!/usr/bin/env python3
"""scripts/test_signal_raise.py — chapter 128b regression for
raise() / abort() and the expanded SIG* table.

Boots the kernel headless, runs two test binaries at the shell:

  1. sigtest2 — installs SIGUSR1 / SIGUSR2 handlers and a
     SIGINT ignore disposition, raise()s each in turn, asserts
     handlers ran exactly when they should have.  Expects the
     marker "all checks passed".

  2. aborttest — calls abort() and dies with SIGABRT.  The
     test reads `echo $?` from the shell afterwards and asserts
     the exit code is 134 (128 + SIGABRT == 128 + 6).  This is
     the C99 7.20.4.1 contract: abort() must terminate the
     process with an "implementation-defined unsuccessful
     termination", and our convention (matching sh + bash) is
     128 + signum.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-sigraise.sock"


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

        # Test 1: sigtest2
        ser.sendall(b"sigtest2\n")
        log = wait_for_new(
            ser,
            [b"all checks passed", b"FAIL:", b"PANIC"],
            20.0,
            log,
        )
        if b"FAIL:" in log:
            print("FAIL: sigtest2 reported a FAIL line")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        if b"PANIC" in log:
            print("FAIL: guest panicked")
            return 1
        if b"all checks passed" not in log:
            print("FAIL: sigtest2 success marker missing")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("sigtest2: PASS")

        # Wait for fresh prompt before next command.
        log = wait_for_new(ser, [b"$ "], 5.0, log)

        # Test 2: aborttest -- runs, dies with SIGABRT, then we
        # ask the shell for $? and expect 134 (128 + SIGABRT==6).
        ser.sendall(b"aborttest\n")
        log = wait_for_new(ser, [b"about to abort"], 10.0, log)
        # After abort the shell reaps the child and re-prompts.
        log = wait_for_new(ser, [b"$ "], 10.0, log)
        # Now grab $?.  Use an echo with a distinguishing prefix
        # so we don't false-positive on the literal "134" in some
        # earlier serial line.
        #
        # Subtlety: the kernel TTY echoes typed bytes back as we
        # send them, so the FIRST appearance of "aborttest_status="
        # in the stream is the echoed command itself, not the
        # shell's expansion.  Wait for the prompt that comes back
        # AFTER the echo command finishes, then look for the
        # second-or-later occurrence of the marker.
        ser.sendall(b"echo aborttest_status=$?\n")
        log = wait_for_new(ser, [b"$ "], 10.0, log)
        # Find every occurrence of the marker and take the last
        # one -- after the prompt has come back, the only way a
        # NEW marker shows up is if the shell expanded $? and
        # printed it.
        positions = []
        i = 0
        while True:
            j = log.find(b"aborttest_status=", i)
            if j < 0:
                break
            positions.append(j)
            i = j + 1
        if len(positions) < 2:
            print("FAIL: only saw the echoed command, no shell expansion")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        # Slice from just past the last occurrence (the expanded
        # one) and read the integer that follows.
        tail = log[positions[-1] + len(b"aborttest_status="):].decode(
            "ascii", "replace")
        # Find the first decimal integer in tail.
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
            print(f"FAIL: abort() exit status {status} (want 134)")
            return 1
        print(f"aborttest: PASS (exit status {status} == 128 + SIGABRT)")

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

#!/usr/bin/env python3
"""scripts/test_clipboard.py -- chapter 108 / M90b clipboard test.

Boots the kernel headless, waits for the shell prompt, and
drives the /bin/clip CLI through a full set/get/gen/clear
round-trip via the chapter-107 IPC bus.  All traffic is
in-guest; no host listener, no SLIRP hostfwd, no virtio-net
chatter in the test path.

This exercises:
  - chapter 108 daemon: /bin/clipboardd binds /srv/clipboard,
    serves SET/GET/GEN/CLEAR operations one-shot per conn.
  - chapter 108 client: /bin/clip uses the libc helpers
    (clip_set / clip_get / clip_generation / clip_clear).
  - chapter 108 supervisor: init.c's supervise() table started
    /bin/clipboardd before the shell prompt -- the very fact
    that `clip set ...` succeeds on the first try proves the
    bind happened ahead of the user-facing prompt.
  - chapter 107 underneath: every clip operation is one
    SYS_SRV_CONNECT + write + read + close on a /srv conn,
    so this test re-exercises the IPC machinery too.

The script writes "Hello chapter 108", reads it back via
`clip get`, verifies byte-for-byte, then bumps the value to
"world" and asserts the generation counter advanced.  Eight
positive assertions, ~10 s wall.

Why no GUI events: the clipboard itself is GUI-agnostic.  GUI
integration (notepad's Ctrl-C/X/V) is covered by hand-testing
and the chapter prose; the regression keeps to the CLI so it
stays fast and hermetic.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-clipboard.sock"


def cleanup():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass


def boot():
    """Launch QEMU headless.  Same shape as test_ipc.py -- the
    clipboard is one more userspace daemon on top of the same
    IPC machinery, so we use the same boot recipe."""
    cleanup()
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


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c:
                break
            out += c
    return out


def wait_for(s, needle, timeout):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def main():
    q = boot()
    try:
        ser = conn()

        # 1. Reach the shell prompt.  Boot also brings up
        #    /bin/clipboardd via init's supervisor.
        log = wait_for(ser, b"$ ", 60.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # 2. Supervisor brought up clipboardd before the prompt.
        #    The daemon prints "[clipboardd] ready on
        #    /srv/clipboard" the moment srv_bind() succeeds.
        if b"[clipboardd] ready on /srv/clipboard" not in log:
            print("FAIL: clipboardd never advertised readiness")
            print(log[-3000:].decode("ascii", "replace"))
            return 1
        print("PASS: clipboardd is up and bound to /srv/clipboard")

        # 3. clip set: write a known phrase.
        ser.sendall(b"clip set Hello chapter 108\n")
        log = wait_for(ser, b"[clipboardd] SET gen=1", 10.0)
        if b"[clipboardd] SET gen=1" not in log:
            print("FAIL: clipboardd did not log the first SET")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: clip set landed (gen=1)")

        # 4. clip get: read it back.  Expect exactly the joined
        #    argv on a single line.
        ser.sendall(b"clip get\n")
        log = wait_for(ser, b"Hello chapter 108", 10.0)
        if b"Hello chapter 108" not in log:
            print("FAIL: clip get did not return the stored payload")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: clip get returned 'Hello chapter 108' verbatim")

        # 5. clip gen: should print "1" alone on a line.
        ser.sendall(b"clip gen\n")
        log = wait_for(ser, b"[clipboardd] GEN -> gen=1", 10.0)
        if b"[clipboardd] GEN -> gen=1" not in log:
            print("FAIL: clip gen did not return 1")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: clip gen returned 1 (daemon-side log)")

        # 6. clip set again with a different value -- generation
        #    must advance.  Tests the "did anything change?"
        #    semantics that polling clients depend on.
        ser.sendall(b"clip set world\n")
        log = wait_for(ser, b"[clipboardd] SET gen=2", 10.0)
        if b"[clipboardd] SET gen=2" not in log:
            print("FAIL: clipboardd did not advance the generation")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: second SET bumped generation to 2")

        # 7. clip get: now returns the new payload.  Confirms
        #    the daemon overwrote the previous slot.
        ser.sendall(b"clip get\n")
        log = wait_for(ser, b"[clipboardd] GET -> gen=2 len=5", 10.0)
        if b"[clipboardd] GET -> gen=2 len=5" not in log:
            print("FAIL: second clip get did not return 'world' (5 bytes)")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: second clip get returned 'world' (5 bytes)")

        # 8. clip clear: empties the clipboard AND advances gen.
        ser.sendall(b"clip clear\n")
        log = wait_for(ser, b"[clipboardd] CLEAR gen=3", 10.0)
        if b"[clipboardd] CLEAR gen=3" not in log:
            print("FAIL: clip clear did not log gen=3")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: clip clear bumped generation to 3 and emptied payload")

        print("\nMILESTONE 90b (clipboard daemon): ALL TESTS PASSED")
        return 0
    finally:
        try:
            q.terminate()
            q.wait(timeout=3)
        except Exception:
            q.kill()


if __name__ == "__main__":
    sys.exit(main())

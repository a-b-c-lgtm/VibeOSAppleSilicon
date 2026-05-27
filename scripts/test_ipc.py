#!/usr/bin/env python3
"""scripts/test_ipc.py -- chapter 112 named-IPC smoke test.

Boots the kernel headless, drops to the shell prompt, and runs
`srvtest`.  Asserts that the in-guest parent (binds
`/srv/echotest` and accepts one connection) and child (dials
`/srv/echotest`, sends a known phrase, reads the echo) hand
back exactly the bytes that were sent -- all without any TCP
or network code in the path.

This test exercises:
  - SYS_SRV_BIND     -> path validation + registry insert
  - SYS_SRV_ACCEPT   -> block / wake handshake with srv_connect
  - SYS_SRV_CONNECT  -> pending-queue plumbing
  - FD_SRV_CONN read/write -> length-prefixed datagram framing
  - vfs_close routing for FD_SRV_LISTEN + FD_SRV_CONN
  - fork's per-kind FD inheritance skip for FD_SRV_LISTEN

Why hermetic: no host listener, no SLIRP hostfwd, no virtio-net
traffic.  The whole IPC handshake happens in kernel memory.
That is exactly the chapter-107 design point: a userspace
service primitive that doesn't depend on the network stack.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-ipc.sock"


def cleanup():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass


def boot():
    """Launch QEMU headless.  No hostfwd needed -- the test is
    entirely in-guest -- but we still bring up virtio-net so the
    kernel's boot-time net self-test phases complete on schedule.
    """
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

        # 1. Reach the shell prompt.  The kernel net self-test
        #    phases all time out cleanly with no host listener,
        #    so reaching the prompt confirms boot finished.
        log = wait_for(ser, b"$ ", 60.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # 2. Run srvtest.
        ser.sendall(b"srvtest\n")

        # 3. Parent bound the path.  Proves SRV_BIND took.
        log = wait_for(ser, b"[srvtest] bound /srv/echotest", 10.0)
        if b"[srvtest] bound /srv/echotest" not in log:
            print("FAIL: srvtest never bound /srv/echotest")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: srvtest bound /srv/echotest")

        # 4. Child connected.  Proves SRV_CONNECT found the
        #    registered name and the accept/connect handshake
        #    completed.
        log += wait_for(ser, b"[srvcli] connected", 10.0)
        if b"[srvcli] connected" not in log:
            print("FAIL: client never connected to /srv/echotest")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: client connected via /srv/echotest")

        # 5. Service accepted (proves SRV_ACCEPT unblocked
        #    correctly after the client's connect arrived).
        log += wait_for(ser, b"[srvsvc] accepted", 10.0)
        if b"[srvsvc] accepted" not in log:
            print("FAIL: service did not accept the connection")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: service accepted client connection")

        # 6. Service received the message.  Proves write+read
        #    framing on the c2s direction works.
        log += wait_for(ser, b"[srvsvc] got 10 bytes: ipc-hello", 10.0)
        if b"[srvsvc] got 10 bytes: ipc-hello" not in log:
            print("FAIL: service did not receive the expected message")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: service got 'ipc-hello' (10 bytes, intact)")

        # 7. Echo round-trip.  Proves the s2c direction works
        #    too -- the same datagram comes back to the client.
        log += wait_for(ser, b"[srvcli] GOT: ipc-hello", 10.0)
        if b"[srvcli] GOT: ipc-hello" not in log:
            print("FAIL: client did not receive the expected echo")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: echo round-trip data intact")

        # 8. Done line.  Proves both ends closed cleanly and
        #    the parent reaped its child without leaks.
        log += wait_for(ser, b"[srvtest] done", 10.0)
        if b"[srvtest] done" not in log:
            print("FAIL: srvtest did not print done line")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: srvtest completed cleanly")

        print("\nnamed-IPC: ALL TESTS PASSED")
        return 0
    finally:
        try:
            q.terminate()
            q.wait(timeout=3)
        except Exception:
            q.kill()


if __name__ == "__main__":
    sys.exit(main())

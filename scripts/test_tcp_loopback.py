#!/usr/bin/env python3
"""scripts/test_tcp_loopback.py -- chapter-106 / M95 smoke test.

Boots the kernel headless with NO host hostfwd and NO local HTTP
server, drops to the shell prompt, and runs `looptest 9999`.
Asserts that the in-guest parent (TCP server) and child (TCP
client) successfully handshake, exchange a known phrase, and
cleanly close -- all over 127.0.0.1 with traffic never leaving
the guest kernel.

Why this matters: before chapter 106, a guest process dialing
127.0.0.1 would hand its SYN to virtio-net TX, SLIRP would
silently drop the 127/8 frame, and the connect() would time out.
Chapter 106 added a kernel loopback short-circuit (net.c's
`net_is_local_ip` + bounded loopback queue drained by net_poll)
so loopback frames re-enter the RX path immediately, never
touching the device.  This test is the user-visible proof.

The test is fully hermetic: it does NOT require any
hostfwd rule, host listener, or external connectivity.  That
hermetic property is exactly what chapter 106 was meant to
deliver.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-loopback.sock"


def cleanup():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass


def boot():
    """Launch QEMU headless with a vanilla user-mode netdev.

    No hostfwd is needed -- the whole test happens inside the
    guest -- but we still bring up virtio-net so the kernel's
    boot-time net self-test phases (ARP / ICMP / DNS / TCP
    connect to 10.0.2.2) can complete on schedule.  Those
    phases just time out cleanly when no host listener answers.
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

        # First wait for the shell prompt.  The kernel net
        # self-test phases (5-7) all time out cleanly with no
        # host listener, so reaching the prompt confirms boot
        # made it through the network bring-up.  Generous budget
        # because the passive-open phase (port 8088 accept
        # timeout) is the long pole.
        log = wait_for(ser, b"$ ", 60.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached after net self-test")

        # Run looptest.  Picks an obscure port to avoid colliding
        # with anything the kernel self-tests are still doing.
        cmd = b"looptest 9999\n"
        ser.sendall(cmd)

        # Listener line first -- proves the parent socket_listen
        # succeeded.
        log = wait_for(ser, b"[looptest] listening on port 9999", 15.0)
        if b"[looptest] listening on port 9999" not in log:
            print("FAIL: looptest listener never started")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: looptest listener bound on port 9999")

        # Client-side connect.  This is the moment of truth:
        # without ch106 the SYN would go to virtio-net TX and
        # die in SLIRP, and we'd never see the "connected" line.
        log += wait_for(ser, b"[loopcli] connected", 20.0)
        if b"[loopcli] connected" not in log:
            print("FAIL: client never reached ESTABLISHED on 127.0.0.1")
            print("       (chapter 106 short-circuit missing or broken?)")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: client connected to 127.0.0.1:9999")

        # Server-side accept (proves the 4-tuple match found the
        # listener despite both sides using src=dst=127.0.0.1).
        log += wait_for(ser, b"[loopsrv] accepted from 127.0.0.1", 10.0)
        if b"[loopsrv] accepted from 127.0.0.1" not in log:
            print("FAIL: server did not accept loopback connection")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: server accepted (peer reported as 127.0.0.1)")

        # Echo round-trip.
        log += wait_for(ser, b"[loopcli] GOT: loopback-hello", 15.0)
        if b"[loopcli] GOT: loopback-hello" not in log:
            print("FAIL: client did not receive the expected echo")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: echo round-trip data intact ('loopback-hello')")

        # Done line.
        log += wait_for(ser, b"[looptest] done", 10.0)
        if b"[looptest] done" not in log:
            print("FAIL: looptest did not print done line")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: looptest completed cleanly")

        print("\nMILESTONE 95 (TCP loopback): ALL TESTS PASSED")
        return 0
    finally:
        try:
            q.terminate()
            q.wait(timeout=3)
        except Exception:
            q.kill()


if __name__ == "__main__":
    sys.exit(main())

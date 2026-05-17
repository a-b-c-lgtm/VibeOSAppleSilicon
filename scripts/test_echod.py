#!/usr/bin/env python3
"""scripts/test_echod.py -- chapter 104 / M93 echod end-to-end test.

Boots the kernel with SLIRP's hostfwd forwarding host:17777 ->
guest:7777, waits for the shell prompt, runs `echod 7777 --once`
in the foreground, dials the listener from the host, sends a
short payload, reads the echoed bytes back, and asserts they
match.  Then waits for echod's "done" log line to confirm the
accept-loop tore down cleanly.

This test exercises:
  - the new SYS_SOCKET_LISTEN syscall (echod's `socket_listen`),
  - the new SYS_SOCKET_ACCEPT syscall (echod's `socket_accept`),
  - the FD_SOCKET_LISTEN fd kind (the listener fd has to survive
    until echod's close() at the end),
  - peer-address out-pointers (echod logs "from a.b.c.d:port"),
  - per-conn echo via the regular read/write socket fd path
    (no new code, but proves accept hands back a fully working
    FD_SOCKET fd).

Why --once: we don't have non-blocking accept or signals yet,
so a long-running echod would block the shell forever (no way
for the test to send Ctrl-C).  --once exits after the first
peer disconnects, freeing the shell so the QEMU teardown is
graceful and reusable across the sweep harness.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-echod.sock"
HOST_PORT  = 17777
GUEST_PORT = 7777

PAYLOAD = b"the quick brown fox jumps over the lazy dog\n"


def cleanup():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass


def boot():
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
        # Punch host:17777 -> guest:7777 so the test script (on
        # the host) can reach the daemon's listener.  Same trick
        # as test_passive_open.py.
        "-netdev", f"user,id=n0,hostfwd=tcp::{HOST_PORT}-:{GUEST_PORT}",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out


def wait_for(s, needle, timeout):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def dial_guest():
    """Open a TCP connection to the guest's echod via SLIRP hostfwd.

    Retries because SLIRP's host-side listening socket is open
    immediately at QEMU start but the guest's echod isn't bound
    until partway through boot + shell launch.  Connection-refused
    is the expected error during that window.
    """
    deadline = time.time() + 15.0
    last_err = None
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5.0)
            s.connect(("127.0.0.1", HOST_PORT))
            return s
        except OSError as e:
            last_err = e
            time.sleep(0.25)
    raise RuntimeError(f"could not connect to host:{HOST_PORT}: {last_err}")


def main():
    q = boot()
    try:
        ser = conn()

        # Wait for the shell prompt.  We need a long timeout
        # here because the kernel's phase-7 self-test (ch103)
        # busy-polls tcp_accept for ~30s waiting for the
        # passive-open harness to dial port 8088.  We're not
        # that harness (we use port 7777), so the kernel will
        # time out with "no inbound connection" and then reach
        # init/sh.  Budget 120s to be safe across CI variance.
        log = wait_for(ser, b"$ ", 120.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: shell prompt available")

        # Run echod in the foreground.  --once makes it exit
        # after one peer so the shell becomes interactive again.
        ser.sendall(b"echod 7777 --once\n")

        log += wait_for(ser, b"echod: listening on port 7777", 15.0)
        if b"echod: listening on port 7777" not in log:
            print("FAIL: echod never logged its listen line")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: echod listening on guest port 7777")

        # Dial the listener and run the echo handshake.
        try:
            client = dial_guest()
        except Exception as e:
            print(f"FAIL: could not dial guest echod: {e}")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: host -> guest TCP handshake via SLIRP hostfwd")

        client.sendall(PAYLOAD)
        # Half-close so echod's read() returns 0 once our bytes
        # are drained and it exits the per-peer loop.
        try: client.shutdown(socket.SHUT_WR)
        except OSError: pass

        # Read all the echoed bytes back.  echod sends them in
        # one or more chunks; loop until peer closes or we hit
        # the full payload size.
        client.settimeout(20.0)
        got = b""
        try:
            while len(got) < len(PAYLOAD):
                chunk = client.recv(4096)
                if not chunk: break
                got += chunk
        except (OSError, socket.timeout):
            pass
        client.close()

        if got != PAYLOAD:
            print(f"FAIL: echoed bytes mismatch")
            print(f"  sent={PAYLOAD!r}")
            print(f"  got ={got!r}")
            return 1
        print(f"PASS: echod echoed all {len(PAYLOAD)} bytes verbatim")

        # echod should now log accept + bytes echoed + done.
        log += wait_for(ser, b"echod: accepted from", 10.0)
        if b"echod: accepted from" not in log:
            print("FAIL: echod never logged the accept line")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: echod logged accept (peer addr surfaced)")

        log += wait_for(ser, b"echod: closed peer; echoed ", 10.0)
        if b"echod: closed peer; echoed " not in log:
            print("FAIL: echod never logged the per-peer close line")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: echod closed peer cleanly")

        log += wait_for(ser, b"echod: done", 10.0)
        if b"echod: done" not in log:
            print("FAIL: echod never exited (no 'done' line)")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: echod exited cleanly after --once")

        print("\nCHAPTER 104 (accept()+echod): ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_passive_open.py -- chapter 103 / M92 passive-open test.

Boots the kernel with SLIRP's hostfwd configured so that the
host's TCP port 18088 is forwarded into the guest's TCP port 8088.
The kernel's boot self-test calls tcp_listen(8088) and waits for
an inbound SYN.  This script:

  1. spawns QEMU with the hostfwd rule,
  2. waits for the kernel to log "TCP listen on port 8088",
  3. opens a TCP connection from the host to 127.0.0.1:18088,
  4. sends a short payload, half-closes, drains, full-closes,
  5. verifies the kernel logged the expected accept + drain +
     clean-close lines.

Passing this test means the kernel can:
  - allocate a TCP_LISTEN slot at a fixed port,
  - match an unknown 4-tuple SYN against that listener,
  - allocate a child slot in TCP_SYN_RECEIVED,
  - send a valid SYN+ACK and pick up the peer's final ACK,
  - promote the child to TCP_ESTABLISHED and surface it via
    tcp_accept,
  - drain peer data + handle the peer's FIN + send its own FIN.

Why hostfwd: SLIRP user-mode networking is NAT-only by default
(guest can dial out, host can't dial in).  Adding hostfwd=tcp::
18088-:8088 punches a single mapping so the host can reach the
guest's listener without us having to bring up tap networking
on the host.  See chapter 103's prose for the SLIRP discussion.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-passive.sock"
HOST_PORT  = 18088   # what the host script connects to
GUEST_PORT = 8088    # what the kernel listens on inside QEMU

PAYLOAD = b"hello from host\n"   # what we send into the kernel


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
        # Chapter 103: hostfwd punches host:18088 -> guest:8088
        # through SLIRP so our test script (running on the host)
        # can dial the kernel's listener.
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
    """Open a TCP connection to the kernel via SLIRP's hostfwd.

    Retries briefly: the kernel calls tcp_listen() during boot,
    but SLIRP's listening socket on the host side is set up by
    QEMU at process start (before the guest has even booted), so
    we may succeed even before the guest is ready -- and the
    initial SYN may then sit in SLIRP's backlog until the guest's
    TCP stack picks it up.  Either way, a short retry loop is the
    most reliable way to handle the race.
    """
    deadline = time.time() + 30.0
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
    q  = boot()
    try:
        ser = conn()

        # Phase 1: kernel announces the listener.
        log = wait_for(ser, b"TCP listen on port 8088", 60.0)
        if b"TCP listen on port 8088" not in log:
            print("FAIL: kernel never reached TCP listen phase")
            print(log[-2000:].decode("ascii","replace"))
            return 1
        print("PASS: kernel created listener on port 8088")

        # Phase 2: open a TCP connection from the host.
        try:
            client = dial_guest()
        except Exception as e:
            print(f"FAIL: could not dial guest via hostfwd: {e}")
            print(log[-2000:].decode("ascii","replace"))
            return 1
        print(f"PASS: host -> guest TCP handshake via SLIRP hostfwd")

        # Phase 3: send a payload, half-close, drain, full-close.
        client.sendall(PAYLOAD)
        try:
            client.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        # Read until peer FIN -- the kernel will call tcp_close after
        # tcp_eof, which sends its own FIN.
        client.settimeout(20.0)
        try:
            _ = client.recv(4096)
        except (OSError, socket.timeout):
            pass
        client.close()
        print("PASS: payload sent, half-close + drain + full-close issued")

        # Phase 4: kernel must log the accept.
        log += wait_for(ser, b"TCP accepted cid=", 30.0)
        if b"TCP accepted cid=" not in log:
            print("FAIL: kernel never reported a successful accept")
            print(log[-2000:].decode("ascii","replace"))
            return 1
        print("PASS: kernel reported successful accept")

        # Phase 5: kernel must log the byte count of the payload it
        # drained.  We don't assert an exact value (off-by-one with
        # CRLF + SLIRP buffering would flake), but it must be > 0.
        log += wait_for(ser, b"TCP accept payload bytes=", 30.0)
        if b"TCP accept payload bytes=" not in log:
            print("FAIL: kernel never logged the drained byte count")
            print(log[-2000:].decode("ascii","replace"))
            return 1
        # Parse the hex byte count.
        n = 0
        for line in log.decode("ascii","replace").splitlines():
            if "TCP accept payload bytes=" in line:
                hexpart = line.split("=")[-1].strip()
                try: n = int(hexpart, 16)
                except ValueError: n = 0
                break
        if n < len(PAYLOAD):
            print(f"FAIL: kernel drained {n} bytes, expected >= {len(PAYLOAD)}")
            return 1
        print(f"PASS: kernel drained {n} bytes ({len(PAYLOAD)} sent)")

        # Phase 6: kernel must complete its passive close cleanly.
        log += wait_for(ser, b"TCP passive close complete", 30.0)
        if b"TCP passive close complete" not in log:
            print("FAIL: kernel never finished its passive close")
            print(log[-2000:].decode("ascii","replace"))
            return 1
        print("PASS: kernel completed passive close cleanly")

        # Phase 7: kernel reaches a shell prompt afterwards (sanity).
        log += wait_for(ser, b"$ ", 30.0)
        if b"$ " not in log:
            print("FAIL: kernel did not reach shell prompt after passive test")
            return 1
        print("PASS: shell prompt reached after passive-open test")

        print("\nCHAPTER 103 (passive open / LISTEN+SYN_RECEIVED): ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

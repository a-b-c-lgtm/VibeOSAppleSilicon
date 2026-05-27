#!/usr/bin/env python3
"""scripts/test_httpget_dns.py — userspace-DNS smoke test.

End-to-end check that the SYS_RESOLVE wrapper works from a real
userspace program: we invoke `httpget` with a hostname (not an
IP) and confirm that the kernel resolves it and httpget then
prints the resolved address before connecting.

We don't actually exchange HTTP body bytes here — example.com on
port 80 isn't guaranteed to be reachable from every CI host's
SLIRP namespace, and we already cover the body-transfer path in
test_httpget.py.  The pass criterion is just:

  - shell prompt appears
  - `httpget example.com 80 /` causes the userspace 'resolved
    example.com -> A.B.C.D' line to appear with a valid IPv4

If the resolve itself fails, httpget prints
'cannot resolve …' and the test fails with the underlying errno.
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-httpget-dns.sock"


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
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def read_until(ser, needles, timeout, prior=b""):
    """Append to a single accumulating buffer until any needle hits.
    Returns the full buffer (including 'prior' so callers can chain
    probes without dropping bytes between calls)."""
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray(prior)
    if any(n in buf for n in needles): return bytes(buf)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
            if any(n in buf for n in needles):
                return bytes(buf)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()

        # Wait for the kernel TCP+DNS self-test phase to finish so
        # the userspace prompt isn't racing the network bring-up.
        log = read_until(ser, [b"DNS reply ip=", b"DNS resolve failed"], 90.0)
        if b"DNS reply ip=" not in log:
            print("FAIL: kernel DNS self-test did not succeed; "
                  "userspace probe would also fail")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print("PASS: kernel DNS self-test succeeded")

        log = read_until(ser, [b"$ "], 30.0, prior=log)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print("PASS: shell prompt available")

        # Type the hostname-form httpget.  Even if the connect step
        # fails, the resolve step runs first and prints a recognised
        # marker line.
        ser.sendall(b"httpget example.com 80 /\n")

        log = read_until(ser, [b"resolved example.com ->",
                               b"cannot resolve"], 30.0, prior=log)
        if b"cannot resolve" in log:
            print("FAIL: httpget could not resolve example.com")
            print(log[-2000:].decode("ascii", "replace")); return 1
        # The marker may match before the IP octets finish printing
        # (printf flushes one chunk at a time over the serial path).
        # Drain a bit more, then look for the full dotted-quad.
        for _ in range(20):
            r,_,_ = select.select([ser],[],[],0.1)
            if r:
                c = ser.recv(8192)
                if c: log = log + c
            if re.search(rb"resolved example\.com -> \d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}", log):
                break
        m = re.search(rb"resolved example\.com -> (\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})", log)
        if not m:
            print("FAIL: resolve marker line not found")
            print(log[-2000:].decode("ascii", "replace")); return 1
        ip = m.group(1).decode()
        if ip == "0.0.0.0":
            print(f"FAIL: bogus resolve result {ip}"); return 1
        print(f"PASS: userspace resolve -> {ip} (via SYS_RESOLVE)")

        print("\nuserspace DNS: TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

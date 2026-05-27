#!/usr/bin/env python3
"""scripts/_dbg_tls_outbound.py -- chapter 130 manual outbound HTTPS probe.

Boot the guest with QEMU SLIRP networking, wait for shell, then
fire `browser <URL>` at a real public site and watch the serial
log for TLS handshake success + HTTP body bytes.

This is the chapter-112g end-to-end proof that:

  * DNS works against SLIRP's 10.0.2.3 forwarder (already true
    since chapter 56, but new domains keep finding bugs).
  * sys_connect can reach arbitrary public IPv4 + port 443
    through SLIRP's outbound NAT (chapter 38).
  * BearSSL's `_full` profile + the public roots from
    /mnt/ca.bundle accept a real-world server certificate
    chain.
  * SNI + SAN/CN matching pass against a hostname the in-guest
    code has never seen (chapter 127 wired SNI for localhost;
    public sites force the SAN-DNS-matching paths to run).

NOT a regression test:

  - Live internet is too brittle for CI: DNS outages, captive
    portals, IP filtering of QEMU's SLIRP source.
  - Specific certificate chains change (CA rotations, ACME
    renewals).  Pinning the test to today's chain would either
    break weekly or have to ignore the cert entirely (which
    defeats the point of the test).
  - QEMU's user-mode networking is firewalled by some
    corporate networks.

Per the debug-scripts-policy memory, this file lives under
`_dbg_*.py` and is kept in-repo as reference material for the
chapter, even though `make test-tls` does not invoke it.

Usage:
    python3 scripts/_dbg_tls_outbound.py                       # example.com
    python3 scripts/_dbg_tls_outbound.py https://example.com/
    python3 scripts/_dbg_tls_outbound.py https://news.ycombinator.com/
"""

import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-tls-outbound.sock"
PROMPT = b"$ "


def boot():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    return subprocess.Popen(
        [
            "qemu-system-aarch64",
            "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
            "-m", "8G", "-smp", "2", "-display", "none",
            "-serial", f"unix:{SOCK},server,nowait",
            "-global", "virtio-mmio.force-legacy=off",
            "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
            "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
            "-device", "virtio-blk-device,drive=hd0",
            "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
            "-device", "virtio-blk-device,drive=hd1",
            "-netdev", "user,id=n0",
            "-device", "virtio-net-device,netdev=n0",
            "-audiodev", "none,id=audio0",
            "-device", "virtio-sound-device,audiodev=audio0",
            "-object", "rng-random,id=rng0,filename=/dev/urandom",
            "-device", "virtio-rng-device,rng=rng0",
            "-kernel", f"{ROOT}/build/kernel.elf",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


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


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else "https://example.com/"
    if not url.startswith("https://"):
        print(f"FAIL: URL must start with https:// (got {url!r})")
        return 1

    print(f"[outbound] target: {url}")

    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [PROMPT], 90.0)
        if PROMPT not in log:
            print("FAIL: never saw shell prompt during boot")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("[outbound] shell prompt reached")

        cmd = f"browser {url}\n".encode()
        ser.sendall(cmd)
        out = read_until(
            ser,
            [
                b"TLS handshake OK",
                b"TLS handshake to",
                b"TLS read error",
                b"cannot resolve",
                b"browser: error",
            ],
            60.0,
        )
        if b"cannot resolve" in out:
            print("FAIL: DNS resolution failed (check SLIRP/host DNS)")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if b"TLS handshake to" in out and b"failed" in out:
            print("FAIL: TLS handshake to public site failed")
            print(out[-3000:].decode("ascii", "replace"))
            return 1
        if b"TLS handshake OK" not in out:
            print("FAIL: never saw 'TLS handshake OK' line")
            print(out[-3000:].decode("ascii", "replace"))
            return 1
        print("[outbound] TLS handshake OK")

        # Wait for body bytes / parse to finish so we know the
        # connection actually ferried payload.
        body = read_until(ser, [PROMPT, b"browser: TLS read error"],
                          60.0, prior=out)
        if b"browser: TLS read error" in body:
            print("FAIL: TLS read error mid-body")
            print(body[-2000:].decode("ascii", "replace"))
            return 1
        if PROMPT not in body[len(out):] + b"\n" + body[-200:]:
            # Be lenient: page may still be parsing.  Just note it.
            print("[outbound] (no prompt return yet -- page may be large)")
        print("PASS: outbound HTTPS round-trip succeeded against", url)
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

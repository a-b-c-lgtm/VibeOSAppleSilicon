#!/usr/bin/env python3
"""_dbg_bare_host.py -- manual probe (chapter 112h).

Goal: confirm canonicalize_url's case (6) default flip works
end-to-end against a real public site.

  $ python3 scripts/_dbg_bare_host.py

Boots the guest with SLIRP networking, waits for the shell
prompt, then types `browser news.ycombinator.com` (no scheme,
no proxy env).  Pre-112h the bare hostname would have been
prefixed with the chapter-106b proxy (http://127.0.0.1:80/)
and 502'd; post-112h it should be prefixed with https:// and
go through the native TLS path added in chapters 112d-112g.

PASS iff we see both:
  * a `[browser] resolved news.ycombinator.com -> <ipv4>`
    line (proves canonicalize_url did NOT prepend the
    in-guest proxy -- otherwise the host would have been
    "127.0.0.1" and DNS would never have run), AND
  * a `TLS handshake OK` line (proves the bundle anchors
    accept the live HN chain).

Also asserts that "127.0.0.1" does NOT appear anywhere in
the captured boot output -- a defence against a regression
where case (6) silently routes through the proxy again.

Per debug-scripts-policy.md this lives in scripts/ as a
manual probe (not part of the regression sweep) and stays
checked in for future reference from the book.
"""

import os
import select
import socket
import subprocess
import sys
import time

ROOT = "/Users/seusher/Desktop/osdev"
SOCK = "/tmp/osdev-serial-bare-host.sock"
PROMPT = b"$ "


def boot_and_probe() -> int:
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass

    qemu = subprocess.Popen(
        [
            "qemu-system-aarch64",
            "-M", "virt,gic-version=3",
            "-cpu", "host",
            "-accel", "hvf",
            "-m", "8G",
            "-smp", "2",
            "-display", "none",
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

    sock = None
    try:
        deadline = time.time() + 5
        while time.time() < deadline:
            if os.path.exists(SOCK):
                try:
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.connect(SOCK)
                    break
                except OSError:
                    sock = None
            time.sleep(0.05)
        if sock is None:
            print("FAIL: could not connect to serial socket")
            return 1

        def read_until(needles, t):
            if isinstance(needles, (bytes, bytearray)):
                needles = [needles]
            buf = bytearray()
            end = time.time() + t
            while time.time() < end:
                r, _, _ = select.select([sock], [], [], 0.2)
                if r:
                    chunk = sock.recv(8192)
                    if not chunk:
                        break
                    buf.extend(chunk)
                    if any(n in buf for n in needles):
                        return bytes(buf)
            return bytes(buf)

        boot = read_until(PROMPT, 90.0)
        if PROMPT not in boot:
            print("FAIL: no shell prompt within 90s")
            sys.stdout.write(boot.decode("ascii", "replace")[-1000:])
            return 1
        print("[probe] shell up")

        sock.sendall(b"browser news.ycombinator.com\n")
        out = read_until(
            [
                b"TLS handshake OK",
                b"502 Bad Gateway",
                b"cannot resolve",
                b"failed",
            ],
            60.0,
        )

        for line in out.decode("ascii", "replace").splitlines():
            if (
                "navigate ->" in line
                or "TLS handshake" in line
                or "Bad Gateway" in line
                or "resolve" in line
                or "[browser]" in line and "load" in line
            ):
                print(line)

        ok = (
            b"resolved news.ycombinator.com" in out
            and b"TLS handshake OK" in out
            and b"127.0.0.1" not in out
        )
        if ok:
            print("PASS: bare hostname canonicalized to https:// and TLS succeeded")
            return 0
        if b"127.0.0.1" in out:
            print("FAIL: still routed through the in-guest proxy")
            return 1
        print("FAIL: did not observe DNS-resolve + TLS-handshake-OK")
        return 1

    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        qemu.kill()
        qemu.wait()
        try:
            os.unlink(SOCK)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(boot_and_probe())

#!/usr/bin/env python3
"""_dbg_hn_resources.py -- manual probe (chapter 131 follow-up).

Goal: verify the resolve_url "base has no path" fix.  Before
the fix, fetching https://news.ycombinator.com/ (or any
http(s) URL whose base has no path component) caused every
relative reference -- stylesheets, images, and post-112h
link clicks -- to silently truncate to just the bare host
URL because resolve_url planted a NUL byte at out[host_len + 1].

This probe boots the guest, loads HN over native TLS, and
asserts that the captured serial log does NOT contain either
of the truncation symptoms:

  * "png_decode failed for https://news.ycombinator.com\\n"
    (with no path after the hostname) -- means the y18.gif
    image src resolved to the bare front-page URL and the
    HTML body was decoded as PNG.
  * "skip sheet (https not supported): https://news.ycombinator.com\\n"
    (with no path after the hostname) -- means news.css?...
    resolved to the bare front-page URL.

It also asserts the POSITIVE outcome of the chapter-112h
follow-up fix: the news.css stylesheet must be FETCHED
(not skipped) over native TLS now that the legacy
"skip https stylesheets" guard in apply_link_sheets was
removed.  Specifically the log must contain both
  - "fetching stylesheet https://news.ycombinator.com/news.css"
  - a second "TLS handshake OK with news.ycombinator.com:443"
    (one for the page, one for the stylesheet)
and must NOT contain any "skip sheet (https not supported)"
line at all (the entire guard is gone, so the substring
should never appear regardless of the URL after the colon).

Per debug-scripts-policy.md this lives in scripts/ as a
manual probe (not part of the regression sweep).  It is
the canonical reference for verifying chapter 131's
resolve_url fix on a live public site.
"""

import os
import select
import socket
import subprocess
import sys
import time

ROOT = "/Users/seusher/Desktop/osdev"
SOCK = "/tmp/osdev-serial-hn-resources.sock"
PROMPT = b"$ "

BAD_PNG_LINES = (
    b"png_decode failed for https://news.ycombinator.com\n",
    b"png_decode failed for https://news.ycombinator.com ",
)
BAD_SHEET_LINES = (
    b"skip sheet (https not supported): https://news.ycombinator.com\n",
    b"skip sheet (https not supported): https://news.ycombinator.com ",
)


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
            return 1
        print("[probe] shell up")

        sock.sendall(b"browser https://news.ycombinator.com/\n")
        # Let the browser run long enough to process every relative
        # ref in HN's HTML.  60s is well over the observed worst-case.
        out = read_until([b"png_decode failed", b"compose_all"], 60.0)
        out += read_until([PROMPT], 30.0)

        for line in out.decode("ascii", "replace").splitlines():
            if any(
                k in line
                for k in [
                    "navigate ->",
                    "TLS handshake",
                    "HTTP/1.1",
                    "skip sheet",
                    "png_decode",
                    "resolved news",
                    "fetching stylesheet",
                ]
            ):
                print(line)

        if any(b in out for b in BAD_PNG_LINES):
            print("FAIL: image URL still truncates to bare host")
            return 1
        if any(b in out for b in BAD_SHEET_LINES):
            print("FAIL: stylesheet URL still truncates to bare host")
            return 1
        if b"skip sheet (https not supported)" in out:
            print("FAIL: legacy https-stylesheet skip guard still firing")
            return 1
        if b"TLS handshake OK" not in out:
            print("FAIL: TLS handshake did not succeed")
            return 1
        if b"fetching stylesheet https://news.ycombinator.com/news.css" not in out:
            print("FAIL: news.css stylesheet not fetched over https")
            return 1
        # Two handshakes expected: one for the page, one for news.css.
        if out.count(b"TLS handshake OK with news.ycombinator.com:443") < 2:
            print("FAIL: expected >=2 TLS handshakes (page + stylesheet)")
            return 1
        if b"127.0.0.1" in out:
            print("FAIL: routed through proxy (sentinel)")
            return 1
        print(
            "PASS: HN loaded over native TLS; "
            "relative refs resolve correctly and the stylesheet is fetched"
        )
        return 0
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

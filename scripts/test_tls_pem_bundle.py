#!/usr/bin/env python3
"""scripts/test_tls_pem_bundle.py -- chapter 112f PEM ingest +
recursive chain validation.

Chapter 112e shipped /mnt/ca.bundle with the BearSSL sample
INTERMEDIATE CAs as trust anchors.  That validated the leaf
directly: one signature check, no recursion.

Chapter 112f promotes the bundle's anchors to the BearSSL sample
ROOT CAs (cert-root-{rsa,ec}.pem, ingested via the new --pem
mode in scripts/mkcabundle.py).  httpsd still serves the full
chain leaf+intermediate, so the validator must now walk two
links:

    server-presented leaf  --sig-by--> server-presented intermediate
                                              --sig-by--> trusted root (anchor)

Passing this test means:

  1. PEM extraction in mkcabundle.py round-trips at least one
     real-world-formatted PEM through to a DER-in-bundle entry.
  2. The validator walks chains of depth > 1 (not just the
     direct-trust trivial case from 112e).
  3. The host-side bundle generator and the guest-side bundle
     parser agree on the framed CAB1 layout end-to-end.

We assert this two ways:

  - The mkcabundle banner ran in `make` output is checked
    indirectly: the bundle size (1208 bytes for the two root
    PEMs) and the magic ("CAB1") are visible in the
    browser's `[browser] loaded /mnt/ca.bundle (NNNN bytes,
    magic=CAB1)` line.
  - The fetch succeeds against both 8443 and 8444 with
    `source=bundle` and the body-token check; the only way
    that can happen is if the validator did the full walk.
"""

import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-tls-pem-bundle.sock"


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


PROMPT = b"$ "


def main():
    # First, verify the host-side bundle file independently.
    bundle_path = os.path.join(ROOT, "assets/osfs/ca.bundle")
    if not os.path.exists(bundle_path):
        print(f"FAIL: {bundle_path} missing; did `make` run?")
        return 1
    with open(bundle_path, "rb") as f:
        bundle = f.read()
    if bundle[:4] != b"CAB1":
        print(f"FAIL: bundle magic = {bundle[:4]!r}, expected b'CAB1'")
        return 1
    count = int.from_bytes(bundle[4:8], "little")
    # Chapter 112f shipped exactly the two BearSSL sample roots
    # (count == 2).  Chapter 112g folds the host's public CA list
    # in alongside them, so count is now "at least 2".  The
    # bearssl-only minimum is what proves the 112f code path:
    # recursive chain validation against a root-only anchor list.
    if count < 2:
        print(f"FAIL: bundle anchor count = {count}, expected >= 2 "
              f"(at minimum: RSA root + ECDSA root from BearSSL)")
        return 1
    print(f"PASS: /mnt/ca.bundle host-side: magic=CAB1, count={count}, "
          f"size={len(bundle)} bytes")

    q = boot()
    try:
        ser = conn()

        boot_log = read_until(ser, [PROMPT], 90.0)
        if PROMPT not in boot_log:
            print("FAIL: never saw shell prompt during boot")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # Drive the browser against the RSA loopback first.
        # The interesting line is the `loaded /mnt/ca.bundle` one
        # from chapter-112e load_ca_bundle_once.
        ser.sendall(b"browser https://localhost:8443/\n")
        out = read_until(
            ser,
            [PROMPT, b"browser: TLS handshake to", b"browser: TLS read error"],
            60.0,
        )
        if b"browser: TLS handshake to" in out:
            print("FAIL: browser reported handshake failure (recursive "
                  "chain walk broken?)")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if b"loaded /mnt/ca.bundle" not in out:
            print("FAIL: bundle load line missing")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if b"magic=CAB1" not in out:
            print("FAIL: bundle magic 'CAB1' missing from load line")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: browser loaded /mnt/ca.bundle (magic=CAB1)")

        if b"source=bundle" not in out:
            print(
                "FAIL: TLS-OK line missing source=bundle "
                "(fallback to in-binary anchor?)"
            )
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if b"handshake" not in out or b"osdev-httpsd" not in out:
            print("FAIL: decrypted body tokens missing")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: RSA leaf validated via 2-link chain walk to root anchor")

        # ECDSA fetch.  Same bundle, different anchor.  This is
        # the second proof of the chain-walk: the EC root signed
        # the EC intermediate, which signed the EC leaf.
        ser.sendall(b"browser https://localhost:8444/\n")
        out2 = read_until(
            ser,
            [PROMPT, b"browser: TLS handshake to", b"browser: TLS read error"],
            60.0,
        )
        if b"browser: TLS handshake to" in out2:
            print("FAIL: EC handshake failed (recursive chain walk "
                  "broken for ECDSA?)")
            print(out2[-2000:].decode("ascii", "replace"))
            return 1
        if b"source=bundle" not in out2:
            print("FAIL: EC fetch did not use bundle anchor")
            print(out2[-2000:].decode("ascii", "replace"))
            return 1
        if b"handshake" not in out2 or b"osdev-httpsd" not in out2:
            print("FAIL: EC decrypted body tokens missing")
            print(out2[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: ECDSA leaf validated via 2-link chain walk to root anchor")

        print("PASS: chapter 112f PEM ingest + recursive chain walk")
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

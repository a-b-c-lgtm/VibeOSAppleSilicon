#!/usr/bin/env python3
"""scripts/test_browser_https_multi.py -- chapter 112e multi-anchor trust store.

Boots the kernel, waits for the shell prompt, then runs

    browser https://localhost:8443/   # RSA-signed cert chain
    browser https://localhost:8444/   # ECDSA P-256 cert chain

against the two in-guest /bin/httpsd instances spawned by init
(chapter 112e adds the --ec on :8444 alongside the original
:8443 from chapter 112b).

Asserts that:

  1. The browser loaded /mnt/ca.bundle at startup (printed by
     load_ca_bundle_once -- "[browser] loaded /mnt/ca.bundle
     (NNNN bytes, magic=CAB1)").
  2. Both handshakes succeeded, with the "source=bundle" tag
     in the [browser] TLS-OK line (i.e. the bundle path was
     used, not the in-binary fallback).
  3. Both decrypted response bodies reached the renderer
     (marker tokens "handshake" and "osdev-httpsd" appear after
     each fetch).
  4. The browser process printed the EC-specific marker
     ("ECDSA P-256" from httpsd's own startup line) for the
     8444 instance, proving the EC chain TU was linked in.

If chapter 112d's single-anchor path regressed (i.e. the
bundle is missing and the browser fell back to the in-binary
anchor), we'd still pass against :8443 but would fail against
:8444 because the in-binary anchor only signs the RSA leaf.
That asymmetry is exactly what makes :8444 a useful test of
the multi-anchor refactor.
"""

import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-browser-https-multi.sock"


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


# The body marker the in-guest httpsd serves regardless of port
# (chapter 112b k_body).  The plain-text renderer collapses
# whitespace so we look for individual tokens.
BODY_TOKENS = (b"handshake", b"osdev-httpsd")
PROMPT = b"$ "


def fetch(ser, url, expect_anchor_source):
    """Run `browser url`, return (out_bytes, ok_bool, msg)."""
    cmd = f"browser {url}\n".encode()
    ser.sendall(cmd)
    fail_handshake = f"TLS handshake to {url.split('//',1)[1].rstrip('/')} failed".encode()
    fail_tls_read  = b"browser: TLS read error"
    fail_notsupp   = b"https:// is not yet supported"
    out = read_until(
        ser,
        [PROMPT, fail_handshake, fail_tls_read, fail_notsupp],
        60.0,
    )
    if fail_notsupp in out:
        return out, False, "pre-112d 'https:// not supported' line"
    if fail_handshake in out:
        return out, False, "TLS handshake failed"
    if fail_tls_read in out:
        return out, False, "TLS read error mid-stream"
    if b"TLS handshake OK with" not in out:
        return out, False, "browser never printed the TLS-OK line"
    if expect_anchor_source.encode() not in out:
        return out, False, (
            f"TLS-OK line missing 'source={expect_anchor_source}' "
            "(bundle path not used?)"
        )
    missing = [t for t in BODY_TOKENS if t not in out]
    if missing:
        return out, False, (
            "decrypted body tokens missing: "
            + ", ".join(t.decode() for t in missing)
        )
    return out, True, ""


def main():
    q = boot()
    try:
        ser = conn()

        # 1. Boot up to the shell.
        boot_log = read_until(ser, [PROMPT], 90.0)
        if PROMPT not in boot_log:
            print("FAIL: never saw shell prompt during boot")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # 2. Confirm both httpsds spawned.
        if b"/bin/httpsd 8443" not in boot_log:
            print("WARN: no '/bin/httpsd 8443' line seen in boot log")
        else:
            print("PASS: in-guest httpsd (RSA) is up on port 8443")

        if b"/bin/httpsd --ec 8444" not in boot_log:
            print("FAIL: no '/bin/httpsd --ec 8444' line seen in boot log")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        if b"ECDSA P-256" not in boot_log:
            print(
                "FAIL: boot log does not show httpsd printing the "
                "'ECDSA P-256' tag (EC chain TU not linked?)"
            )
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: in-guest httpsd (ECDSA P-256) is up on port 8444")

        # 3. RSA fetch.
        rsa_out, rsa_ok, rsa_msg = fetch(
            ser, "https://localhost:8443/", "source=bundle"
        )
        if not rsa_ok:
            print(f"FAIL (RSA :8443): {rsa_msg}")
            print(rsa_out[-2000:].decode("ascii", "replace"))
            return 1
        if b"loaded /mnt/ca.bundle" not in rsa_out:
            print(
                "FAIL: browser never printed the "
                "'loaded /mnt/ca.bundle' line on first https:// fetch"
            )
            print(rsa_out[-2000:].decode("ascii", "replace"))
            return 1
        if b"magic=CAB1" not in rsa_out:
            print("FAIL: bundle magic 'CAB1' missing from load line")
            print(rsa_out[-2000:].decode("ascii", "replace"))
            return 1
        print(
            "PASS: RSA fetch (https://localhost:8443/) "
            "validated via /mnt/ca.bundle"
        )

        # 4. EC fetch.  Same trust store, different anchor.
        ec_out, ec_ok, ec_msg = fetch(
            ser, "https://localhost:8444/", "source=bundle"
        )
        if not ec_ok:
            print(f"FAIL (EC :8444): {ec_msg}")
            print(ec_out[-2000:].decode("ascii", "replace"))
            return 1
        print(
            "PASS: ECDSA fetch (https://localhost:8444/) "
            "validated via /mnt/ca.bundle (same multi-anchor store)"
        )

        print(
            "PASS: chapter 112e multi-anchor trust store "
            "(RSA + ECDSA, bundle-sourced) end-to-end"
        )
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

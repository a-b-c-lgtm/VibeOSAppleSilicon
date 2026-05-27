#!/usr/bin/env python3
"""scripts/test_tls_chain.py -- chapter 126 X.509 chain validation.

Boots the kernel, drops to /bin/sh, runs

    tlstest --handshake-ca 127.0.0.1 8443

against the in-guest httpsd, and asserts that the BearSSL
"minimal" X.509 validator -- NOT the knownkey shortcut from
chapter 125 -- accepts the sample chain against a trust anchor
built at runtime from the intermediate CA cert.

The chain in vendor/bearssl/samples/chain-rsa.h is:

    CERT0 (leaf, CN=localhost) <-- CERT1 (intermediate CA, self-signed)

We tell the validator to trust CERT1.  At handshake time the
server presents [CERT0, CERT1]; the validator walks back from
CERT0, verifies CERT0 was signed by CERT1's key, then matches
CERT1's subject DN against our anchor -- and accepts.  It also
checks notBefore/notAfter against the wall-clock the kernel got
from the PL031 RTC at boot (chapter 96); the sample cert is
valid 2010-01-01..2037-12-31, so this passes today.

Failure modes:
  - wrong epoch offset in tls_socket.c -> BR_ERR_X509_EXPIRED (54)
  - DN buffer too small or append_dn miswired -> BR_ERR_X509_NOT_TRUSTED (62)
  - pubkey copy wrong size -> BR_ERR_X509_BAD_SIGNATURE (52)
  - SNI mismatch -> BR_ERR_X509_BAD_SERVER_NAME (56)
  - SYS_GETTIMEOFDAY returning 0 -> BR_ERR_X509_EXPIRED (54)

Same QEMU command line as test_tls_handshake.py.
"""

import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-tls-chain.sock"


def boot():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
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
        "-audiodev", "none,id=audio0",
        "-device", "virtio-sound-device,audiodev=audio0",
        "-object", "rng-random,id=rng0,filename=/dev/urandom",
        "-device", "virtio-rng-device,rng=rng0",
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
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray(prior)
    if any(n in buf for n in needles): return bytes(buf)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
            if any(n in buf for n in needles): return bytes(buf)
    return bytes(buf)


PASS_LINE     = b"tls handshake: PASS chapter 126"
FAIL_LINE     = b"tlstest: FAIL"
ANCHOR_LINE   = b"built trust anchor from CA cert"
WALLCLOCK     = b"wallclock tv_sec="
HANDSHAKE_OK  = b"handshake complete"


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        ser.sendall(b"tlstest --handshake-ca 127.0.0.1 8443\n")
        out = read_until(ser, [PASS_LINE, FAIL_LINE], 30.0)

        if b"[svc] unknown syscall" in out:
            print("FAIL: tlstest hit unknown-syscall")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if b"kernel returned" in out:
            print("FAIL: tlstest reported a kernel error")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if FAIL_LINE in out:
            print("FAIL: tlstest emitted a failure line")
            idx = out.find(FAIL_LINE)
            print(out[max(0, idx - 400):idx + 400]
                  .decode("ascii", "replace"))
            return 1

        idx = out.find(b"tlstest --handshake-ca")
        tail = out[idx:] if idx >= 0 else out
        tail_flat = re.sub(rb"\s+", b" ", tail)

        if ANCHOR_LINE not in tail_flat:
            print("FAIL: tlstest never built the trust anchor")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: trust anchor built at runtime from CA cert DER")

        if WALLCLOCK not in tail_flat:
            print("FAIL: tlstest never reported wallclock tv_sec "
                  "(SYS_GETTIMEOFDAY failure?)")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        # Sanity-check the wallclock is in the sample cert validity
        # window (2010-01-01 .. 2037-12-31 in Unix seconds).
        m = re.search(rb"wallclock tv_sec=(\d+)", tail_flat)
        if m is None:
            print("FAIL: malformed wallclock line")
            return 1
        ts = int(m.group(1))
        # 1262304000 = 2010-01-01 UTC; 2145916800 = 2037-12-31 UTC
        if not (1262304000 <= ts <= 2145916800):
            print(f"FAIL: wallclock tv_sec={ts} outside cert validity "
                  f"window (2010..2037)")
            return 1
        print(f"PASS: wallclock tv_sec={ts} inside cert validity window")

        if HANDSHAKE_OK not in tail_flat:
            print("FAIL: TLS handshake with chain validator never completed")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: TLS handshake complete (chain validator, loopback)")

        if PASS_LINE not in tail:
            print("FAIL: tlstest never emitted PASS summary for 112c")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: chapter 126 X.509 chain validation end-to-end")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

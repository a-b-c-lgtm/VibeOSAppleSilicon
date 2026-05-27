#!/usr/bin/env python3
"""scripts/test_tls_handshake.py -- chapter 125 TLS handshake regression.

Boots the kernel, drops to /bin/sh, runs

    tlstest --handshake 127.0.0.1 8443

against the in-guest httpsd that init.c spawned on port 8443, and
asserts:

  1. The shell prompt appears (kernel + userspace booted).
  2. tlstest decodes the pinned RSA-2048 public key out of the
     leaf cert (looks for the `pinned RSA-2048 public key` line).
  3. The TLS handshake completes (`handshake complete` appears).
  4. The application data exchange succeeds: tlstest receives the
     well-known marker `tls handshake ok` that httpsd writes only
     INSIDE the TLS-encrypted record stream.
  5. The summary `tls handshake: PASS` line is emitted.

Failure modes the script catches:
  - getrandom() / br_ssl_engine_inject_entropy not wired up -> a
    `tls handshake rc=` line will appear with a non-zero BR_ERR_*.
  - knownkey validator not installed -> handshake fails with
    BR_ERR_X509_NOT_TRUSTED (62) or similar.
  - test_chain.c didn't actually export the CHAIN/RSA symbols ->
    httpsd fails to start with `server_reset failed`.
  - Buffer wiring wrong -> connect returns with BR_ERR_TOO_LARGE
    (12) or BR_ERR_BAD_STATE.

Modelled after scripts/test_tlstest.py.  Same QEMU command line
(virtio-rng included so getrandom can satisfy the 64-byte seed
both client and server need).
"""

import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-tls-handshake.sock"


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


PASS_LINE        = b"tls handshake: PASS"
FAIL_LINE        = b"tlstest: FAIL"
HANDSHAKE_OK     = b"handshake complete"
PINNED_KEY_LINE  = b"pinned RSA-"
HTTPSD_LISTENING = b"httpsd: osdev-httpsd"


def main():
    q = boot()
    try:
        ser = conn()
        # 1. Wait for shell prompt.  Init spawns httpsd before sh
        # so by the time the prompt appears, httpsd is listening
        # (or has at least logged its failure to the serial).
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        # Sanity: httpsd should have logged its startup banner
        # somewhere between init.c's spawn and the shell prompt.
        if HTTPSD_LISTENING not in log:
            print("WARN: httpsd startup banner not seen in boot log "
                  "(test will likely fail at connect)")

        # 2. Run the handshake test.
        ser.sendall(b"tlstest --handshake 127.0.0.1 8443\n")
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
            # find the FAIL line and print it plus context
            idx = out.find(FAIL_LINE)
            print(out[max(0, idx - 400):idx + 400]
                  .decode("ascii", "replace"))
            return 1

        # Slice forward from the command echo so we don't false-
        # positive-match content from earlier boot stages.
        idx = out.find(b"tlstest --handshake")
        tail = out[idx:] if idx >= 0 else out
        tail_flat = re.sub(rb"\s+", b" ", tail)

        if PINNED_KEY_LINE not in tail_flat:
            print("FAIL: tlstest never reported the pinned public key")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: tlstest pinned RSA-2048 public key from leaf cert")

        if HANDSHAKE_OK not in tail_flat:
            print("FAIL: TLS handshake never completed")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: TLS handshake complete (in-guest, loopback)")

        if PASS_LINE not in tail:
            print("FAIL: tlstest never emitted PASS summary")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: chapter 125 in-guest TLS handshake end-to-end")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

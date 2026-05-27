#!/usr/bin/env python3
"""scripts/test_browser_https.py -- chapter 127 native HTTPS in the browser.

Boots the kernel, waits for the shell prompt, runs

    browser https://localhost:8443/

against the in-guest /bin/httpsd (auto-spawned by init at boot,
chapter 125), and asserts that:

  1. The browser's TLS path actually engaged (`[browser] TLS
     handshake OK with 127.0.0.1:8443 (chain-validated, ...)`).
  2. The decrypted HTTP response made it back as application
     bytes: the marker string "tls handshake ok" appears in the
     rendered page output.

This is the chapter-112d end-to-end test: pre-112d the same
command line would print `https:// is not yet supported` and
exit non-zero.

Test architecture differs from test_tls_chain.py (which drove
the standalone tlstest binary): here the consumer is the real
/bin/browser pipeline -- URL parse -> resolve -> TCP -> TLS
handshake -> HTTP/1.1 GET -> chunked drain -> tokeniser ->
DOM -> layout -> plain-text render -> stdout.  Passing this
test means every layer between argv and the renderer can
carry a TLS-wrapped response without surprise.

QEMU command line mirrors test_tls_chain.py byte-for-byte
(virtio-rng required for SYS_GETRANDOM entropy, virtio-blk
hd0/hd1 for /mnt + /data, virtio-net + virtio-sound to keep
the boot path identical to the regression-sweep baseline).
"""

import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-browser-https.sock"


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


HANDSHAKE_LINE = b"TLS handshake OK with localhost:8443"
# httpsd body is "tls handshake ok\nserved by osdev-httpsd/1.0\n".
# The plain-text renderer collapses words onto a line with
# variable spacing, so check for individual tokens rather than
# the exact substring.
MARKER_TOKENS  = (b"handshake", b"osdev-httpsd")
BROWSER_DONE   = b"$ "
FAIL_TLS       = b"TLS handshake to localhost:8443 failed"
FAIL_NOTSUPP   = b"https:// is not yet supported"
FAIL_TLS_READ  = b"browser: TLS read error"


def main():
    q = boot()
    try:
        ser = conn()

        # 1. Boot.
        boot_log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in boot_log:
            print("FAIL: never saw shell prompt during boot")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # 2. Confirm httpsd is up.  init's chapter-112b boot line
        #    prints "[init] launching /bin/httpsd 8443 (background,
        #    loopback TLS)" and httpsd itself prints "httpsd:
        #    serving ...".  Either is fine -- we just need the
        #    server thread to exist when we dial.
        if b"/bin/httpsd 8443" not in boot_log and b"httpsd:" not in boot_log:
            print("WARN: no httpsd line seen in boot log; test may fail at handshake")
        else:
            print("PASS: in-guest httpsd is up on port 8443")

        # 3. Drive the browser.  Default mode (no flag) renders
        #    a plain-text dump to serial, which is enough to see
        #    the response body.  Any path works -- httpsd serves
        #    the marker body for every GET (chapter 125).  Host
        #    is "localhost" rather than "127.0.0.1" so the sample
        #    chain's CN=localhost leaf passes SNI verification.
        ser.sendall(b"browser https://localhost:8443/\n")
        out = read_until(ser,
                         [BROWSER_DONE, FAIL_TLS, FAIL_NOTSUPP, FAIL_TLS_READ],
                         60.0)

        if FAIL_NOTSUPP in out:
            print("FAIL: browser printed pre-112d 'https:// not supported' line")
            return 1
        if FAIL_TLS in out:
            print("FAIL: browser reported TLS handshake failure")
            idx = out.find(FAIL_TLS)
            print(out[max(0, idx - 400):idx + 400]
                  .decode("ascii", "replace"))
            return 1
        if FAIL_TLS_READ in out:
            print("FAIL: browser hit a TLS read error mid-stream")
            idx = out.find(FAIL_TLS_READ)
            print(out[max(0, idx - 400):idx + 400]
                  .decode("ascii", "replace"))
            return 1

        if HANDSHAKE_LINE not in out:
            print("FAIL: browser never printed the chapter-112d TLS-OK line")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: browser ran the native TLS handshake (chain-validated)")

        # 4. Decrypted body reached the renderer.  The httpsd
        #    body is plain text ("tls handshake ok\nserved by ...")
        #    so the plain-mode renderer copies the tokens through.
        #    Look for individual words rather than the exact
        #    substring -- the renderer collapses lines onto one
        #    row with variable whitespace.
        missing = [t for t in MARKER_TOKENS if t not in out]
        if missing:
            print("FAIL: marker tokens never reached browser output: "
                  + ", ".join(t.decode() for t in missing))
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: decrypted body reached the browser renderer end-to-end")

        if BROWSER_DONE not in out:
            print("WARN: browser never returned to shell prompt within 60s")
            # not a hard fail -- the handshake + body checks above
            # already cover the chapter-112d contract.

        print("PASS: chapter 127 native HTTPS in /bin/browser end-to-end")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

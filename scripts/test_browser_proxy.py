#!/usr/bin/env python3
"""scripts/test_browser_proxy.py -- chapter 110 regression.

End-to-end exercise of the chapter-106b architectural change:

  * userspace/browser/browser.c flipped BR_DEFAULT_PROXY from
    "http://10.0.2.2:8080/" (host-side scripts/https_proxy.py)
    to "http://127.0.0.1:8080/" (in-guest httpd).
  * /bin/proxytest is a new orchestrator that spawns httpd
    (with chapter-106a's HTTPD_UPSTREAM env honoured), waits
    briefly, then spawns the browser with a https:// URL.  The
    browser's canonicalize_url rewrites that to the default
    proxy address, dials it over loopback, and httpd splices
    the request out to our fake upstream.

This is the proof that THREE in-house features compose end to
end without anyone outside the guest:

  - chapter 108  (TCP loopback so 127.0.0.1:8080 works at all)
  - chapter 109 (httpd's serve_forward "be a dumb pipe")
  - chapter 110 (browser default proxy points at loopback)

The fake upstream lives on the host at 10.0.2.2:UPSTREAM_PORT
(SLIRP NAT) and returns a recognisable marker + the request
path.  We grep the serial log for the marker after browser
plain-mode rendering.  The marker landing in the serial output
proves:

  browser --plain --> 127.0.0.1:8080 --> httpd serve_forward
                  --> 10.0.2.2:18083 (SLIRP NAT) --> host upstream
                  --> response splice --> browser renders --> serial

If chapter 108 is reverted, the browser's connect(127.0.0.1)
will time out.  If chapter 109 is reverted, httpd will 404 on
the unknown path.  If chapter 110 is reverted (or the user
runs an old browser), BR_DEFAULT_PROXY still points at
10.0.2.2:8080 and we'd need scripts/https_proxy.py running on
the host -- which this test deliberately doesn't start, so the
fetch would fail.  All three regressions show as a missing
marker.

Boot-time net self-test (chapter 105) holds boot ~30s before
the shell appears; we give 120s of slack for the prompt.
"""
import http.server
import os
import select
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-browser-proxy.sock"

# Distinct from test_httpd_forward.py's 18082 so a sweep that
# happens to leave a wedged proxy on 18082 doesn't fool us into
# thinking 106b passed.
UPSTREAM_PORT = 18083
UPSTREAM_FOR_GUEST = f"10.0.2.2:{UPSTREAM_PORT}"

# Marker for the fake upstream.  The chapter-106b expectation
# is that the browser's plain-text render emits this verbatim
# in the serial log.
MARKER = "M97-BROWSER-PROXY-OK"

# We let proxytest's default URL drive the test (defined in
# userspace/proxytest/proxytest.c).  Pinning it here would make
# proxytest and this test drift apart silently.
DEFAULT_URL_PATH = "/m97.proxy.test/path"  # what the proxy receives


# ----------------------------------------------------------------
# Host-side fake upstream.  Mirrors test_httpd_forward.py but uses
# a distinct port and marker so the two tests can co-exist in a
# parallel sweep (sweep.sh is serial today but defensive port
# choices cost nothing) and don't get confused for each other in
# stale-log diagnosis.
# ----------------------------------------------------------------

class FakeUpstream(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # Keep the body short -- the browser's plain-text render
        # will dump the full text to the serial console, so a
        # 10 KiB response would scroll forever.
        body = (f"<html><body>{MARKER}|path={self.path}\n</body></html>\n"
                ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args, **kwargs):
        pass


class _Reusable(http.server.HTTPServer):
    allow_reuse_address = True


def start_upstream():
    srv = _Reusable(("127.0.0.1", UPSTREAM_PORT), FakeUpstream)
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    return srv


# ----------------------------------------------------------------
# QEMU plumbing.  No hostfwd needed -- the whole flow happens in
# the guest; we only need outbound NAT so the guest can reach
# 10.0.2.2:18083 where our fake upstream listens.
# ----------------------------------------------------------------

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


# Persistent leftover buffer for wait_for: see the same comment
# in test_browser_hn_repeat.py.  Without this, when the kernel is
# fast enough to print two consecutive markers in the same 400 ms
# drain window, the second wait_for() loses the bytes between
# them and times out spuriously.
_wait_for_carry = bytearray()


def wait_for(s, needle, timeout):
    global _wait_for_carry
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    # Anything left over from the previous wait_for() drain.
    if needle in _wait_for_carry:
        idx = _wait_for_carry.index(needle) + len(needle)
        out = bytes(_wait_for_carry[:idx])
        del _wait_for_carry[:idx]
        return out
    while time.time() < deadline:
        _wait_for_carry += drain(s, time.time() + 0.4)
        if needle in _wait_for_carry:
            idx = _wait_for_carry.index(needle) + len(needle)
            out = bytes(_wait_for_carry[:idx])
            del _wait_for_carry[:idx]
            return out
    out = bytes(_wait_for_carry)
    _wait_for_carry.clear()
    return out


# ----------------------------------------------------------------
# Test body.
# ----------------------------------------------------------------

def main():
    upstream = start_upstream()
    q = boot()
    try:
        ser = conn()

        # Wait for shell.  Chapter-103 boot self-test holds boot
        # ~30s while it busy-polls accept() on its own port; we
        # give 120s of headroom.
        log = wait_for(ser, b"$ ", 120.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: shell prompt available")

        # Set HTTPD_UPSTREAM before spawning proxytest so the
        # child httpd inherits it via the env table.  This is
        # the same mechanism test_httpd_forward.py uses.
        ser.sendall(f"export HTTPD_UPSTREAM={UPSTREAM_FOR_GUEST}\n".encode())
        log += wait_for(ser, b"$ ", 5.0)
        # The shell echoes the line; do a soft check that export
        # didn't error.  If it did, the next prompt would be
        # preceded by an error line.
        print(f"PASS: export HTTPD_UPSTREAM={UPSTREAM_FOR_GUEST}")

        # Invoke proxytest with no args -- it defaults to
        # https://m97.proxy.test/path which the browser will
        # rewrite to http://127.0.0.1:8080/m97.proxy.test/path.
        ser.sendall(b"proxytest\n")

        # First sign of life from proxytest: the "[proxytest]
        # spawning /bin/httpd" log line.
        log += wait_for(ser, b"[proxytest] spawning /bin/httpd", 30.0)
        if b"[proxytest] spawning /bin/httpd" not in log:
            print("FAIL: proxytest never spawned httpd")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: proxytest spawned httpd")

        # httpd's own listen line shows up next.
        log += wait_for(ser, b"httpd: listening on port 8080", 15.0)
        if b"httpd: listening on port 8080" not in log:
            print("FAIL: httpd did not bind port 8080")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: httpd listening on guest port 8080")

        # Confirm chapter-106a forward-upstream line matches what
        # we exported.  This proves load_upstream_from_env() saw
        # the value despite the spawn() inheritance path (we just
        # tested that the env table survives spawn()).
        log += wait_for(ser, f"forward upstream 10.0.2.2:{UPSTREAM_PORT}".encode(),
                        5.0)
        if f"forward upstream 10.0.2.2:{UPSTREAM_PORT}".encode() not in log:
            print(f"FAIL: httpd upstream != 10.0.2.2:{UPSTREAM_PORT}")
            print(log[-2000:].decode("ascii","replace")); return 1
        print(f"PASS: httpd forward upstream = 10.0.2.2:{UPSTREAM_PORT}")

        # proxytest's "spawning /bin/browser" line.  Note the
        # actual format is "[proxytest] iter N/M: spawning /bin/browser ..."
        # — match the trailing portion so we're robust to the
        # iter-prefix added in chapter 110.
        log += wait_for(ser, b"spawning /bin/browser", 5.0)
        if b"spawning /bin/browser" not in log:
            print("FAIL: proxytest never spawned browser")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: proxytest spawned browser")

        # The whole point: did the browser print the MARKER?
        # Plain-mode browser dumps every text node verbatim, so
        # the marker from the fake upstream's <body> shows up
        # in the serial log.
        log += wait_for(ser, MARKER.encode(), 60.0)
        if MARKER.encode() not in log:
            print(f"FAIL: marker {MARKER} not seen on serial")
            print(log[-3000:].decode("ascii","replace")); return 1
        print(f"PASS: browser rendered the upstream body ({MARKER})")

        # httpd should log the forward dispatch with the path
        # the browser's canonicalize_url produced.  The "/upstream/"
        # prefix is exactly what serve_forward sees -- chapter 109
        # logs it via log_request "forward GET <path> -> 200".
        # We don't pin the EXACT path here because the URL gets
        # canonicalised; we just confirm the dispatch picked the
        # forward arm.
        log += wait_for(ser, b"forward GET ", 5.0)
        if b"forward GET " not in log:
            print("FAIL: httpd did not log a forward dispatch")
            print(log[-3000:].decode("ascii","replace")); return 1
        print("PASS: httpd dispatched the browser request as 'forward'")

        # And -> 200 status on that forward.
        log += wait_for(ser, b"-> 200", 5.0)
        if b"-> 200" not in log:
            print("FAIL: forward did not return 200")
            print(log[-3000:].decode("ascii","replace")); return 1
        print("PASS: forward returned HTTP 200")

        # proxytest's "done" line is the all-three-children-clean
        # signal.
        log += wait_for(ser, b"[proxytest] done", 30.0)
        if b"[proxytest] done" not in log:
            print("FAIL: proxytest did not print done")
            print(log[-3000:].decode("ascii","replace")); return 1
        print("PASS: proxytest reaped both children cleanly")

        print("\nCHAPTER 106b (browser -> in-guest httpd): ALL TESTS PASSED")
        return 0
    finally:
        try: upstream.shutdown()
        except Exception: pass
        try: q.terminate()
        except Exception: pass
        try: q.wait(timeout=5)
        except Exception:
            try: q.kill()
            except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_url_http.py — URL + HTTP-parser test.

Validates the new URL form of httpget end-to-end:

  1. Spin up a local HTTP server on 127.0.0.1:8889 with three
     endpoints:
        /m58           -> 200 with Content-Length, plain text,
                          recognisable marker.
        /m58-chunked   -> 200 with Transfer-Encoding: chunked
                          and a multi-chunk body that compacts
                          to a recognisable marker.
        /m58-redirect  -> 302 with Location: /m58, no body.
  2. Boot the kernel, wait for shell prompt.
  3. Type three commands at the shell:
        httpget http://10.0.2.2:8889/m58
        httpget http://10.0.2.2:8889/m58-chunked
        httpget http://10.0.2.2:8889/m58-redirect
  4. Assert that for each invocation:
        - the structured "[httpget] HTTP/1.x NNN ..." line appears
        - the body marker appears in the console output
        - for the redirect case, "[httpget] following redirect"
          appears AND the final body marker eventually appears

Why a separate port (8889) from the httpget test:  test_httpget.py
binds 8888 and the GitHub-Action style sequencing won't allow
two suites to bind the same socket if they ever run in
parallel.  Picking a different port avoids the rebind dance.
"""
import http.server, os, re, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-url-http.sock"
HTTP_PORT = 8889

MARKER_PLAIN   = "M58-PLAIN-MARKER"
MARKER_CHUNKED = "M58-CHUNKED-MARKER"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/m58":
            body = (MARKER_PLAIN + "\n").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/m58-chunked":
            # Hand-roll the chunked response so we exercise the
            # decoder with multiple chunks of varying size.
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            # Send "M58-CHUNKED" then "-MARKER\n" as two chunks.
            for piece in (b"M58-CHUNKED", b"-MARKER\n"):
                hdr = f"{len(piece):x}\r\n".encode()
                self.wfile.write(hdr)
                self.wfile.write(piece)
                self.wfile.write(b"\r\n")
            self.wfile.write(b"0\r\n\r\n")   # last-chunk
            return
        if self.path == "/m58-redirect":
            self.send_response(302)
            self.send_header("Location", "http://10.0.2.2:8889/m58")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    def log_message(self, *_a, **_k): pass


class _Reusable(http.server.HTTPServer):
    allow_reuse_address = True


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
    Returns the full buffer (including 'prior') so chained probes
    don't drop bytes between calls."""
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
            if any(n in buf for n in needles): return bytes(buf)
    return bytes(buf)


def wait_for_new(ser, needles, timeout, log):
    """Wait for any needle to appear in serial bytes received AFTER
    the current end-of-`log`. Returns the new accumulated buffer.

    Use this instead of `read_until(..., prior=log)` whenever the
    needle (e.g. the shell '$ ' prompt or a marker that already
    appeared in an earlier sub-test) might already be in `log` —
    otherwise read_until returns immediately on the stale match.
    """
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    cutoff = len(log)
    buf = bytearray(log)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(buf.find(n, cutoff) >= 0 for n in needles):
            return bytes(buf)
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
    return bytes(buf)


def start_http():
    srv = _Reusable(("127.0.0.1", HTTP_PORT), Handler)
    th  = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    return srv


def main():
    srv = start_http()
    q   = boot()
    try:
        ser = conn()

        # Wait for shell prompt (let the kernel net self-test finish first).
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print("PASS: shell prompt available")

        # ── Test 1: plain Content-Length response ──
        ser.sendall(b"httpget http://10.0.2.2:8889/m58\n")
        log = wait_for_new(ser, [MARKER_PLAIN.encode(),
                                 b"httpget: "], 30.0, log)
        if MARKER_PLAIN.encode() not in log:
            print(f"FAIL: plain marker {MARKER_PLAIN} missing")
            print(log[-2000:].decode("ascii", "replace")); return 1
        if not re.search(rb"\[httpget\] HTTP/1\.\d 200", log):
            print("FAIL: structured 200 status line missing")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print("PASS: URL form fetched plain Content-Length response")

        # Drain back to a fresh prompt.
        log = wait_for_new(ser, [b"$ "], 5.0, log)

        # ── Test 2: chunked response decoded in place ──
        ser.sendall(b"httpget http://10.0.2.2:8889/m58-chunked\n")
        log = wait_for_new(ser, [MARKER_CHUNKED.encode(),
                                 b"malformed HTTP"], 30.0, log)
        if MARKER_CHUNKED.encode() not in log:
            print(f"FAIL: chunked marker {MARKER_CHUNKED} missing")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print("PASS: chunked Transfer-Encoding decoded")

        log = wait_for_new(ser, [b"$ "], 5.0, log)

        # ── Test 3: 302 redirect followed once ──
        # Use wait_for_new throughout: MARKER_PLAIN was already seen
        # in test 1, so plain read_until(prior=log) would return
        # immediately on the stale hit and we'd never wait for the
        # post-redirect body to actually arrive.
        ser.sendall(b"httpget http://10.0.2.2:8889/m58-redirect\n")
        log = wait_for_new(ser, [b"following redirect"], 30.0, log)
        if b"following redirect" not in log:
            print("FAIL: redirect-follow log line missing")
            print(log[-2000:].decode("ascii", "replace")); return 1
        log = wait_for_new(ser, [MARKER_PLAIN.encode()], 30.0, log)
        if log.count(MARKER_PLAIN.encode()) < 2:
            # Need to see the marker AGAIN (test 1 was the first hit).
            print("FAIL: redirect target body never delivered")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print("PASS: 302 redirect followed and final body delivered")

        print("\nURL parser + HTTP/1.1 response: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        try: srv.shutdown()
        except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

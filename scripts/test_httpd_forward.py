#!/usr/bin/env python3
"""scripts/test_httpd_forward.py -- chapter 106a / M96 forwarding-proxy test.

End-to-end exercise of httpd's chapter-106a "be a dumb pipe"
forwarding mode.  Boots the kernel with TWO network paths in
play:

  1. SLIRP hostfwd     host:18081  ->  guest:8081
        Lets the test (running on the host) dial INTO the
        guest's httpd, the same way test_httpd.py does for
        the chapter-105 local-file path.

  2. SLIRP outbound    guest:10.0.2.2:<UPSTREAM_PORT>
                                  ->  host's loopback python server
        Lets the guest's httpd dial OUT to a tiny Python
        `http.server` we run on the host, which plays the role
        of `scripts/https_proxy.py` for this test.  Keeping it
        hermetic means we don't depend on `scripts/https_proxy.py`
        being launched separately, and we never touch the real
        internet.

The asserted behaviour:

  A. The chapter-105 LOCAL path still works.  GET /mnt/hello.txt
     returns the OSFS file bytes byte-for-byte.  Regression
     check: prefix dispatch didn't break.

  B. The chapter-106a FORWARD path works.  GET /upstream/<random>
     returns the canned body the Python server produced for
     that path.  Proves:
        - the upstream socket_connect() succeeded from inside an
          inbound handler (chapter 106 loopback prerequisite),
        - the client's request bytes were replayed verbatim
          (the Python server logs the path it received and we
          confirm it matches the path we asked for),
        - the response was spliced back without reframing.

  C. httpd logs the per-request "forward" line so future
     greppers (and the human reader of the chapter) can tell
     local vs forward at a glance.

The chapter-103 boot self-test runs on port 8088 and times out
gracefully (~30s) before init -- we bind 8081, so we're not in
its way, but boot still takes ~30s longer than headless tests.
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
SOCK = "/tmp/osdev-serial-httpd-forward.sock"

# Pick non-default ports so this test doesn't fight test_httpd.py
# during a parallel sweep (sweep.sh is serial today, but
# defensive port choices cost nothing).
HOST_PORT       = 18081      # host-side, dialed by this script
GUEST_PORT      = 8081       # guest-side, where httpd binds
UPSTREAM_PORT   = 18082      # host-side, where our fake proxy listens
# The guest reaches the host at SLIRP's gateway-of-the-guest IP.
UPSTREAM_FOR_GUEST = f"10.0.2.2:{UPSTREAM_PORT}"

# Recognizable marker the fake upstream returns.  Distinct from
# the test_httpget.py marker so a wedged sweep is easier to
# diagnose.
MARKER = "M96-FORWARD-OK-PAYLOAD"

# Path the test asks for through the forward dispatch.  Anything
# that isn't /mnt/, /data/, or /proc/ will route to serve_forward
# in chapter-106a httpd.
FORWARD_PATH = "/upstream/news.ycombinator.com/item?id=1"

# Local path used for the regression check.  Same target
# test_httpd.py uses, which guarantees the asset exists.
LOCAL_PATH    = "/mnt/hello.txt"
LOCAL_ASSET   = os.path.join(ROOT, "assets/osfs/hello.txt")


# ----------------------------------------------------------------
# Host-side fake "https_proxy" upstream.  Echoes back a body that
# encodes the exact path the request asked for, so we can prove
# the guest replayed the request verbatim.
# ----------------------------------------------------------------

class FakeUpstream(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = (f"{MARKER}|path={self.path}\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        # We rely on HTTP/1.0 + close framing on the way back to
        # the guest.  http.server defaults to HTTP/1.0 which
        # closes the conn after each response, which is exactly
        # what serve_forward's splice loop expects.
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args, **kwargs):
        # Silence the per-request stderr log lines that
        # BaseHTTPRequestHandler emits by default; keeps the
        # sweep output clean.
        pass


class _Reusable(http.server.HTTPServer):
    allow_reuse_address = True


def start_upstream():
    srv = _Reusable(("127.0.0.1", UPSTREAM_PORT), FakeUpstream)
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    return srv


# ----------------------------------------------------------------
# QEMU + serial plumbing.  Same shape as test_httpd.py /
# test_echod.py.
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
        # One netdev does double duty: outbound NAT to 10.0.2.2
        # (where our upstream listens) AND the hostfwd that lets
        # this script dial in.
        "-netdev", f"user,id=n0,hostfwd=tcp::{HOST_PORT}-:{GUEST_PORT}",
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


def wait_for(s, needle, timeout):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


# ----------------------------------------------------------------
# Host-side HTTP client.  Issues one GET against the guest's
# httpd via the SLIRP hostfwd and drains the whole response.
# ----------------------------------------------------------------

def dial_guest():
    deadline = time.time() + 15.0
    last_err = None
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(8.0)
            s.connect(("127.0.0.1", HOST_PORT))
            return s
        except OSError as e:
            last_err = e
            time.sleep(0.25)
    raise RuntimeError(f"could not connect to host:{HOST_PORT}: {last_err}")


def http_get(target_path):
    s = dial_guest()
    req = (f"GET {target_path} HTTP/1.0\r\n"
           f"Host: osdev\r\n"
           f"User-Agent: test_httpd_forward.py\r\n"
           f"\r\n").encode()
    s.sendall(req)
    try: s.shutdown(socket.SHUT_WR)
    except OSError: pass

    s.settimeout(30.0)
    raw = b""
    while True:
        try:
            chunk = s.recv(4096)
        except (OSError, socket.timeout):
            break
        if not chunk: break
        raw += chunk
    s.close()

    if b"\r\n\r\n" not in raw:
        return None, {}, raw
    head, body = raw.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    parts = lines[0].split(b" ", 2)
    if len(parts) < 2 or not parts[1].isdigit():
        return None, {}, body
    code = int(parts[1])
    headers = {}
    for h in lines[1:]:
        if b":" not in h: continue
        k, v = h.split(b":", 1)
        headers[k.strip().lower()] = v.strip()
    return code, headers, body


# ----------------------------------------------------------------
# Test body.  Two GETs against ONE httpd run: the forward path
# (run first, while we still have to-be-served upstream), then
# the local-path regression.  We use --once would only let us do
# ONE request per httpd run, so we run httpd twice -- first with
# --once for the forward GET, then again with --once for the
# local GET.  Cleaner than smuggling state through "&".
# ----------------------------------------------------------------

def run_httpd_once_with_upstream(ser, log):
    """Start httpd with HTTPD_UPSTREAM pointing at our fake.
    Returns the log buffer after we've seen the listening line."""
    # `export` is a shell builtin (chapter-33 env vars).  We seed
    # the env BEFORE spawning httpd so load_upstream_from_env()
    # picks it up.
    ser.sendall(
        f"export HTTPD_UPSTREAM={UPSTREAM_FOR_GUEST}\n".encode())
    # Drain the prompt that re-appears after export returns.
    log += wait_for(ser, b"$ ", 5.0)

    ser.sendall(f"httpd {GUEST_PORT} --once\n".encode())
    log += wait_for(ser, f"httpd: listening on port {GUEST_PORT}".encode(),
                    15.0)
    return log


def main():
    if not os.path.exists(LOCAL_ASSET):
        print(f"FAIL: test prerequisite missing: {LOCAL_ASSET}")
        return 1
    with open(LOCAL_ASSET, "rb") as f:
        expected_local_body = f.read()

    upstream = start_upstream()
    q = boot()
    try:
        ser = conn()

        # Wait for the shell.  Long timeout because chapter-103's
        # boot self-test busy-polls accept() on port 8088 for ~30s.
        log = wait_for(ser, b"$ ", 120.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: shell prompt available")

        # ------------------------------------------------------
        # Phase A: chapter-106a FORWARD path.
        # ------------------------------------------------------
        log = run_httpd_once_with_upstream(ser, log)
        if f"httpd: listening on port {GUEST_PORT}".encode() not in log:
            print("FAIL: httpd never logged its listen line (forward run)")
            print(log[-2000:].decode("ascii","replace")); return 1
        print(f"PASS: httpd listening on guest port {GUEST_PORT} (forward run)")

        # Sanity: confirm httpd picked up our upstream override.
        # The "httpd: forward upstream a.b.c.d:port" line is printed
        # right after the listening line.
        expected_upstream_line = f"forward upstream 10.0.2.2:{UPSTREAM_PORT}".encode()
        log += wait_for(ser, expected_upstream_line, 5.0)
        if expected_upstream_line not in log:
            print(f"FAIL: httpd never logged the upstream override "
                  f"(expected {expected_upstream_line!r})")
            print(log[-2000:].decode("ascii","replace")); return 1
        print(f"PASS: httpd picked up HTTPD_UPSTREAM={UPSTREAM_FOR_GUEST}")

        try:
            code, headers, body = http_get(FORWARD_PATH)
        except Exception as e:
            print(f"FAIL: forward GET raised: {e}")
            print(log[-2000:].decode("ascii","replace")); return 1

        if code != 200:
            print(f"FAIL: forward GET {FORWARD_PATH} -> status {code}")
            print(f"  body[:200]={body[:200]!r}"); return 1
        print(f"PASS: forward GET {FORWARD_PATH} returned 200 OK")

        expected_body = (f"{MARKER}|path={FORWARD_PATH}\n").encode()
        if body != expected_body:
            print("FAIL: forward body mismatch (proves splice or upstream broken)")
            print(f"  expected={expected_body!r}")
            print(f"  got     ={body!r}")
            return 1
        print(f"PASS: forward body matches fake upstream byte-for-byte "
              f"({len(body)} bytes)")

        # httpd's per-request log line: must say "forward".
        log += wait_for(ser, f"forward GET {FORWARD_PATH} -> 200".encode(),
                        10.0)
        if f"forward GET {FORWARD_PATH} -> 200".encode() not in log:
            print("FAIL: httpd didn't log the forward request line")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: httpd logged 'forward' dispatch for non-local path")

        log += wait_for(ser, b"httpd: done", 10.0)
        if b"httpd: done" not in log:
            print("FAIL: httpd never exited after --once (forward run)")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: httpd exited cleanly after forward request")

        # ------------------------------------------------------
        # Phase B: chapter-105 LOCAL path regression.
        # ------------------------------------------------------
        # Make sure we're back at the prompt before issuing
        # the next command.
        log += wait_for(ser, b"$ ", 10.0)

        ser.sendall(f"httpd {GUEST_PORT} --once\n".encode())
        log += wait_for(ser, f"httpd: listening on port {GUEST_PORT}".encode(),
                        15.0)
        if f"httpd: listening on port {GUEST_PORT}".encode() not in log:
            print("FAIL: httpd never logged its listen line (local run)")
            print(log[-2000:].decode("ascii","replace")); return 1
        print(f"PASS: httpd listening on guest port {GUEST_PORT} (local run)")

        try:
            code, headers, body = http_get(LOCAL_PATH)
        except Exception as e:
            print(f"FAIL: local GET raised: {e}")
            print(log[-2000:].decode("ascii","replace")); return 1

        if code != 200:
            print(f"FAIL: local GET {LOCAL_PATH} -> status {code}")
            return 1
        print(f"PASS: local GET {LOCAL_PATH} returned 200 OK")

        if body != expected_local_body:
            print("FAIL: local body changed (chapter-106a prefix dispatch "
                  "broke chapter-105 serving)")
            print(f"  expected len={len(expected_local_body)}, got len={len(body)}")
            return 1
        print(f"PASS: local body matches /mnt/hello.txt byte-for-byte "
              f"({len(body)} bytes)")

        log += wait_for(ser, f"local GET {LOCAL_PATH} -> 200".encode(), 10.0)
        if f"local GET {LOCAL_PATH} -> 200".encode() not in log:
            print("FAIL: httpd didn't log the local request line")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: httpd logged 'local' dispatch for /mnt/ path")

        log += wait_for(ser, b"httpd: done", 10.0)
        if b"httpd: done" not in log:
            print("FAIL: httpd never exited after --once (local run)")
            return 1
        print("PASS: httpd exited cleanly after local request")

        print("\nCHAPTER 106a (httpd TLS bridge): ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        try: upstream.shutdown()
        except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

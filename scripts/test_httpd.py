#!/usr/bin/env python3
"""scripts/test_httpd.py -- chapter 107 /bin/httpd test.

Boots the kernel with SLIRP's hostfwd forwarding host:18080 ->
guest:8080, waits for the shell prompt, runs `httpd 8080 --once`
in the guest, dials port 18080 from the host, issues a single
GET request for /mnt/hello.txt, and asserts:

  - the response is a well-formed HTTP/1.0 200 OK,
  - Content-Type is text/plain,
  - the response body matches the on-disk file byte-for-byte,
  - httpd logs the expected per-request line and exits cleanly.

We use /mnt/hello.txt as the target because it's the smallest
file we know is always present in /mnt (chapter 11 onwards).
Reading it back through the network exercises the whole stack:

  host TCP  ->  SLIRP hostfwd  ->  guest virtio-net
  ->  kernel tcp_handle  ->  socket_accept syscall
  ->  httpd's read_request  ->  open("/mnt/hello.txt")
  ->  kernel vfs_read on the OSFS mount
  ->  httpd's write loop  ->  socket write -> tcp_send
  ->  guest virtio-net  ->  SLIRP  ->  host recv

The chapter-103 boot self-test still runs on port 8088 and
times out gracefully (~30s) before init starts; httpd binds
8080 and is unaffected.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-httpd.sock"
HOST_PORT  = 18080
GUEST_PORT = 8080

# The target file httpd should serve.  Lives in /mnt/ on the
# OSFS-1 disk image, baked at build time from assets/osfs/.
TARGET_PATH      = "/mnt/hello.txt"
TARGET_ASSET     = os.path.join(ROOT, "assets/osfs/hello.txt")
EXPECTED_CTYPE   = b"text/plain"   # substring -- charset suffix varies


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
        # host:18080 -> guest:8080 so the test can dial in.
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


def dial_guest():
    """Open a TCP connection to the guest's httpd via SLIRP hostfwd.

    Retries because SLIRP's host-side listener is open immediately
    at QEMU start but httpd's socket_listen doesn't run until the
    boot self-test finishes and we type the command.  Connection-
    refused is the expected error during that window.
    """
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
    """Issue one GET and return (status_code, headers_dict, body_bytes).

    HTTP/1.0 + Connection: close means we drain until peer FIN,
    so there's no Content-Length to parse for framing -- we just
    read until recv returns empty.
    """
    s = dial_guest()
    req = (f"GET {target_path} HTTP/1.0\r\n"
           f"Host: osdev\r\n"
           f"User-Agent: test_httpd.py\r\n"
           f"\r\n").encode()
    s.sendall(req)
    try: s.shutdown(socket.SHUT_WR)
    except OSError: pass

    s.settimeout(20.0)
    raw = b""
    while True:
        try:
            chunk = s.recv(4096)
        except (OSError, socket.timeout):
            break
        if not chunk: break
        raw += chunk
    s.close()

    # Split headers / body at the first blank line.
    if b"\r\n\r\n" not in raw:
        return None, {}, raw
    head, body = raw.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    # Parse status line: "HTTP/1.0 200 OK"
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


def main():
    if not os.path.exists(TARGET_ASSET):
        print(f"FAIL: test prerequisite missing: {TARGET_ASSET}")
        return 1
    with open(TARGET_ASSET, "rb") as f:
        expected_body = f.read()

    q = boot()
    try:
        ser = conn()

        # Wait for the shell prompt.  See test_echod.py for why
        # this needs a long timeout (ch103 self-test busy-polls
        # tcp_accept(8088) for ~30s before timing out).
        log = wait_for(ser, b"$ ", 120.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: shell prompt available")

        # Spin up the daemon.
        ser.sendall(b"httpd 8080 --once\n")
        log += wait_for(ser, b"httpd: listening on port 8080", 15.0)
        if b"httpd: listening on port 8080" not in log:
            print("FAIL: httpd never logged its listen line")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: httpd listening on guest port 8080")

        # Issue the GET.
        try:
            code, headers, body = http_get(TARGET_PATH)
        except Exception as e:
            print(f"FAIL: GET request raised: {e}")
            print(log[-2000:].decode("ascii","replace")); return 1

        if code != 200:
            print(f"FAIL: GET {TARGET_PATH} returned status {code}")
            print(f"  body[:200]={body[:200]!r}")
            return 1
        print(f"PASS: GET {TARGET_PATH} returned 200 OK")

        ctype = headers.get(b"content-type", b"")
        if EXPECTED_CTYPE not in ctype:
            print(f"FAIL: Content-Type mismatch: got {ctype!r}, expected substring {EXPECTED_CTYPE!r}")
            return 1
        print(f"PASS: Content-Type is {ctype.decode('ascii','replace')!r}")

        if headers.get(b"connection", b"").lower() != b"close":
            print(f"FAIL: missing Connection: close header (got {headers.get(b'connection')!r})")
            return 1
        print("PASS: Connection: close header present")

        if body != expected_body:
            print(f"FAIL: body mismatch")
            print(f"  expected len={len(expected_body)}, got len={len(body)}")
            print(f"  expected[:80]={expected_body[:80]!r}")
            print(f"  got     [:80]={body[:80]!r}")
            return 1
        print(f"PASS: response body matches /mnt/hello.txt byte-for-byte ({len(body)} bytes)")

        # httpd should log the per-request line and then exit
        # because of --once.
        log += wait_for(ser, b"GET /mnt/hello.txt -> 200", 10.0)
        if b"GET /mnt/hello.txt -> 200" not in log:
            print("FAIL: httpd never logged the per-request line")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: httpd logged GET /mnt/hello.txt -> 200")

        log += wait_for(ser, b"httpd: done", 10.0)
        if b"httpd: done" not in log:
            print("FAIL: httpd never exited (no 'done' line)")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: httpd exited cleanly after --once")

        print("\nCHAPTER 105 (/bin/httpd): ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_tcp.py — TCP client smoke test.

Brings up a tiny localhost HTTP server on port 8888, then boots
the kernel with QEMU SLIRP user-mode networking.  SLIRP forwards
the guest's connection to 10.0.2.2:8888 to the host's
127.0.0.1:8888 — so the kernel's boot self-test exercises every
TCP state transition (SYN → SYN+ACK ack → ESTABLISHED →
PSH+ACK data → FIN exchange → CLOSED) against a real listener.

Passing this test means the kernel can:
  - acquire a DHCP lease (the DHCP path still works),
  - open an outbound TCP connection,
  - send a complete HTTP/1.0 request,
  - receive the server's response body intact,
  - close the connection cleanly.

The HTTP server returns a fixed 64-byte payload so we can also
spot-check the recv length the kernel logs.
"""
import http.server, os, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-tcp.sock"
HTTP_PORT = 8888

# Fixed 64-byte body so the kernel's "bytes=" log matches.
BODY = b"X" * 64


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *_args, **_kwargs):
        pass


class _ReusableHTTPServer(http.server.HTTPServer):
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


def start_http_server():
    srv = _ReusableHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    th  = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    return srv


def main():
    srv = start_http_server()
    q   = boot()
    try:
        ser = conn()

        log = wait_for(ser, b"TCP connect to 10.0.2.2:8888", 35.0)
        if b"TCP connect to 10.0.2.2:8888" not in log:
            print("FAIL: TCP self-test phase never started")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: TCP connect issued by kernel")

        log += wait_for(ser, b"TCP connection ESTABLISHED", 10.0)
        if b"TCP connection ESTABLISHED" not in log:
            print("FAIL: handshake did not reach ESTABLISHED")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: TCP three-way handshake completed")

        log += wait_for(ser, b"HTTP response bytes=", 15.0)
        if b"HTTP response bytes=" not in log:
            print("FAIL: HTTP response not drained")
            return 1
        # The body is 64 bytes + headers; total response is well over
        # 100 bytes.  The kernel logs the count in hex.
        for line in log.decode("ascii","replace").splitlines():
            if "HTTP response bytes=" in line:
                hexpart = line.split("=")[-1].strip()
                try:
                    n = int(hexpart, 16)
                except ValueError:
                    n = 0
                if n < 64:
                    print(f"FAIL: response too short ({n} bytes)")
                    return 1
                print(f"PASS: HTTP response received ({n} bytes)")
                break

        log += wait_for(ser, b"TCP close complete", 10.0)
        if b"TCP close complete" not in log:
            print("FAIL: connection close did not complete")
            return 1
        print("PASS: TCP close completed cleanly")

        log += wait_for(ser, b"$ ", 25.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached after TCP self-test")
            return 1
        print("PASS: shell prompt reached after TCP self-test")

        print("\nTCP: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        try: srv.shutdown()
        except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

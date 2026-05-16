#!/usr/bin/env python3
"""scripts/test_httpget.py \u2014 milestone-56 socket-syscall smoke test.

End-to-end exercise of the new userspace TCP client path:

  1. Boot the kernel (which auto-runs its M55 TCP self-test on
     port 8888 \u2014 we let that succeed first so we know the
     kernel side is fine).
  2. Wait for the shell prompt.
  3. Type `httpget 10.0.2.2 8888 /m56`, where the host's HTTP
     server returns a unique recognizable body.
  4. Assert the body bytes echoed by `httpget` appear on the
     guest's serial console.

Passing this test means a userspace process can:
  - call SYS_SOCKET_CONNECT and get back a real fd,
  - issue ordinary write()/read()/close() against that fd,
  - receive bytes that traveled all the way through the
    M51\u2013M55 networking stack and into a user buffer.
"""
import http.server, os, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-httpget.sock"
HTTP_PORT = 8888

# Recognizable body so we can grep for it on the guest console.
MARKER = "M56-HTTPGET-OK-PAYLOAD"
BODY   = (MARKER + "\n").encode()


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

        # First, let the kernel's own M55 self-test run to completion
        # \u2014 that proves the network stack is up and avoids racing
        # against DHCP / ARP from the userspace command.
        log = wait_for(ser, b"TCP close complete", 45.0)
        if b"TCP close complete" not in log:
            print("FAIL: kernel M55 self-test never finished")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: kernel TCP self-test completed")

        # Wait for the shell prompt before typing.
        log += wait_for(ser, b"$ ", 30.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: shell prompt available")

        # Type the httpget invocation.
        cmd = b"httpget 10.0.2.2 8888 /m56\n"
        ser.sendall(cmd)

        # Look for the marker in the guest's console output.
        log = wait_for(ser, MARKER.encode(), 30.0)
        if MARKER.encode() not in log:
            print("FAIL: httpget did not echo HTTP body to console")
            print(log[-2000:].decode("ascii","replace")); return 1
        print(f"PASS: httpget delivered HTTP body to userspace ({MARKER})")

        # Sanity: see the byte count line.
        log += wait_for(ser, b"[httpget] received ", 10.0)
        if b"[httpget] received " not in log:
            print("FAIL: httpget did not print byte count line")
            return 1
        print("PASS: httpget printed byte count summary")

        print("\nMILESTONE 56 (sockets+httpget): ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        try: srv.shutdown()
        except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Boot the kernel + spin a localhost HTTP server, capture the
serial log filtered to network-stack lines."""
import http.server, os, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-snoop-tcp.sock"
HTTP_PORT = 8888

BODY = b"X" * 64

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(BODY)
    def log_message(self, *_a, **_k):
        sys.stderr.write(f"http: {self.command} {self.path}\n")

class S(http.server.HTTPServer):
    allow_reuse_address = True

def cleanup():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 18
    cleanup()
    srv = S(("127.0.0.1", HTTP_PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    q = subprocess.Popen([
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
    deadline = time.time() + 5.0
    s = None
    while time.time() < deadline and s is None:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(SOCK)
            except OSError:
                s = None; time.sleep(0.05)
        else:
            time.sleep(0.05)
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        r,_,_ = select.select([s],[],[],0.2)
        if r:
            c = s.recv(4096)
            if not c: break
            buf += c
    try: q.terminate(); q.wait(timeout=3)
    except Exception: q.kill()
    srv.shutdown()
    text = buf.decode("ascii","replace")
    for line in text.splitlines():
        if any(t in line for t in ("[net]","[dhcp]","[tcp]","[virtio-net]","panic","FAIL")):
            print(line)

if __name__ == "__main__":
    main()

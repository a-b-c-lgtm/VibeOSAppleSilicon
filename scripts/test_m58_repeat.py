#!/usr/bin/env python3
"""scripts/test_m58_repeat.py — reproduce the post-httpget panic.

Boot the kernel, run httpget against /m58 three times back to back,
and capture every diagnostic line.  We want to see:

  * `[heap-diag] heap_start=... heap_end=...`  (printed once at boot
    if main.c was wired to call kheap_diag — currently it isn't, so
    this script also greps for the existing 'initialising kernel
    heap (... bytes @ ...)' line)
  * Every `[diag-stk K|U]` line so we can spot any thread whose
    kernel stack lands outside the heap range
  * The exception dump that follows the panic

Save the full transcript to /tmp/m58-repeat.log so we can reference
it from the book / memory notes.
"""
import http.server, os, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-m58repeat.sock"
HTTP_PORT = 8889
LOGFILE   = "/tmp/m58-repeat.log"

BODY = b"M58-PLAIN-MARKER\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/m58":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(BODY)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(BODY)
        else:
            self.send_error(404)
    def log_message(self, *_a, **_kw):
        pass


class _Reuse(http.server.HTTPServer):
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


def drain(s, dur):
    end = time.time() + dur
    out = b""
    while time.time() < end:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out


def wait_for(s, needle, timeout):
    if isinstance(needle, str): needle = needle.encode()
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += drain(s, 0.3)
        if needle in buf: return buf
    return buf


def main():
    srv = _Reuse(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    q = boot()
    full = b""
    try:
        ser = conn()
        full += wait_for(ser, b"TCP close complete", 60.0)
        full += wait_for(ser, b"$ ", 30.0)
        if b"$ " not in full:
            print("FAIL: never reached shell prompt")
        else:
            for i in range(3):
                print(f"[host] sending httpget #{i+1} ...")
                ser.sendall(b"httpget http://10.0.2.2:8889/m58\n")
                chunk = wait_for(ser, b"[sys_exit] thread '/bin/httpget'",
                                 20.0)
                full += chunk
                if b"PANIC" in chunk or b"unhandled" in chunk:
                    print(f"PANIC during httpget #{i+1}")
                    break
                # small drain to absorb prompt re-print
                full += drain(ser, 0.5)

        # If everything completed cleanly, give the kernel a moment
        # to optionally panic on its own (the original crash report
        # showed the panic AFTER httpget finished).
        full += drain(ser, 3.0)
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        try: srv.shutdown()
        except Exception: pass

    with open(LOGFILE, "wb") as f:
        f.write(full)
    print(f"\n--- captured {len(full)} bytes -> {LOGFILE} ---")

    # Print just the diagnostic lines and any panic.
    text = full.decode("ascii","replace")
    for line in text.splitlines():
        if ("[diag-stk" in line or "[heap-diag" in line
                or "initialising kernel heap" in line
                or "PANIC" in line or "unhandled" in line
                or "ESR_EL1" in line or "FAR_EL1" in line
                or "ELR_EL1" in line or "vector" in line
                or "[sys_exit] thread '/bin/httpget'" in line):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())

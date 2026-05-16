#!/usr/bin/env python3
"""Run `browser <cmd>` against the QEMU image and print the captured serial."""
import os, select, socket, subprocess, sys, time

if len(sys.argv) < 2:
    print("usage: run_browser_cmd.py <command-line>")
    sys.exit(1)
CMD = " ".join(sys.argv[1:])

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-runcmd.sock"
try: os.unlink(SOCK)
except FileNotFoundError: pass

q = subprocess.Popen([
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
    "-m", "8G", "-display", "none",
    "-serial", f"unix:{SOCK},server,nowait",
    "-global", "virtio-mmio.force-legacy=off",
    "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-drive",  f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
    "-device", "virtio-blk-device,drive=hd1",
    "-netdev", "user,id=n0",
    "-device", "virtio-net-device,netdev=n0",
    "-kernel", f"{ROOT}/build/kernel.elf",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

deadline = time.time() + 15
s = None
while time.time() < deadline:
    if os.path.exists(SOCK):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(SOCK)
            break
        except OSError:
            s = None
    time.sleep(0.1)

if s is None:
    print("ERR: could not connect to qemu serial socket")
    q.terminate(); q.wait(timeout=3)
    sys.exit(2)

buf = b""
def drain_until(needle, secs):
    global buf
    t = time.time() + secs
    while time.time() < t:
        r,_,_ = select.select([s],[],[],0.25)
        if not r: continue
        try: c = s.recv(16384)
        except OSError: break
        if not c: break
        buf += c
        if needle in buf: return True
    return False

drain_until(b"/$ ", 30)
s.sendall(CMD.encode() + b"\n")
end = time.time() + 60
last = time.time()
while time.time() < end and time.time() - last < 6:
    r,_,_ = select.select([s],[],[],0.5)
    if not r: continue
    try: c = s.recv(16384)
    except OSError: break
    if not c: break
    buf += c
    last = time.time()

q.terminate()
try: q.wait(timeout=3)
except Exception: q.kill()
try: os.unlink(SOCK)
except Exception: pass

sys.stdout.buffer.write(buf)

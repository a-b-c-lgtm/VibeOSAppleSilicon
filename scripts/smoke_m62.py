#!/usr/bin/env python3
"""Smoke-run /bin/layout once and dump the output."""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-m62-smoke.sock"
LOG  = "/tmp/m62-smoke.log"

try: os.unlink(SOCK)
except FileNotFoundError: pass

q = subprocess.Popen([
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
    "-m", "8G", "-display", "none",
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

deadline = time.time() + 5
s = None
while time.time() < deadline:
    if os.path.exists(SOCK):
        try: s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(SOCK); break
        except OSError: pass
    time.sleep(0.05)

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
s.sendall(b"layout /mnt/test_layout.html 800\n")
# Read for up to 60 seconds total or until 5 seconds of silence.
end = time.time() + 60
last_data = time.time()
while time.time() < end and time.time() - last_data < 5:
    r,_,_ = select.select([s],[],[],0.5)
    if not r: continue
    try: c = s.recv(16384)
    except OSError: break
    if not c: break
    buf += c
    last_data = time.time()

q.terminate()
try: q.wait(timeout=3)
except Exception: q.kill()
try: os.unlink(SOCK)
except Exception: pass

with open(LOG, "wb") as f: f.write(buf)
sys.stdout.buffer.write(buf)

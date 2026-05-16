#!/usr/bin/env python3
"""Reproduce test_fork_exec timing: send forktest IMMEDIATELY when $ appears."""
import os, select, socket, subprocess, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-probe2.sock"
try: os.unlink(SOCK)
except FileNotFoundError: pass

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

deadline = time.time() + 5
s = None
while time.time() < deadline:
    if os.path.exists(SOCK):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK); break
    time.sleep(0.05)

# Read until we see "$ " — same as test_fork_exec
buf = b""
end = time.time() + 90
while time.time() < end:
    r,_,_ = select.select([s],[],[],0.2)
    if r:
        c = s.recv(8192)
        if not c: break
        buf += c
        if b"$ " in buf:
            print(f"=== saw '$ ' after {len(buf)} bytes; sending forktest ===")
            t_send = time.time()
            s.sendall(b"forktest\n")
            break

# Now read for 15s after sending
end = time.time() + 15
while time.time() < end:
    r,_,_ = select.select([s],[],[],0.2)
    if r:
        c = s.recv(8192)
        if not c: break
        buf += c

# Print everything from the moment we saw $
idx = buf.find(b"/$ ")
print(buf[idx:].decode("ascii","replace") if idx >= 0 else "(no /$ )")
print(f"\n=== total bytes: {len(buf)} ===")

q.terminate()
try: q.wait(timeout=3)
except Exception: q.kill()

#!/usr/bin/env python3
"""Manual input probe - boot with net + try sending forktest, see all the output."""
import os, select, socket, subprocess, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-probe.sock"
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

def drain(timeout):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            buf += c
    return buf

# Wait for prompt
print("=== boot ===")
boot = drain(15.0)
sys.stdout.write(boot.decode("ascii", "replace"))
print(f"\n=== boot bytes: {len(boot)} ===")
print(f"=== last 20 bytes: {boot[-20:]!r} ===")

# Send "ls\n" first as a sanity check
print("\n=== send 'ls\\n' ===")
s.sendall(b"ls\n")
ls_out = drain(5.0)
sys.stdout.write(ls_out.decode("ascii", "replace"))
print(f"\n=== ls reply bytes: {len(ls_out)} ===")

# Send forktest
print("\n=== send 'forktest\\n' ===")
s.sendall(b"forktest\n")
ft_out = drain(15.0)
sys.stdout.write(ft_out.decode("ascii", "replace"))
print(f"\n=== forktest reply bytes: {len(ft_out)} ===")

q.terminate()
try: q.wait(timeout=3)
except Exception: q.kill()

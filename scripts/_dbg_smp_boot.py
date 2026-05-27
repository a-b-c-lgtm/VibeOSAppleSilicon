#!/usr/bin/env python3
"""
_dbg_smp_boot.py — chapter 87 smoke test.

Boot the kernel under QEMU `-smp 2` and capture serial output for
20 seconds.  Prints the boot log so we can confirm:

    [smp] bringing up additional cores ...
    [smp] DTB reports 2 cpu(s)
    [smp] PSCI CPU_ON cpu=1 ...
    [smp] CPU 1 ready (mpidr = 0x1)
    [smp] all CPUs online

Kept (per the debug-scripts policy) as a reference snapshot of how
the SMP bring-up looked when chapter 87 first landed.
"""
import os, select, socket, subprocess, sys, time

SOCK = "/tmp/osdev-smp-boot.sock"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

try: os.unlink(SOCK)
except FileNotFoundError: pass

q = subprocess.Popen([
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3",
    "-cpu", "host", "-accel", "hvf",
    "-m", "8G",
    "-smp", "2",
    "-display", "none",
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

deadline = time.time() + 5
s = None
while time.time() < deadline:
    if os.path.exists(SOCK):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK); break
    time.sleep(0.05)
if s is None:
    print("ERROR: serial socket never appeared", file=sys.stderr)
    q.kill(); sys.exit(1)

buf = b""
end = time.time() + 20
while time.time() < end:
    r, _, _ = select.select([s], [], [], 0.5)
    if r:
        c = s.recv(8192)
        if not c: break
        buf += c

print("LEN:", len(buf))
print(buf.decode("ascii", "replace"))
print("\n--- chapter 87 markers ---")
need = [
    b"[smp] bringing up additional cores",
    b"[smp] DTB reports 2 cpu(s)",
    b"[smp] PSCI CPU_ON cpu=1",
    b"[smp] CPU 1 ready",
    b"[smp] all CPUs online",
    b"[smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK",
    b"[smp-ipi] cpu=1 OK round-trip",
    b"[smp-ipi] all OK",
    b"[smp-sched] cpu_1 ran 4 of 4 OK",
]
ok = True
for n in need:
    found = n in buf
    print(("OK  " if found else "MISS"), n.decode())
    ok = ok and found

q.terminate()
try: q.wait(timeout=3)
except Exception: q.kill()

sys.exit(0 if ok else 1)

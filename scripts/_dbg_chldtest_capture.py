#!/usr/bin/env python3
"""scripts/_dbg_chldtest_capture.py — diagnostic capture of full
chldtest output without stopping on PANIC.  Used during the
chapter 74 (COW) bring-up to see the kernel panic that the
regular test_sigchld.py harness was clipping off too early.

Per the user's debug-script policy, kept around for future reference.
"""
import os, socket, subprocess, time, select, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-cow-debug.sock"

try: os.unlink(SOCK)
except FileNotFoundError: pass

q = subprocess.Popen([
    "qemu-system-aarch64", "-M", "virt,gic-version=3",
    "-cpu", "host", "-accel", "hvf", "-m", "8G", "-smp", "2",
    "-display", "none",
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

try:
    deadline = time.time() + 5
    s = None
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK)
                break
            except OSError: pass
        time.sleep(0.05)

    buf = bytearray()
    end = time.time() + 30
    sent = False
    cmd  = sys.argv[1] if len(sys.argv) > 1 else "chldtest"
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.2)
        if r:
            c = s.recv(8192)
            if c: buf.extend(c)
        if not sent and b"$ " in buf:
            sent = True
            s.sendall(cmd.encode() + b"\n")
    sys.stdout.write(buf.decode("ascii", "replace"))
finally:
    q.kill(); q.wait()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

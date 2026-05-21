#!/usr/bin/env python3
"""_dbg_fontd_timing.py — quick check of test_fontd's serial timing.

Boots the same QEMU command test_fontd.py uses, captures serial,
prints timestamps for fontd-ready and the shell prompt. Tells us
whether the test's 15s post-fontd-ready wait_for is failing
because $ never arrives, or arrives too slowly, or arrives but
in a form wait_for can't see.
"""
import os, socket, subprocess, time, select, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SS = "/tmp/osdev-serial-dbg.sock"
QM = "/tmp/osdev-qmp-dbg.sock"

for p in (SS, QM):
    try: os.unlink(p)
    except FileNotFoundError: pass

q = subprocess.Popen([
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
    "-m", "8G", "-smp", "2", "-display", "none",
    "-serial", f"unix:{SS},server,nowait",
    "-qmp", f"unix:{QM},server,nowait",
    "-global", "virtio-mmio.force-legacy=off",
    "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-device", "virtio-gpu-device,xres=1280,yres=800",
    "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device",
    "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
    "-device", "virtio-blk-device,drive=hd1",
    "-kernel", f"{ROOT}/build/kernel.elf",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(1.0)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(SS)

t0 = time.time()
buf = b""
fontd_at = None
prompt_at = None
shell_at  = None

deadline = t0 + 40.0
while time.time() < deadline:
    r, _, _ = select.select([s], [], [], 0.2)
    if r:
        c = s.recv(8192)
        if not c: break
        buf += c
        sys.stdout.write(c.decode("utf-8", "replace"))
        sys.stdout.flush()
        if fontd_at is None and b"[fontd] ready on /srv/font" in buf:
            fontd_at = time.time() - t0
        if shell_at is None and b"tiny shell ready" in buf:
            shell_at = time.time() - t0
        if prompt_at is None and b"$ " in buf:
            prompt_at = time.time() - t0
        if fontd_at and prompt_at:
            break

q.terminate()
print()
print("---")
print(f"boot to fontd-ready:   {fontd_at}")
print(f"boot to tiny-shell-ready: {shell_at}")
print(f"boot to $ prompt:      {prompt_at}")
if fontd_at and prompt_at:
    print(f"fontd-ready -> prompt: {prompt_at - fontd_at:.2f}s")
print(f"bytes captured: {len(buf)}")
print("dollar-space present:", b"$ " in buf)

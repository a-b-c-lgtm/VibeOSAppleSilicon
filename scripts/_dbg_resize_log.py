#!/usr/bin/env python3
"""scripts/_dbg_resize_log.py -- chapter 118 resize bring-up.

Boots, spawns notepad, drags the grip, and dumps the serial
log so we can see whether [wsd] resize / [win_fb] resize /
[wmclient] remap fired.  Kept per the debug-scripts-policy
memory.
"""
import json, os, select, socket, subprocess, time, sys

ROOT = "/Users/seusher/Desktop/osdev"
QMP  = "/tmp/osdev-qmp-rzdbg.sock"
SER  = "/tmp/osdev-serial-rzdbg.sock"

def cleanup():
    for p in (QMP, SER):
        try: os.unlink(p)
        except FileNotFoundError: pass

def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SER},server,nowait",
        "-qmp", f"unix:{QMP},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", "virtio-gpu-device,xres=1280,yres=800",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def conn(p):
    d = time.time() + 8
    while time.time() < d:
        if os.path.exists(p):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(p); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError(p)

def qrl(q):
    b = b""
    while not b.endswith(b"\n"):
        c = q.recv(4096); b += c
    return json.loads(b)

def qsend(q, o):
    q.sendall((json.dumps(o) + "\n").encode())
    while True:
        m = qrl(q)
        if "return" in m or "error" in m: return m

def cur(qmp, x, y):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7fff / 1280)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7fff / 800)}},
    ]}})

def btn(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": down, "button": "left"}}]}})

def drain(s, sec):
    out = b""
    d = time.time() + sec
    while time.time() < d:
        r, _, _ = select.select([s], [], [], 0.2)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out

def main():
    q = boot()
    try:
        ser = conn(SER)
        qmp = conn(QMP)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        buf = b""
        d = time.time() + 25
        while time.time() < d:
            buf += drain(ser, 0.4)
            if b"$ " in buf: break

        ser.sendall(b"notepad /tmp/dbg.txt\n")
        d = time.time() + 6
        while time.time() < d:
            buf += drain(ser, 0.4)
            if b"[wm] window created" in buf: break
        time.sleep(0.8)

        gcx, gcy = 140 + 720 - 6, 140 + 24 + 440 - 6
        sys.stderr.write(f"grip at ({gcx},{gcy})\n")
        cur(qmp, gcx, gcy); time.sleep(0.15)
        btn(qmp, True); time.sleep(0.15)
        for i in range(1, 9):
            cur(qmp, gcx + 20 * i, gcy + 12 * i)
            time.sleep(0.12)
        btn(qmp, False); time.sleep(0.6)

        buf += drain(ser, 1.0)
        sys.stdout.write(buf[-9000:].decode("ascii", "replace"))
    finally:
        try: q.kill(); q.wait(timeout=3)
        except Exception: pass
        cleanup()

if __name__ == "__main__":
    main()

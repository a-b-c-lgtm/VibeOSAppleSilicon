#!/usr/bin/env python3
"""scripts/_dbg_browser_resize_visual.py — capture a screenshot
during a browser resize drag so we can VISUALLY inspect whether
the bug #3 black-grown-region symptom reproduces.

Outputs three PPM files:
  /tmp/_dbg_brrsz_before.ppm   -- before any resize
  /tmp/_dbg_brrsz_during.ppm   -- mid-drag (button still down)
  /tmp/_dbg_brrsz_after.ppm    -- after drag end + 2 s settle

Look at the "during" frame and check whether the area in the
window's grown region is black, wsd-placeholder (gray-blue), or
browser content.
"""
import json, os, socket, subprocess, sys, time, select, re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/_dbg-brrsz-qmp.sock"
SERIAL_SOCK = "/tmp/_dbg-brrsz-ser.sock"

FB_W = 1280
FB_H = 1024
ABS_MAX = 0x7FFF

def cleanup():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass

def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-qmp", f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", f"virtio-gpu-device,xres={FB_W},yres={FB_H}",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def conn(p):
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(p):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(p); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError(p)

def qrl(q):
    buf = b""
    while not buf.endswith(b"\n"):
        c = q.recv(4096); buf += c
    return json.loads(buf)

def qsend(q, o):
    q.sendall((json.dumps(o) + "\n").encode())
    while True:
        m = qrl(q)
        if "return" in m or "error" in m: return m

def s2a(x, y): return (int(x * ABS_MAX / FB_W), int(y * ABS_MAX / FB_H))

def cur(q, x, y):
    ax, ay = s2a(x, y)
    qsend(q, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}]}})

def btn(q, d):
    qsend(q, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": d, "button": "left"}}]}})

def dump(q, p):
    try: os.unlink(p)
    except FileNotFoundError: pass
    qsend(q, {"execute": "screendump", "arguments": {"filename": p}})
    d = time.time() + 2.0
    while time.time() < d:
        if os.path.exists(p) and os.path.getsize(p) > 0:
            time.sleep(0.05); break
        time.sleep(0.05)

def drain(s, d):
    out = b""
    while time.time() < d:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out

def wait_for(s, needle, t):
    if isinstance(needle, str): needle = needle.encode()
    d = time.time() + t
    buf = b""
    while time.time() < d:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK); qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})
        wait_for(ser, b"$ ", 25.0)
        ser.sendall(b"browser --gui /mnt/test_layout.html\n")
        log = wait_for(ser, b"[browser] gui window=", 8.0)
        m = re.search(rb"\[browser\] gui window=\d+ size=(\d+)x(\d+)", log)
        WIN_W = int(m.group(1)); WIN_H = int(m.group(2))
        WIN_X, WIN_Y = 140, 140
        TITLE_H = 24; GRIP = 12
        print(f"browser {WIN_W}x{WIN_H} at ({WIN_X},{WIN_Y})")
        time.sleep(2.0)

        # SKIP the move; just clamp the grip drag to stay onscreen.
        # Where IS the grip in scanout coords?
        grip_cx = WIN_X + WIN_W - GRIP // 2
        grip_cy = WIN_Y + TITLE_H + WIN_H - GRIP // 2
        print(f"grip at ({grip_cx}, {grip_cy})")
        if grip_cy >= FB_H - 5 or grip_cx >= FB_W - 5:
            print(f"  ! grip off-screen ({grip_cx},{grip_cy}); aborting")
            return 1

        cur(qmp, 50, 50); time.sleep(0.3)
        dump(qmp, "/tmp/_dbg_brrsz_before.ppm")
        print("captured before")

        # Drag the grip, capturing during AND draining serial.
        tgt_cx  = min(grip_cx + 60, FB_W - 5)
        tgt_cy  = min(grip_cy + 30, FB_H - 5)
        if tgt_cx <= grip_cx or tgt_cy <= grip_cy:
            print("  ! no room to grow"); return 1
        print(f"drag {grip_cx},{grip_cy} -> {tgt_cx},{tgt_cy}")
        cur(qmp, grip_cx, grip_cy); time.sleep(0.1)
        btn(qmp, True); time.sleep(0.1)
        # First half of drag.
        midx = (grip_cx + tgt_cx) // 2
        midy = (grip_cy + tgt_cy) // 2
        steps = 8
        for i in range(1, steps + 1):
            ix = grip_cx + (midx - grip_cx) * i // steps
            iy = grip_cy + (midy - grip_cy) * i // steps
            cur(qmp, ix, iy); time.sleep(0.04)
        # Screenshot mid-drag.
        dump(qmp, "/tmp/_dbg_brrsz_during.ppm")
        print("captured during")
        # Second half.
        for i in range(1, steps + 1):
            ix = midx + (tgt_cx - midx) * i // steps
            iy = midy + (tgt_cy - midy) * i // steps
            cur(qmp, ix, iy); time.sleep(0.04)
        btn(qmp, False); time.sleep(0.2)
        cur(qmp, 50, 50); time.sleep(2.0)
        dump(qmp, "/tmp/_dbg_brrsz_after.ppm")
        print("captured after")
        # Drain serial for debugging.
        out = drain(ser, time.time() + 0.5)
        print("---- serial tail ----")
        print(out.decode("ascii", "replace"))
        return 0
    finally:
        try: q.kill()
        except Exception: pass
        try: q.wait(timeout=2)
        except Exception: pass
        cleanup()

if __name__ == "__main__":
    sys.exit(main())

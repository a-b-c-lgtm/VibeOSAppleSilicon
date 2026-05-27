#!/usr/bin/env python3
"""scripts/test_tablet.py — tablet smoke test.

Boots the kernel headless with virtio-keyboard + virtio-tablet,
launches the `paint` userspace demo via the virtio-keyboard, drives
the tablet via QMP input-send-event, screen-grabs the framebuffer
with QMP screendump, and verifies that:

  1. The tablet driver probed ('mouse online').
  2. The paint window opens (WM logs '[wm] window created').
  3. After we drag with the left button held, NEW pixels appear
     along the drag line (different from the pre-drag frame).
  4. After we click the close button, the window disappears
     (centre pixel changes back away from the post-paint colour).

Headless via -display none + Unix-socket -serial + QMP socket.  Same
input-via-virtio-keyboard pattern as scripts/test_wm.py.
"""
import json
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-tablet.sock"
SERIAL_SOCK = "/tmp/osdev-serial-tablet.sock"
DUMP_PATH   = "/tmp/osdev-fb-tablet.ppm"

FB_W = 1280
FB_H = 800
ABS_MAX = 0x7FFF

# Match wm.c / paint.c.
WIN_W   = 600
WIN_H   = 400
# chapter 118 — wsd paints a 24-px title bar above each
# decorated window.  Paint is decorated (flags = 0), so its
# body starts at WIN_Y + TITLE_H.  Paint still quits on ESC.
TITLE_H = 24
# wsd cascade base is (100, 100) step (40, 40).  Launcher claims
# slot 0 (100, 100); paint -- the second app spawned -- claims
# slot 1 at (140, 140).
WIN_X   = 140
WIN_Y   = 140

def cleanup():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass

def boot_qemu():
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

def wait_for_socket(path, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {path}")

def qmp_recv_line(qmp):
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = qmp.recv(4096)
        if not chunk: raise RuntimeError("QMP closed")
        buf += chunk
    return json.loads(buf)

def qmp_send(qmp, obj):
    qmp.sendall((json.dumps(obj) + "\n").encode())
    while True:
        msg = qmp_recv_line(qmp)
        if "return" in msg or "error" in msg:
            return msg

KEYMAP = {
    "a":"a","b":"b","c":"c","d":"d","e":"e","f":"f","g":"g","h":"h",
    "i":"i","j":"j","k":"k","l":"l","m":"m","n":"n","o":"o","p":"p",
    "q":"q","r":"r","s":"s","t":"t","u":"u","v":"v","w":"w","x":"x",
    "y":"y","z":"z",
    "0":"0","1":"1","2":"2","3":"3","4":"4","5":"5","6":"6","7":"7",
    "8":"8","9":"9",
    " ":"spc", "\n":"ret", "\x1b":"esc",
}

def send_key(qmp, qcode):
    for down in (True, False):
        qmp_send(qmp, {
            "execute": "input-send-event",
            "arguments": {"events": [{
                "type": "key",
                "data": {"down": down,
                         "key": {"type": "qcode", "data": qcode}},
            }]},
        })

def type_text(qmp, text):
    for ch in text:
        send_key(qmp, KEYMAP[ch]); time.sleep(0.04)

def screen_to_abs(sx, sy):
    return (sx * ABS_MAX) // (FB_W - 1), (sy * ABS_MAX) // (FB_H - 1)

def send_motion(qmp, sx, sy):
    ax, ay = screen_to_abs(sx, sy)
    qmp_send(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})

def send_button(qmp, sx, sy, button, down):
    ax, ay = screen_to_abs(sx, sy)
    qmp_send(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
        {"type": "btn", "data": {"button": button, "down": down}},
    ]}})

def screendump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qmp_send(qmp, {"execute": "screendump", "arguments": {"filename": path}})
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.05); break
        time.sleep(0.05)

def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise RuntimeError("not a P6 PPM")
    idx = 0
    def token():
        nonlocal idx
        while data[idx:idx+1] in (b" ", b"\n", b"\r", b"\t"): idx += 1
        if data[idx:idx+1] == b"#":
            while data[idx:idx+1] not in (b"\n", b""): idx += 1
            return token()
        start = idx
        while data[idx:idx+1] not in (b" ", b"\n", b"\r", b"\t", b""): idx += 1
        return data[start:idx]
    magic = token(); w = int(token()); h = int(token()); maxv = int(token())
    assert magic == b"P6" and maxv == 255
    idx += 1
    return w, h, data[idx: idx + w*h*3]

def pixel_at(pixels, w, x, y):
    o = (y*w + x)*3
    return (pixels[o], pixels[o+1], pixels[o+2])

def drain(ser, deadline):
    out = b""
    while time.time() < deadline:
        r,_,_ = select.select([ser], [], [], 0.1)
        if r:
            chunk = ser.recv(4096)
            if not chunk: break
            out += chunk
    return out

def wait_for(ser, needle, timeout, accumulator=b""):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = bytearray(accumulator)
    while time.time() < deadline:
        buf += drain(ser, time.time() + 0.4)
        if needle in bytes(buf):
            return bytes(buf)
    return bytes(buf)

def main():
    q = boot_qemu()
    try:
        ser = wait_for_socket(SERIAL_SOCK)
        qmp = wait_for_socket(QMP_SOCK)
        qmp_recv_line(qmp); qmp_send(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 20.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached"); print(boot_log.decode("ascii","replace")); return 1
        if b"mouse online" not in boot_log:
            print("FAIL: tablet did not probe"); print(boot_log.decode("ascii","replace")); return 1
        if b"window manager ... ok" not in boot_log:
            print("FAIL: WM did not initialise"); return 1
        print("PASS: tablet + WM probed, shell ready")

        # Spawn paint via the serial socket (sh's actual stdin).
        # Since the launcher window auto-focuses at boot, QMP
        # keystrokes go to the launcher (mouse-only) and never reach
        # sh.  Routing through serial bypasses the WM input path.
        ser.sendall(b"paint\n")
        log = wait_for(ser, b"[wm] window created", 5.0)
        if b"[wm] window created" not in log:
            print("FAIL: paint window did not appear"); print(log.decode("ascii","replace")); return 1
        print("PASS: paint window opened")

        time.sleep(0.4)

        cx = WIN_X + WIN_W // 2
        cy = WIN_Y + TITLE_H + WIN_H // 2
        screendump(qmp, DUMP_PATH)
        w_pre, _, pre = read_ppm(DUMP_PATH)
        before = pixel_at(pre, w_pre, cx, cy)
        print(f"  centre pixel before drag: {before}")

        send_motion(qmp, cx, cy)
        time.sleep(0.05)
        send_button(qmp, cx, cy, "left", True)
        time.sleep(0.05)
        for i in range(8):
            send_motion(qmp, cx + 4*i, cy + 2*i)
            time.sleep(0.05)
        send_button(qmp, cx + 32, cy + 16, "left", False)
        time.sleep(0.4)

        screendump(qmp, DUMP_PATH)
        w_post, _, post = read_ppm(DUMP_PATH)
        changed = 0
        for i in range(8):
            sx = cx + 4*i; sy = cy + 2*i
            if pixel_at(pre, w_pre, sx, sy) != pixel_at(post, w_post, sx, sy):
                changed += 1
        print(f"  drag-line pixels changed: {changed}/8")
        if changed < 4:
            print("FAIL: drag did not paint enough pixels"); return 1
        print("PASS: drag painted into canvas")

        close_x = WIN_X + WIN_W - 10
        close_y = WIN_Y + 10
        # Chapter 117: no decoration / close button --
        # paint quits on ESC.  Click inside paint first to make
        # sure wsd focuses it, then send ESC via virtio-keyboard.
        # The earlier drag block already left-clicked inside paint
        # which should have transferred focus, but a fresh click
        # is cheap insurance against compositor focus quirks.
        send_motion(q if False else qmp, close_x, close_y)
        # Actually click in the middle of the window where it's
        # definitely paint (close_x might be outside the new
        # window rect if the geometry shifts).
        send_motion(qmp, WIN_X + 20, WIN_Y + 60)
        send_button(qmp, WIN_X + 20, WIN_Y + 60, "left", True)
        send_button(qmp, WIN_X + 20, WIN_Y + 60, "left", False)
        time.sleep(0.2)
        send_key(qmp, "esc")
        wait_for(ser, b"[wm] destroyed window", 3.0)
        time.sleep(0.4)
        screendump(qmp, DUMP_PATH)
        w_g, _, gone_px = read_ppm(DUMP_PATH)
        after = pixel_at(gone_px, w_g, cx, cy)
        post_centre = pixel_at(post, w_post, cx, cy)
        if after == post_centre:
            print(f"FAIL: window was not closed (centre still {after})"); return 1
        print(f"PASS: close via ESC worked (centre {post_centre} -> {after})")

        print("\nALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

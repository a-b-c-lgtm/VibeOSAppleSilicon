#!/usr/bin/env python3
"""scripts/test_launcher.py — milestone-44 smoke test.

Boots headless, launches the launcher window, then drives the
mouse via virtio-tablet to click the "gui_term" button.  Verifies
that a SECOND WM window is created (the spawned gui_term).
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-launch.sock"
SERIAL_SOCK = "/tmp/osdev-serial-launch.sock"
DUMP_PATH   = "/tmp/osdev-fb-launch.ppm"

FB_W = 1280
FB_H = 800

# First window the WM creates lands at (80, 60).
WIN_X, WIN_Y = 80, 60
WIN_W, WIN_H = 240, 180
TITLE_H      = 24

# Tablet absolute axis range (QEMU virtio-tablet uses 0..0x7FFF).
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

def conn(path):
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError(f"no socket: {path}")

def qrl(qmp):
    buf = b""
    while not buf.endswith(b"\n"):
        c = qmp.recv(4096)
        if not c: raise RuntimeError("qmp closed")
        buf += c
    return json.loads(buf)

def qsend(qmp, obj):
    qmp.sendall((json.dumps(obj) + "\n").encode())
    while True:
        m = qrl(qmp)
        if "return" in m or "error" in m: return m

KEYMAP = {**{c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"},
          " ": "spc", "\n": "ret", "\x1b": "esc",
          ".": "dot", "/": "slash", "-": "minus"}

def send_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [{
            "type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]}})

def type_text(qmp, text):
    for ch in text:
        send_key(qmp, KEYMAP[ch]); time.sleep(0.04)

def screen_to_abs(x_screen, y_screen):
    """Map a (pixel x, pixel y) coordinate to QEMU virtio-tablet
    absolute axis values."""
    return (int(x_screen * ABS_MAX / FB_W),
            int(y_screen * ABS_MAX / FB_H))

def move_to(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})

def left_click(qmp, x, y):
    move_to(qmp, x, y)
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": True,  "button": "left"}},
    ]}})
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}},
    ]}})

def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out

def wait_for(s, needle, timeout):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf

def count_window_creates(buf):
    return buf.count(b"[wm] window created")

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 20.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell ready")

        # Post-M46, init auto-spawns /bin/launcher at boot, so the
        # launcher window is already in the WM by the time the shell
        # prompt appears.  We don't need to type 'launcher' again.
        # We do need to confirm the launcher window exists.
        cumulative = boot_log
        if count_window_creates(cumulative) < 1:
            # Give it a beat in case the WM created the window after
            # the first '$ ' was emitted.
            cumulative += wait_for(ser, b"[wm] window created", 3.0)
        if count_window_creates(cumulative) < 1:
            print("FAIL: launcher window not present"); return 1
        print("PASS: launcher window opened (auto-spawned by init)")

        time.sleep(0.4)

        # Sanity screendump of the launcher.
        screendump(qmp, DUMP_PATH)
        print(f"  saved screendump: {DUMP_PATH}")

        # Click the first button (gui_term).  Buttons are at
        # window-content y = 16 + i*(36+8) = 16 / 60 / 104, height 36.
        # Window first lands at (80, 60); content area starts at
        # (WIN_X, WIN_Y + TITLE_H) = (80, 84).  So button-0 centre =
        # (80 + 240/2, 84 + 16 + 18) = (200, 118).
        btn0_cx = WIN_X + WIN_W // 2
        btn0_cy = WIN_Y + TITLE_H + 16 + 18

        # Snapshot the cumulative serial buffer, then click.
        prev_creates = count_window_creates(cumulative)
        left_click(qmp, btn0_cx, btn0_cy)
        log2 = wait_for(ser, b"[wm] window created", 5.0)
        cumulative += log2
        new_creates = count_window_creates(cumulative)
        if new_creates - prev_creates < 1:
            print("FAIL: clicking button did not spawn a child window")
            print("--- serial since click ---")
            print(log2.decode("ascii","replace"))
            return 1
        print(f"PASS: click spawned a new window "
              f"(window-creates {prev_creates} -> {new_creates})")

        time.sleep(0.4)
        screendump(qmp, DUMP_PATH)
        print(f"  saved screendump after click: {DUMP_PATH}")

        # M45: assert the spawned gui_term window actually appears in
        # the framebuffer.  gui_term cascades to (112, 92) with a
        # 720x440 body and 24px title bar.  Sample a pixel deep in the
        # title bar at x=500 (well clear of the launcher's right edge
        # at x=320) and verify it is the WM title-bar blue rather than
        # the wallpaper navy.
        ppm = read_ppm(DUMP_PATH)
        title_pix    = pixel_at(ppm, 500, 100)   # gui_term title bar
        wallpaper_px = pixel_at(ppm, 1000, 700)  # bottom-right corner
        if title_pix == wallpaper_px:
            print(f"FAIL: gui_term title bar not painted; pixel at "
                  f"(500,100)={title_pix} matches wallpaper")
            return 1
        # Title bar is a desaturated blue: dominant blue channel.
        r, g, b = title_pix
        if not (b > r and b > 80):
            print(f"FAIL: pixel at (500,100)={title_pix} doesn't look "
                  f"like the WM title-bar blue")
            return 1
        print(f"PASS: spawned window rendered "
              f"(title-bar pixel at (500,100) = {title_pix})")

        print("\nMILESTONE 45: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


def screendump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {"execute": "screendump", "arguments": {"filename": path}})
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.05); break
        time.sleep(0.05)


def read_ppm(path):
    """Tiny PPM (P6) reader.  Returns (w, h, raw bytes RGB)."""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        assert magic == b"P6", f"bad magic {magic!r}"
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = (int(x) for x in line.split())
        maxval = int(f.readline().strip())
        assert maxval == 255
        data = f.read()
    return w, h, data

def pixel_at(ppm, x, y):
    w, h, data = ppm
    o = (y * w + x) * 3
    return data[o], data[o+1], data[o+2]


if __name__ == "__main__":
    sys.exit(main())

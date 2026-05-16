#!/usr/bin/env python3
"""scripts/test_paint_drag.py — reproduction for the "paint app
only draws on mouse release" bug.

Strategy
--------
Boot, click the launcher's "paint" button to spawn /bin/paint
(avoids any keyboard interaction).  Then exercise paint with a
synthetic drag:

    mouse_down at A
    mouse_move A -> B      (one synthetic intermediate point)
    SCREENSHOT  -- should already show stamps at A and along the
                   move; the bug claims the move stamp is missing
                   until release
    mouse_move B -> C
    SCREENSHOT  -- should show stamp at A, line A->B, line B->C
    mouse_up at C
    SCREENSHOT  -- final state, should match (or be a superset of)
                   the mid-drag screenshot

A pixel-color check at each waypoint between MOVE syscalls
verifies whether mid-drag stamps were rendered.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-paint.sock"
SERIAL_SOCK = "/tmp/osdev-serial-paint.sock"
DUMP_PATH   = "/tmp/osdev-fb-paint.ppm"

FB_W = 1280
FB_H = 800
ABS_MAX = 0x7FFF

# Launcher geometry — see userspace/launcher/launcher.c.
LAUNCHER_X, LAUNCHER_Y = 80, 60
LAUNCHER_W = 240
WM_BORDER  = 1
WM_TITLE_H = 24

# Launcher "paint" button (button index 1).  Center = (201, 162).
PAINT_BTN_X = LAUNCHER_X + WM_BORDER + 16 + 208 // 2  # 201
PAINT_BTN_Y = LAUNCHER_Y + WM_TITLE_H + 16 + 36 + 8 + 36 // 2  # 162

# Paint window: opens at the next auto-cascade slot.  Cascade slots
# are (80,60), (120,90), (160,120), ... in 40,30 increments.  The
# launcher used (80,60), so paint opens at (120,90).
PAINT_X = 120
PAINT_Y = 90
PAINT_W = 600
PAINT_H = 400

# Click points inside paint's content area.  The content origin
# (in screen coords) is at:
PAINT_CX0 = PAINT_X + WM_BORDER     # 121
PAINT_CY0 = PAINT_Y + WM_TITLE_H    # 114

# Three mid-canvas waypoints we'll drag through.  Stay well clear
# of paint's own header text (top 32 px) and the right-edge swatch
# (last 30 px).  BRUSH = 12, so the painted square at (sx,sy) covers
# [sx-6..sx+5] x [sy-6..sy+5] in window coords.
WAY_A = (PAINT_CX0 + 200, PAINT_CY0 + 200)
WAY_B = (PAINT_CX0 + 250, PAINT_CY0 + 220)
WAY_C = (PAINT_CX0 + 300, PAINT_CY0 + 240)

# When sampling the screen at a waypoint, the cursor sprite (12x19,
# hotspot at the top-left corner) sits at (sx..sx+11, sy..sy+18) and
# occludes the right/bottom of the stamp.  Sample at (sx-4, sy-2)
# which is inside the 12x12 brush footprint but outside the cursor.
SAMPLE_DX = -4
SAMPLE_DY = -2
def sample(p): return (p[0] + SAMPLE_DX, p[1] + SAMPLE_DY)


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

def screen_to_abs(x, y):
    return (int(x * ABS_MAX / FB_W), int(y * ABS_MAX / FB_H))

def move(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})

def button(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": bool(down), "button": "left"}},
    ]}})

def left_click(qmp, x, y):
    move(qmp, x, y); time.sleep(0.05)
    button(qmp, True); time.sleep(0.05)
    button(qmp, False)

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

# Paint default canvas is GUI_BGRA(0xF8,0xF8,0xF8) ~= near-white.
# Default colour PALETTE[0] is GUI_BGRA(0xC0,0x30,0x30) = red.
def is_canvas_white(p):
    return p[0] >= 230 and p[1] >= 230 and p[2] >= 230

def is_red_stamp(p):
    return p[0] >= 140 and p[1] <= 80 and p[2] <= 80

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached"); return 1
        time.sleep(0.6)

        # Step 1: spawn paint by clicking the launcher's PAINT button.
        print(f"  clicking launcher's paint button at "
              f"({PAINT_BTN_X}, {PAINT_BTN_Y})")
        left_click(qmp, PAINT_BTN_X, PAINT_BTN_Y)
        log = wait_for(ser, b"[wm] window created", 5.0)
        if b"[wm] window created" not in log:
            print("FAIL: paint window did not appear"); return 1
        time.sleep(0.5)
        print("PASS: paint window opened")

        # Sanity: canvas at WAY_A is currently white.
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        before_a = pixel_at(ppm, *sample(WAY_A))
        if not is_canvas_white(before_a):
            print(f"NOTE: WAY_A pixel before drag = {before_a} "
                  f"(expected near-white canvas)")

        # Step 2: press left button at WAY_A.
        print(f"  mouse-down at {WAY_A}")
        move(qmp, *WAY_A); time.sleep(0.05)
        button(qmp, True)
        time.sleep(0.2)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        after_down = pixel_at(ppm, *sample(WAY_A))
        if not is_red_stamp(after_down):
            print(f"FAIL: mouse-DOWN did not paint at WAY_A "
                  f"(pixel = {after_down})")
            return 1
        print(f"PASS: mouse-DOWN painted at WAY_A "
              f"(pixel = {after_down})")

        # Step 3: drag through WAY_B WHILE BUTTON IS HELD, then check
        # the pixel BEFORE releasing.  This is the bug: WAY_B should
        # show a red stamp at this moment.
        print(f"  drag (button held) -> {WAY_B}")
        move(qmp, *WAY_B)
        time.sleep(0.2)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        mid_b = pixel_at(ppm, *sample(WAY_B))
        print(f"  mid-drag pixel at WAY_B = {mid_b}")
        bug_observed_at_b = not is_red_stamp(mid_b)

        # Step 4: drag through WAY_C, also still holding.
        print(f"  drag (button held) -> {WAY_C}")
        move(qmp, *WAY_C)
        time.sleep(0.2)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        mid_c = pixel_at(ppm, *sample(WAY_C))
        print(f"  mid-drag pixel at WAY_C = {mid_c}")
        bug_observed_at_c = not is_red_stamp(mid_c)

        # Step 5: simulate a FAST drag — many move events with no
        # sleep between them — then sample mid-line points.  This
        # is the real-world failure mode reported by the user
        # (slow drags painted, fast drags lagged behind the mouse).
        FAST_A = (PAINT_CX0 + 50,  PAINT_CY0 + 320)
        FAST_B = (PAINT_CX0 + 550, PAINT_CY0 + 320)
        STEPS  = 50
        print(f"  fast drag {FAST_A} -> {FAST_B} ({STEPS} substeps, no sleep)")
        # Move to FAST_A first, button is still held from earlier.
        move(qmp, *FAST_A)
        time.sleep(0.05)
        for i in range(1, STEPS + 1):
            x = FAST_A[0] + (FAST_B[0] - FAST_A[0]) * i // STEPS
            y = FAST_A[1] + (FAST_B[1] - FAST_A[1]) * i // STEPS
            move(qmp, x, y)
        time.sleep(0.5)     # let WM + paint catch up

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        # Sample 5 evenly-spaced points along the fast-drag line.
        samples = []
        for i in range(1, 6):
            x = FAST_A[0] + (FAST_B[0] - FAST_A[0]) * i // 6
            y = FAST_A[1] + (FAST_B[1] - FAST_A[1]) * i // 6
            sx, sy = sample((x, y))
            p = pixel_at(ppm, sx, sy)
            samples.append((sx, sy, p, is_red_stamp(p)))
        red_count = sum(1 for _,_,_,ok in samples if ok)
        print(f"  fast-drag samples (red?): "
              + ", ".join(f"{p}={ok}" for _,_,p,ok in samples))
        if red_count < 4:
            print(f"FAIL: fast drag painted only {red_count}/5 sample "
                  f"points (paint lagged behind mouse motion)")
            return 1
        print(f"PASS: fast drag painted {red_count}/5 sample points")

        # Step 6: release button at FAST_B.
        print(f"  mouse-UP at {FAST_B}")
        button(qmp, False)
        time.sleep(0.4)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        post_b = pixel_at(ppm, *sample(WAY_B))
        post_c = pixel_at(ppm, *sample(WAY_C))
        print(f"  after-release pixel at WAY_B = {post_b}")
        print(f"  after-release pixel at WAY_C = {post_c}")

        if bug_observed_at_b or bug_observed_at_c:
            print("\n>>> mid-drag stamp at B/C missing.")
            print(f"    WAY_B: mid-drag={mid_b}  after-release={post_b}")
            print(f"    WAY_C: mid-drag={mid_c}  after-release={post_c}")
            return 1
        print("\nPAINT DRAG: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

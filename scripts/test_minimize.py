#!/usr/bin/env python3
"""scripts/test_minimize.py — wsd minimize/restore regression
test, originally chapter 118.

Exercises the userspace (wsd-side) minimize / restore plumbing:

  1. Boot to desktop.  Summon the launcher via the Start
     button (the launcher itself is NO_DECORATION and so does
     NOT exercise the title-bar minimize button; we use it
     just to spawn notepad).
  2. Click the launcher's "notepad" button.  Notepad is a
     decorated window at the wsd cascade origin (100, 100),
     720x440, with the wsd-painted 24-px title bar.
  3. Click notepad's title-bar minimize button.  wsd flips
     `hidden=1`, drops kernel-WM focus via gui_set_minimized,
     and full-recomposes.  Notepad's body pixels become
     wallpaper pixels.
  4. The taskbar (which polls WM_LIST every 150 ms) sees the
     new GUI_WIN_FLAG_MINIMIZED bit and re-renders the
     "notepad" cell in the dim CELL_MIN_BGRA palette.
  5. Click the dim taskbar cell.  Taskbar maps the click x
     back to a cell index, looks up the win_id stashed at
     paint time, and sends WM_WIN_RESTORE.  wsd clears
     `hidden`, re-raises, and full-recomposes; notepad body
     pixels return.

History note: prior to the Start-menu rewrite of the launcher
this test drove the launcher's own title-bar minimize button.
The launcher no longer has a title bar (NO_DECORATION) so we
moved the test to notepad, which still has decorations and so
still exercises the exact wsd code path the test cares about.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-min.sock"
SERIAL_SOCK = "/tmp/osdev-serial-min.sock"
DUMP_PATH   = "/tmp/osdev-fb-min.ppm"

FB_W = 1280
FB_H = 800

# Taskbar / Start button (must match userspace/taskbar/taskbar.c).
TASKBAR_H          = 28
START_BTN_X        = 8
START_BTN_Y_OFFSET = 4
START_BTN_W        = 60
START_BTN_H        = TASKBAR_H - 8

# Launcher panel: NO_DECORATION, 240x232, pinned just above
# the taskbar at x = 0.
LAUNCHER_W, LAUNCHER_H = 240, 232
LAUNCHER_X             = 0
LAUNCHER_Y             = FB_H - TASKBAR_H - LAUNCHER_H

# Launcher button layout (userspace/launcher/launcher.c):
#   BTN_TOP = 16, button h = 36, gap = 8.
# Button 0: y = 16, button 1: y = 60, button 2: y = 104, ...
# Order is: gui_term, paint, notepad, browser  (button index 2).
LAUNCHER_BTN_TOP  = 16
LAUNCHER_BTN_H    = 36
LAUNCHER_BTN_GAP  = 8
LAUNCHER_NOTEPAD_IDX = 2
NOTEPAD_BTN_CX = LAUNCHER_X + LAUNCHER_W // 2
NOTEPAD_BTN_CY = (LAUNCHER_Y + LAUNCHER_BTN_TOP
                  + LAUNCHER_NOTEPAD_IDX *
                    (LAUNCHER_BTN_H + LAUNCHER_BTN_GAP)
                  + LAUNCHER_BTN_H // 2)

# Notepad window geometry — userspace/notepad/notepad.c
# (WIN_W/WIN_H) at wsd cascade base (WM_CASCADE_BASE_X/Y).
NOTEPAD_X, NOTEPAD_Y = 100, 100
NOTEPAD_W, NOTEPAD_H = 720, 440

# wsd decoration constants — userspace/wsd/wsd.c.
WSD_TITLE_H     = 24
WSD_CLOSE_BTN_W = 20
WSD_MIN_BTN_W   = 20
WSD_BTN_GAP     = 2
WSD_BTN_INSET   = 2

# Close button: cb_x = bar_x + bar_w - WSD_CLOSE_BTN_W - WSD_BTN_INSET.
# Minimize button: mb_x = cb_x - WSD_BTN_GAP - WSD_MIN_BTN_W.
_CB_X = NOTEPAD_X + NOTEPAD_W - WSD_CLOSE_BTN_W - WSD_BTN_INSET
_MB_X = _CB_X - WSD_BTN_GAP - WSD_MIN_BTN_W
MIN_BTN_CX = _MB_X + WSD_MIN_BTN_W // 2
MIN_BTN_CY = NOTEPAD_Y + WSD_BTN_INSET + (WSD_TITLE_H - 2 * WSD_BTN_INSET) // 2

# Body sample — well below notepad's title bar, inside the
# warm off-white background.  Avoid the status bar at the
# bottom (STATUS_H ~ 20).
BODY_SX = NOTEPAD_X + 60
BODY_SY = NOTEPAD_Y + WSD_TITLE_H + 100

# Notepad BG_BGRA = GUI_BGRA(0xF8, 0xF8, 0xF0) = warm off-white.
NOTEPAD_BG_RGB = (0xF8, 0xF8, 0xF0)

# Taskbar cells start at CELLS_X0 = 8 + 60 + 8 = 76.  Cell 0
# width 180, so cx range [76, 256].  Sample well right of the
# "notepad" label so we land on plain cell fill, not text.
CELLS_X0             = 76
TASKBAR_CELL0_CX     = CELLS_X0 + 12          # near label, still clickable
TASKBAR_CELL0_CY     = FB_H - TASKBAR_H + 14  # bar_y + 14
TASKBAR_CELL_FILL_SX = CELLS_X0 + 160         # right edge, plain fill
TASKBAR_CELL_FILL_SY = TASKBAR_CELL0_CY

# Taskbar palette — userspace/taskbar/taskbar.c.
CELL_MIN_RGB    = (0x18, 0x20, 0x38)
CELL_NORMAL_RGB = (0x30, 0x40, 0x70)

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

def screen_to_abs(x, y):
    return (int(x * ABS_MAX / FB_W), int(y * ABS_MAX / FB_H))

def left_click(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": True,  "button": "left"}}]}})
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}]}})

def click_start_button(qmp):
    cx = START_BTN_X + START_BTN_W // 2
    cy = (FB_H - TASKBAR_H) + START_BTN_Y_OFFSET + START_BTN_H // 2
    left_click(qmp, cx, cy)

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

def near(a, b, tol=10):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 25.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached")
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")
        time.sleep(0.6)

        # Step 1: summon the launcher via Start, then click its
        # "notepad" button to spawn the decorated window we'll
        # actually test against.
        click_start_button(qmp)
        wait_for(ser, b"[taskbar] start -> show launcher", 3.0)
        time.sleep(0.35)
        left_click(qmp, NOTEPAD_BTN_CX, NOTEPAD_BTN_CY)
        wait_for(ser, b"[wm] window created", 5.0)
        time.sleep(0.8)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        # Notepad must be visible (BG pixel = warm off-white).
        body0 = pixel_at(ppm, BODY_SX, BODY_SY)
        if not near(body0, NOTEPAD_BG_RGB, tol=15):
            print(f"FAIL: notepad BG not visible after spawn — "
                  f"({BODY_SX},{BODY_SY}) = {body0}, "
                  f"expected ~{NOTEPAD_BG_RGB}")
            return 1
        print(f"PASS: notepad visible after spawn (body = {body0})")

        # Step 2: click notepad's title-bar minimize button.
        print(f"  clicking minimize button at "
              f"({MIN_BTN_CX}, {MIN_BTN_CY})")
        left_click(qmp, MIN_BTN_CX, MIN_BTN_CY)
        time.sleep(0.5)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body1 = pixel_at(ppm, BODY_SX, BODY_SY)
        if near(body1, NOTEPAD_BG_RGB, tol=15):
            print(f"FAIL: notepad still visible after minimize — "
                  f"({BODY_SX},{BODY_SY}) = {body1}")
            return 1
        print(f"PASS: minimize hid notepad body (body now = {body1})")

        # Step 3: taskbar cell now in dim CELL_MIN palette.
        cell_pix = pixel_at(ppm, TASKBAR_CELL_FILL_SX, TASKBAR_CELL_FILL_SY)
        if not near(cell_pix, CELL_MIN_RGB, tol=15):
            print(f"FAIL: minimized cell not dim — "
                  f"({TASKBAR_CELL_FILL_SX},{TASKBAR_CELL_FILL_SY}) "
                  f"= {cell_pix}, expected ~{CELL_MIN_RGB}")
            return 1
        print(f"PASS: notepad cell rendered dim (pixel = {cell_pix})")

        # Step 4: click the (dim) taskbar cell to restore.
        print(f"  clicking taskbar cell at "
              f"({TASKBAR_CELL0_CX}, {TASKBAR_CELL0_CY})")
        left_click(qmp, TASKBAR_CELL0_CX, TASKBAR_CELL0_CY)
        time.sleep(0.6)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body2 = pixel_at(ppm, BODY_SX, BODY_SY)
        if not near(body2, NOTEPAD_BG_RGB, tol=15):
            print(f"FAIL: notepad not restored — "
                  f"({BODY_SX},{BODY_SY}) = {body2}, "
                  f"expected ~{NOTEPAD_BG_RGB}")
            return 1
        print(f"PASS: taskbar cell click restored notepad (body = {body2})")

        # Cell should be back to the plain (non-minimized) fill.
        cell_pix2 = pixel_at(ppm, TASKBAR_CELL_FILL_SX, TASKBAR_CELL_FILL_SY)
        if not near(cell_pix2, CELL_NORMAL_RGB, tol=20):
            print(f"  note: restored cell pixel = {cell_pix2}, "
                  f"expected near {CELL_NORMAL_RGB}")
        else:
            print(f"PASS: restored cell back to normal "
                  f"(pixel = {cell_pix2})")

        print("\nMINIMIZE REGRESSION: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

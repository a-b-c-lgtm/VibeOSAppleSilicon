#!/usr/bin/env python3
"""scripts/test_minimize.py — milestone-51 smoke test.

Boots fully headless.  Verifies the new "minimize / restore"
plumbing end-to-end:

  1. The auto-spawned launcher window is visible after boot.
  2. Clicking the launcher's title-bar minimize button (the new
     grey "_" button immediately left of the red close X) hides
     it: the launcher's body pixels become wallpaper pixels.
  3. The launcher's taskbar cell is still present (minimize !=
     destroy) and rendered DIM (CELL_MIN_BGRA = 0x18,0x20,0x38).
  4. Clicking the dim taskbar cell restores the launcher: body
     pixels become white again.
  5. With launcher restored AND focused, clicking its taskbar
     cell minimizes it again (focused-cell -> minimize toggle).

We exercise both the title-bar minimize button (kernel WM path)
and the taskbar's tri-state click handler (userspace + kernel
SYS_GUI_SET_MINIMIZED + SYS_GUI_RAISE_WINDOW auto-restore).
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-min.sock"
SERIAL_SOCK = "/tmp/osdev-serial-min.sock"
DUMP_PATH   = "/tmp/osdev-fb-min.ppm"

FB_W = 1280
FB_H = 800

# Launcher window geometry — known from userspace/launcher/launcher.c
# and the WM's first-cascade slot.
LAUNCHER_X, LAUNCHER_Y = 80, 60
LAUNCHER_W, LAUNCHER_H = 240, 180
WM_TITLE_H              = 24
WM_BORDER               = 1
WM_CLOSE_BTN_W          = 20
WM_MIN_BTN_W            = 20
WM_BTN_GAP              = 2

# A pixel deep inside the launcher's content body (white BG).
BODY_SX, BODY_SY = 200, 90

# Title-bar minimize-button center.
DECO_W = LAUNCHER_W + 2 * WM_BORDER
MIN_BTN_X0 = LAUNCHER_X + DECO_W - WM_CLOSE_BTN_W - 2 - WM_BTN_GAP - WM_MIN_BTN_W
MIN_BTN_CX = MIN_BTN_X0 + WM_MIN_BTN_W // 2     # = 288
MIN_BTN_CY = LAUNCHER_Y + 2 + (WM_TITLE_H - 4) // 2  # = 72

# Taskbar cell-0 center: cell at x=8, y=4 inside a bar that sits at
# y = FB_H - 28 = 772.  Cell is 180x20 → center (8+90, 776+10) =
# (98, 786).
TASKBAR_CELL0_CX = 98
TASKBAR_CELL0_CY = 786

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

def is_whiteish(p, threshold=200):
    return p[0] >= threshold and p[1] >= threshold and p[2] >= threshold

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
        if b"launching /bin/launcher" not in boot_log and \
           b"[launcher]" not in boot_log:
            # Best-effort: launcher still spawns even if init's banner
            # changes.  We'll detect it via screendump in a moment.
            pass
        print("PASS: shell prompt reached")

        time.sleep(0.6)     # let launcher + taskbar render
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        # Step 1: launcher visible.
        body0 = pixel_at(ppm, BODY_SX, BODY_SY)
        if not is_whiteish(body0):
            print(f"FAIL: launcher BG not white at boot — "
                  f"({BODY_SX},{BODY_SY}) = {body0}")
            return 1
        print(f"PASS: launcher visible at boot (body = {body0})")

        # Step 2: click the title-bar minimize button.
        print(f"  clicking minimize button at ({MIN_BTN_CX}, {MIN_BTN_CY})")
        left_click(qmp, MIN_BTN_CX, MIN_BTN_CY)
        time.sleep(0.4)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body1 = pixel_at(ppm, BODY_SX, BODY_SY)
        if is_whiteish(body1):
            print(f"FAIL: launcher still visible after minimize — "
                  f"({BODY_SX},{BODY_SY}) = {body1}")
            return 1
        print(f"PASS: minimize hid launcher (body now = {body1})")

        # Step 3: taskbar cell still there but DIM.  Sample inside
        # cell 0 well right of the "launcher" label.
        cell_pix = pixel_at(ppm, 170, 786)
        # CELL_MIN_BGRA = (24, 32, 56) in RGB.
        if not near(cell_pix, (24, 32, 56), tol=15):
            print(f"FAIL: minimized cell not dim — "
                  f"(170,786) = {cell_pix}, expected ~(24,32,56)")
            return 1
        print(f"PASS: launcher cell rendered dim (pixel = {cell_pix})")

        # Step 4: click the (dim) taskbar cell to restore.
        print(f"  clicking taskbar cell at "
              f"({TASKBAR_CELL0_CX}, {TASKBAR_CELL0_CY})")
        left_click(qmp, TASKBAR_CELL0_CX, TASKBAR_CELL0_CY)
        time.sleep(0.4)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body2 = pixel_at(ppm, BODY_SX, BODY_SY)
        if not is_whiteish(body2):
            print(f"FAIL: launcher not restored — "
                  f"({BODY_SX},{BODY_SY}) = {body2}")
            return 1
        print(f"PASS: taskbar cell click restored launcher (body = {body2})")

        # Sanity: cell should now be in focused colour
        # (CELL_FOCUS_BGRA = (96, 144, 224) in RGB).
        cell_pix2 = pixel_at(ppm, 170, 786)
        if not near(cell_pix2, (96, 144, 224), tol=20):
            print(f"  note: restored cell pixel = {cell_pix2} "
                  f"(expected near (96,144,224) for focused)")
        else:
            print(f"PASS: restored cell rendered FOCUSED "
                  f"(pixel = {cell_pix2})")

        # Step 5: click the focused cell to minimize again.
        print(f"  clicking focused cell at "
              f"({TASKBAR_CELL0_CX}, {TASKBAR_CELL0_CY}) to minimize")
        left_click(qmp, TASKBAR_CELL0_CX, TASKBAR_CELL0_CY)
        time.sleep(0.4)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body3 = pixel_at(ppm, BODY_SX, BODY_SY)
        if is_whiteish(body3):
            print(f"FAIL: focused-cell click did not minimize — "
                  f"({BODY_SX},{BODY_SY}) = {body3}")
            return 1
        print(f"PASS: focused-cell click minimized launcher "
              f"(body = {body3})")

        print("\nMILESTONE 51: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

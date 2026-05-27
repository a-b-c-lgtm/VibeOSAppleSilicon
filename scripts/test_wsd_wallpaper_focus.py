#!/usr/bin/env python3
"""scripts/test_wsd_wallpaper_focus.py — chapter-109d follow-up.

Asserts that a click on the wallpaper does NOT steal keyboard
focus from a currently-active app.  Previously the click went
through wsd's body-click handler (point_in_body matched the
wallpaper, which is fullscreen) and called gui_raise_window on
the wallpaper's kernel shadow, silently setting kernel
g_focus_id = wallpaper.  Result: ESC and other keystrokes
went to the desktop process instead of the app the user was
actually using -- e.g. paint would not exit on ESC.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-wpf.sock"
SERIAL_SOCK = "/tmp/osdev-serial-wpf.sock"
DUMP_PATH   = "/tmp/osdev-fb-wpf.ppm"

FB_W = 1280
FB_H = 800

# Launcher's "paint" button (index 1, see launcher.c geometry).
# chapter 55 UX: launcher is a Start-menu panel, NODECORATION
# + ALWAYS_ON_TOP, anchored above the taskbar at (0, 540) and
# hidden at boot (Start button toggles it).
BAR_H      = 28
LAUNCHER_X = 0
LAUNCHER_Y = FB_H - BAR_H - 232            # 540
TITLE_H    = 0                             # NODECORATION
PAINT_BTN_X = LAUNCHER_X + 16 + 208 // 2   # 120
PAINT_BTN_Y = LAUNCHER_Y + TITLE_H + 78    # 618

# Taskbar's Start button (see userspace/taskbar/taskbar.c).
START_BTN_X      = 8
START_BTN_Y_OFF  = 4
START_BTN_W      = 60
START_BTN_H      = BAR_H - 8
START_CX = START_BTN_X + START_BTN_W // 2                       # 38
START_CY = (FB_H - BAR_H) + START_BTN_Y_OFF + START_BTN_H // 2  # 786

# Wallpaper click target: somewhere clearly not overlapped by
# the launcher or paint (paint opens at (100,100) 600x424; the
# launcher panel sits in the bottom-left).
WALLPAPER_X, WALLPAPER_Y = 1000, 400

# Paint corner swatch location, used as an "is paint alive?"
# detector (will be the red default colour while paint is
# alive and ESC unhandled, becomes wallpaper pixel after ESC).
# Paint is the first cascade-positioned client and lands at
# cascade slot 0 = (100, 100).
PAINT_X = 100
PAINT_Y = 100
PAINT_W = 600
PAINT_TITLE_H = 24
SWATCH_SX = PAINT_X + PAINT_W - 24 + 8
SWATCH_SY = PAINT_Y + PAINT_TITLE_H + 8 + 8

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


def move(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})


def button(qmp, down, which="left"):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": bool(down), "button": which}},
    ]}})


def click(qmp, x, y, which="left"):
    move(qmp, x, y); time.sleep(0.08)
    button(qmp, True, which); time.sleep(0.08)
    button(qmp, False, which); time.sleep(0.08)


def press_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [{
            "type": "key",
            "data": {"down": down,
                     "key": {"type": "qcode", "data": qcode}}}]}})


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
        assert magic == b"P6"
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = (int(x) for x in line.split())
        assert int(f.readline().strip()) == 255
        data = f.read()
    return w, h, data


def pixel_at(ppm, x, y):
    w, h, data = ppm
    o = (y * w + x) * 3
    return data[o], data[o + 1], data[o + 2]


def is_red(rgb): return rgb[0] >= 140 and rgb[1] <= 80 and rgb[2] <= 80


def main():
    q = boot()
    rc = 0
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell prompt reached")
        time.sleep(0.5)

        # Summon the launcher panel (hidden at boot).
        print(f"  clicking Start button at ({START_CX}, {START_CY}) "
              f"to summon launcher")
        click(qmp, START_CX, START_CY)
        wait_for(ser, b"start -> show launcher", 3.0)
        time.sleep(0.4)

        # Spawn paint via launcher.
        print(f"  clicking launcher paint button at "
              f"({PAINT_BTN_X}, {PAINT_BTN_Y})")
        click(qmp, PAINT_BTN_X, PAINT_BTN_Y)
        if b"[wmclient] window id=" not in wait_for(
                ser, b"[wmclient] window id=", 6.0):
            print("FAIL: paint did not open"); return 1
        time.sleep(1.0)
        move(qmp, 4, 4); time.sleep(0.3)

        # Sanity check: paint visible (red swatch).
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        sw0 = pixel_at(ppm, SWATCH_SX, SWATCH_SY)
        if not is_red(sw0):
            print(f"FAIL: pre-test paint swatch not red ({sw0})")
            return 1
        print(f"PASS: paint visible with red swatch ({sw0})")

        # Click the wallpaper.  The fix means this is a no-op:
        # no z change, no kernel focus change, no MOUSE_DOWN
        # delivered to the desktop.
        print(f"  clicking wallpaper at ({WALLPAPER_X}, {WALLPAPER_Y})")
        click(qmp, WALLPAPER_X, WALLPAPER_Y)
        time.sleep(0.5)
        move(qmp, 4, 4); time.sleep(0.3)

        # Paint must still be visible.
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        sw1 = pixel_at(ppm, SWATCH_SX, SWATCH_SY)
        if not is_red(sw1):
            print(f"FAIL: paint covered after wallpaper click ({sw1})")
            return 1
        print(f"PASS: paint still visible after wallpaper click")

        # ESC should still reach paint and make it exit.  If the
        # wallpaper click had stolen focus, ESC would go to the
        # desktop process and paint would stay alive.
        print(f"  sending ESC (should reach paint, not desktop)")
        press_key(qmp, "esc")
        time.sleep(1.0)
        move(qmp, 4, 4); time.sleep(0.3)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        sw2 = pixel_at(ppm, SWATCH_SX, SWATCH_SY)
        print(f"  post-ESC swatch pixel = {sw2}")
        if is_red(sw2):
            print(f"FAIL: ESC did not reach paint -- wallpaper click "
                  f"stole focus")
            rc = 1
        else:
            print(f"PASS: ESC exited paint -- wallpaper click did not "
                  f"steal focus")

        print("DONE" if rc == 0 else "DONE (failures above)")
        return rc
    finally:
        try: q.kill()
        except Exception: pass
        try: q.wait(timeout=2)
        except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

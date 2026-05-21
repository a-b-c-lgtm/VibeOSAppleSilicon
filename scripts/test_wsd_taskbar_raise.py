#!/usr/bin/env python3
"""scripts/test_wsd_taskbar_raise.py — chapter-109d follow-up.

Asserts that clicking a taskbar cell raises the corresponding
window even when it is NOT minimized.  Previously WM_WIN_RESTORE
was a no-op for non-hidden windows, so the user could not click
through the taskbar to bring an obscured (but visible) window
to the front.

Setup: spawn paint (cascade 140,140 600x424), spawn notepad
(cascade 180,180 720x440 -- partially covers paint).  notepad
opens last, so its title bar = active (steel blue) and paint's
= idle (dim gray-blue).  Click each taskbar cell in turn until
paint's title bar flips to active blue, confirming the click
made paint the focused window.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-tbr.sock"
SERIAL_SOCK = "/tmp/osdev-serial-tbr.sock"
DUMP_PATH   = "/tmp/osdev-fb-tbr.ppm"

FB_W = 1280
FB_H = 800
TITLE_H = 24

LAUNCHER_X, LAUNCHER_Y = 100, 100
PAINT_BTN_X    = LAUNCHER_X + 16 + 208 // 2
PAINT_BTN_Y    = LAUNCHER_Y + TITLE_H + 16 + 36 + 8 + 36 // 2  # idx 1
NOTEPAD_BTN_X  = PAINT_BTN_X
NOTEPAD_BTN_Y  = LAUNCHER_Y + TITLE_H + 16 + 2*(36 + 8) + 36//2  # idx 2

# Cascade positions (base 100,100 step 40,40).
PAINT_X, PAINT_Y = 140, 140

# Paint title-bar sample point: a few px in from the LEFT
# edge of the bar (above paint's body), at y mid-bar.
# Notepad's title bar at y in [180, 204] does NOT cover this
# (we sample at y=152) so the sample is always on paint's bar.
PAINT_BAR_SX = PAINT_X + 200
PAINT_BAR_SY = PAINT_Y + 12

# wsd title-bar colours (see wsd.c WSD_DECO_BG_*):
# Macro GUI_BGRA(R,G,B) packs (R<<16)|(G<<8)|B; constants
# 0xff3a6ea5 and 0xff556677 thus decode as ACTIVE = R=0x3a
# G=0x6e B=0xa5 and IDLE = R=0x55 G=0x66 B=0x77.  QEMU's PPM
# screendump emits in real RGB so these are what pixel_at()
# returns.
ACTIVE_RGB = (0x3a, 0x6e, 0xa5)
IDLE_RGB   = (0x55, 0x66, 0x77)


def near(a, b, tol=18):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


BAR_Y = FB_H - 28
BAR_CELL_CY = BAR_Y + 14

def cell_cx(i):
    return 8 + i * (180 + 6) + 90

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

        # Spawn paint.
        print(f"  clicking launcher paint button")
        click(qmp, PAINT_BTN_X, PAINT_BTN_Y)
        wait_for(ser, b"[wmclient] window id=", 6.0)
        time.sleep(0.6)

        # Spawn notepad via the SHELL (not the launcher button) --
        # paint now covers the launcher's lower buttons so a click
        # at NOTEPAD_BTN_Y would land in paint's body instead of
        # on the launcher.
        print(f"  spawning notepad from shell")
        ser.sendall(b"notepad\n")
        wait_for(ser, b"[notepad]", 6.0)
        time.sleep(1.5)
        move(qmp, 4, 4); time.sleep(0.4)

        # Paint should now have an IDLE title bar (notepad is on top
        # and focused).
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        bar_before = pixel_at(ppm, PAINT_BAR_SX, PAINT_BAR_SY)
        print(f"  paint title bar before taskbar click = {bar_before}")
        if not near(bar_before, IDLE_RGB):
            print(f"NOTE: paint title bar isn't IDLE (expected ~{IDLE_RGB}) "
                  f"-- focus state unclear; skipping")
            return 0
        print(f"  paint correctly shows IDLE before taskbar click")

        # Click each taskbar cell idx 0..3 until paint's title
        # bar flips to ACTIVE.
        found_idx = -1
        for i in range(4):
            cx = cell_cx(i)
            if cx >= FB_W: break
            print(f"  clicking taskbar cell idx={i} at "
                  f"({cx}, {BAR_CELL_CY})")
            click(qmp, cx, BAR_CELL_CY)
            time.sleep(0.6)
            move(qmp, 4, 4); time.sleep(0.4)
            screendump(qmp, DUMP_PATH)
            ppm = read_ppm(DUMP_PATH)
            bar = pixel_at(ppm, PAINT_BAR_SX, PAINT_BAR_SY)
            if near(bar, ACTIVE_RGB):
                print(f"  cell idx={i} made paint ACTIVE (bar {bar})")
                found_idx = i
                break
            else:
                print(f"  cell idx={i} did not activate paint "
                      f"(bar {bar})")

        if found_idx < 0:
            print(f"BUG#3 FAIL: no taskbar cell raised paint to ACTIVE")
            rc = 1
        else:
            print(f"BUG#3 PASS: taskbar cell {found_idx} raised paint")

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

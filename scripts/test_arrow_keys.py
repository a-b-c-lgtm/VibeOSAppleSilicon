#!/usr/bin/env python3
"""scripts/test_arrow_keys.py — bug-fix smoke test for the bug
where pressing an arrow key killed the focused GUI window.

Background
----------
Cursor / arrow keys come from virtio-input as the three-byte ANSI
CSI sequence  ESC [ X  (X in A/B/C/D).  Before this fix the WM
delivered each byte as its own GUI_EVENT_KEY, the first of which
looked indistinguishable from a real ESC keypress, and apps like
launcher / paint / notepad treat ESC as "quit" — so any arrow
key would close the focused window.

The fix is a small CSI parser in kernel/core/wm.c that absorbs
ESC + '[' + final into a single GUI_EVENT_KEY whose arg0 is one
of the GUI_KEY_* extended codes (0x101..0x106).  A bare ESC press
still arrives as arg0 = 0x1B because the parser flushes any
unfollowed ESC at the end of pump_input_into_wm.

This test verifies both halves of that fix:
  1. Pressing each of the four arrow keys leaves the launcher
     window on screen.
  2. Pressing ESC after that still closes the launcher.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-arrow.sock"
SERIAL_SOCK = "/tmp/osdev-serial-arrow.sock"
DUMP_PATH   = "/tmp/osdev-fb-arrow.ppm"

FB_W = 1280
FB_H = 800

# Launcher window geometry — see userspace/launcher/launcher.c.
# Post-Start-menu: launcher is a NO_DECORATION panel pinned just
# above the 28-px taskbar.  Origin = (0, FB_H - 28 - 232) and
# content area starts at the window origin (no title bar).  It
# is HIDDEN at boot; the taskbar's Start button summons it.
TASKBAR_H              = 28
LAUNCHER_W, LAUNCHER_H = 240, 232
LAUNCHER_X             = 0
LAUNCHER_Y             = FB_H - TASKBAR_H - LAUNCHER_H

# Start button geometry (must match taskbar.c START_BTN_*).
START_BTN_X        = 8
START_BTN_Y_OFFSET = 4
START_BTN_W        = 60
START_BTN_H        = TASKBAR_H - 8
ABS_MAX            = 0x7FFF

# A pixel deep inside the launcher's body (its BG_BGRA is light grey
# 0xE8,0xEC,0xF0).  Wallpaper at the same coords is much darker
# (greenish-blue gradient ~ 0x2F,0x45,0x5C in this region).
BODY_SX, BODY_SY = LAUNCHER_X + 10, LAUNCHER_Y + 10

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

def send_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {
            "execute": "input-send-event",
            "arguments": {"events": [{
                "type": "key",
                "data": {"down": down,
                         "key": {"type": "qcode", "data": qcode}},
            }]},
        })
        time.sleep(0.04)

def screen_to_abs(x_screen, y_screen):
    return (int(x_screen * ABS_MAX / FB_W),
            int(y_screen * ABS_MAX / FB_H))

def left_click(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": True,  "button": "left"}},
    ]}})
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}},
    ]}})

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

def is_launcher_body(p):
    """Launcher body BGRA = 0xE8,0xEC,0xF0 -> RGB ~232,236,240."""
    return p[0] >= 220 and p[1] >= 220 and p[2] >= 220

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        # Wait until shell prompt + launcher have rendered.
        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached"); return 1
        time.sleep(0.6)

        # Launcher boots HIDDEN now (Start-menu model).  Click
        # the taskbar's Start button to summon it before doing
        # any pixel-level visibility assertions.
        click_start_button(qmp)
        wait_for(ser, b"[taskbar] start -> show launcher", 3.0)
        time.sleep(0.4)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body0 = pixel_at(ppm, BODY_SX, BODY_SY)
        if not is_launcher_body(body0):
            print(f"FAIL: launcher body not visible after Start "
                  f"(pixel = {body0})")
            return 1
        print(f"PASS: launcher visible after Start click (pixel = {body0})")

        # Press each arrow key in turn.  Before the fix any one of
        # these would close the launcher (its CSI ESC byte was
        # delivered as a bare GUI_EVENT_KEY whose arg0 == 0x1B).
        for label, qcode in (("UP",    "up"),
                             ("DOWN",  "down"),
                             ("LEFT",  "left"),
                             ("RIGHT", "right")):
            send_key(qmp, qcode)
            time.sleep(0.15)
            screendump(qmp, DUMP_PATH)
            ppm = read_ppm(DUMP_PATH)
            body = pixel_at(ppm, BODY_SX, BODY_SY)
            if not is_launcher_body(body):
                print(f"FAIL: pressing {label} closed the launcher "
                      f"(body pixel now = {body})")
                return 1
            print(f"PASS: launcher still visible after {label} arrow")

        # Now press ESC.  Historically this closed the launcher
        # (it exited the process).  Post-Start-menu, ESC instead
        # asks wsd to MINIMIZE the launcher, so it disappears
        # from compose (body pixel becomes wallpaper) but the
        # process stays alive — clicking its taskbar cell would
        # bring it back.  The pixel-level observation is the
        # same as before, which is what we assert here.
        send_key(qmp, "esc")
        time.sleep(0.4)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body = pixel_at(ppm, BODY_SX, BODY_SY)
        if is_launcher_body(body):
            print(f"FAIL: ESC did NOT hide the launcher "
                  f"(body pixel still launcher-coloured = {body})")
            return 1
        print(f"PASS: ESC hid the launcher "
              f"(body pixel now wallpaper = {body})")

        print("\nARROW-KEY BUG: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

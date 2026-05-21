#!/usr/bin/env python3
"""scripts/test_wsd_paint_bugs.py — chapter-109d follow-up bugs.

Three user-reported bugs against paint after the wsd port:

  1. ESC no longer exits paint.
  2. Paint shows no colour indicator on startup, and right-click
     does not cycle the active colour.
  3. Clicking an application button on the launcher no longer
     makes the launched app the active (focused) window.

This script verifies all three.  Bug #1 is the easiest:
spawn paint, send ESC via virtio-keyboard, and look for paint's
exit log on the serial port.

Bug #2 has two halves: (a) screenshot right after paint starts
and check there's a recognisable swatch in the top-right corner
of the canvas, (b) right-click on the canvas, screenshot, and
check the swatch colour changed.

Bug #3: spawn paint via the launcher, then send ESC and check
paint actually receives it (vs. the launcher swallowing the key
because it kept focus).
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-paintbugs.sock"
SERIAL_SOCK = "/tmp/osdev-serial-paintbugs.sock"
DUMP_PATH   = "/tmp/osdev-fb-paintbugs.ppm"

FB_W = 1280
FB_H = 800

# Launcher geometry — see userspace/launcher/launcher.c.
# chapter 109e UX: launcher is a Start-menu-style panel,
# NODECORATION + ALWAYS_ON_TOP, anchored above the taskbar
# at (0, FB_H - BAR_H - 232) = (0, 540), hidden at boot.
# The taskbar's Start button toggles it.
BAR_H      = 28
LAUNCHER_X = 0
LAUNCHER_Y = FB_H - BAR_H - 232           # 540
LAUNCHER_W = 240
TITLE_H    = 0                            # NODECORATION

START_BTN_X      = 8
START_BTN_Y_OFF  = 4
START_BTN_W      = 60
START_BTN_H      = BAR_H - 8
START_CX = START_BTN_X + START_BTN_W // 2                       # 38
START_CY = (FB_H - BAR_H) + START_BTN_Y_OFF + START_BTN_H // 2  # 786

# Launcher button "paint" is index 1: window-relative
#   x = 16 + 208/2 = 120;
#   y = 16 + 1*(36+8) + 36/2 = 78.
PAINT_BTN_X = LAUNCHER_X + 120                  # 120
PAINT_BTN_Y = LAUNCHER_Y + TITLE_H + 78         # 618

# Paint opens at cascade slot 0 (the launcher uses _at and
# does NOT advance the cascade, so paint is the first cascade
# client) = (100, 100), 600x400, decorated with a 24-px wsd
# title bar.
PAINT_X = 100
PAINT_Y = 100
PAINT_W = 600
PAINT_H = 400
PAINT_TITLE_H = 24
# Paint canvas origin in screen coords.
PAINT_CX0 = PAINT_X
PAINT_CY0 = PAINT_Y + PAINT_TITLE_H

# Paint draws a 16x16 swatch at (WIDTH - 24, 8) in canvas coords.
# Screen coords therefore are (PAINT_CX0 + 576, PAINT_CY0 + 8).
SWATCH_SX = PAINT_CX0 + PAINT_W - 24 + 8   # centre of 16x16
SWATCH_SY = PAINT_CY0 + 8 + 8

# Right-click target deep in the canvas, well away from the
# header text + corner swatch.
RCLICK_X = PAINT_CX0 + 300
RCLICK_Y = PAINT_CY0 + 250

# Paint's PALETTE in BGRA: red, green, blue, yellow, magenta,
# cyan, near-black, white.  After one right-click, the active
# colour shifts from red (idx 0) to green (idx 1).
def is_red(rgb,   tol=40): return rgb[0] >= 140 and rgb[1] <= 80 and rgb[2] <= 80
def is_green(rgb, tol=40): return rgb[0] <= 80 and rgb[1] >= 140 and rgb[2] <= 80

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
    return data[o], data[o + 1], data[o + 2]


def sample_block(ppm, cx, cy, half=4):
    """Return the most-frequent pixel in a (2*half+1)^2 block around
    (cx, cy).  Stable against single-pixel anti-aliasing fringes."""
    counts = {}
    for dy in range(-half, half + 1):
        for dx in range(-half, half + 1):
            p = pixel_at(ppm, cx + dx, cy + dy)
            counts[p] = counts.get(p, 0) + 1
    return max(counts.items(), key=lambda kv: kv[1])[0]


def main():
    q = boot()
    rc = 0
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached")
            return 1
        print("PASS: shell prompt reached")
        time.sleep(0.5)

        # Summon the launcher: hidden at boot, click Start to
        # show.  This puts the launcher panel under our intended
        # paint-button click below.
        print(f"  clicking Start button at ({START_CX}, {START_CY}) "
              f"to summon launcher")
        click(qmp, START_CX, START_CY)
        wait_for(ser, b"start -> show launcher", 3.0)
        time.sleep(0.4)

        # ----------------------------------------------------------
        # BUG #3 — click the launcher's "paint" button.
        # ----------------------------------------------------------
        print(f"  clicking launcher's paint button at "
              f"({PAINT_BTN_X}, {PAINT_BTN_Y})")
        click(qmp, PAINT_BTN_X, PAINT_BTN_Y)
        log = wait_for(ser, b"[wmclient] window id=", 6.0)
        if b"[wmclient] window id=" not in log:
            print("FAIL: paint did not open")
            print(log[-1500:].decode("ascii", "replace"))
            return 1
        # Wait an extra moment for paint's initial render.
        time.sleep(1.0)

        # Move cursor far away so it doesn't pollute screenshots.
        move(qmp, 4, 4)
        time.sleep(0.3)

        # ----------------------------------------------------------
        # BUG #2a — colour indicator present on startup.
        # The default colour PALETTE[0] is red.  We sample the
        # top-right corner swatch position and look for red.
        # ----------------------------------------------------------
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        sw = sample_block(ppm, SWATCH_SX, SWATCH_SY, half=2)
        print(f"  initial swatch pixel @ ({SWATCH_SX},{SWATCH_SY}) = {sw}")
        if is_red(sw):
            print(f"BUG#2a PASS: initial colour swatch is visible (red)")
        else:
            print(f"BUG#2a FAIL: no red swatch at startup "
                  f"(pixel = {sw}) — colour indicator missing")
            rc = 1

        # ----------------------------------------------------------
        # BUG #2b — right-click cycles the active colour.
        # Move to canvas centre, right-click, then re-check swatch
        # AND check the right-click painted a green dot at that spot.
        # Actually right-click only changes colour — does NOT stamp
        # (per paint.c).  So just check the swatch flips to green.
        # ----------------------------------------------------------
        print(f"  right-clicking at ({RCLICK_X}, {RCLICK_Y})")
        click(qmp, RCLICK_X, RCLICK_Y, which="right")
        time.sleep(0.6)
        move(qmp, 4, 4)
        time.sleep(0.3)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        sw2 = sample_block(ppm, SWATCH_SX, SWATCH_SY, half=2)
        print(f"  post-rclick swatch pixel = {sw2}")
        if is_green(sw2):
            print(f"BUG#2b PASS: right-click cycled colour to green")
        else:
            print(f"BUG#2b FAIL: right-click did not change swatch "
                  f"(still {sw2}, expected green) — right-button events "
                  f"not reaching paint")
            rc = 1

        # ----------------------------------------------------------
        # BUG #1 / #3 — ESC should make paint exit.  paint logs
        # nothing on exit, but its destruction shows up in wsd's
        # client gc.  We assert by checking that AFTER ESC the
        # paint canvas no longer covers (SWATCH_SX, SWATCH_SY)
        # (becomes wallpaper or another window's pixel).
        # ----------------------------------------------------------
        print(f"  sending ESC")
        press_key(qmp, "esc")
        time.sleep(1.0)
        move(qmp, 4, 4)
        time.sleep(0.3)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        sw3 = sample_block(ppm, SWATCH_SX, SWATCH_SY, half=2)
        print(f"  post-ESC swatch pixel = {sw3}")
        # Paint canvas was light (0xF8, ~248); if paint exited the
        # swatch position is now wallpaper (chosen by user — varies),
        # but it should NOT still be the green/red swatch colour.
        if is_green(sw3) or is_red(sw3):
            print(f"BUG#1 FAIL: paint still alive after ESC "
                  f"(swatch still {sw3})")
            rc = 1
        else:
            print(f"BUG#1 PASS: paint exited on ESC "
                  f"(swatch pixel = {sw3})")

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

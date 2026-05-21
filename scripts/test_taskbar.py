#!/usr/bin/env python3
"""scripts/test_taskbar.py — milestone-47 smoke test.

Boots fully headless.  Verifies:
  1. Both /bin/taskbar and /bin/launcher auto-start.
  2. The taskbar logs window-create with NO_DECORATION + ALWAYS_ON_TOP
     flags.
  3. The taskbar's distinctive dark-blue BG is visible at the bottom
     of the framebuffer, and the green Start button is painted at
     its left edge.
  4. Spawning a real app (notepad) over serial makes a CELL appear
     in the taskbar at the expected position (just right of the
     Start button).
  5. Clicking the Start button summons the launcher panel (chapter
     109e UX: launcher is now a Start-menu-style panel, hidden at
     boot, anchored above the taskbar).

Chapter 109e notes: the launcher is no longer one of the taskbar's
cells — it lives in a NODECORATION + ALWAYS_ON_TOP window that the
taskbar's filter explicitly hides from its cell list (the launcher's
title is the literal string "launcher").  Summon/dismiss is the
Start button's job, not a cell click.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-tb.sock"
SERIAL_SOCK = "/tmp/osdev-serial-tb.sock"
DUMP_PATH   = "/tmp/osdev-fb-tb.ppm"

FB_W = 1280
FB_H = 800

# Taskbar / launcher geometry — see userspace/taskbar/taskbar.c
# and userspace/launcher/launcher.c.
BAR_H            = 28
START_BTN_X      = 8
START_BTN_Y_OFF  = 4
START_BTN_W      = 60
START_BTN_H      = BAR_H - 8
START_CX = START_BTN_X + START_BTN_W // 2                       # 38
START_CY = (FB_H - BAR_H) + START_BTN_Y_OFF + START_BTN_H // 2  # 786
CELLS_X0 = START_BTN_X + START_BTN_W + 8                         # 76

LAUNCHER_X = 0
LAUNCHER_Y = FB_H - BAR_H - 232                                  # 540
LAUNCHER_W = 240
LAUNCHER_H = 232

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

ABS_MAX = 0x7FFF
def screen_to_abs(x, y):
    return (int(x * ABS_MAX / FB_W), int(y * ABS_MAX / FB_H))

def qmp_move(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})

def qmp_btn(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": bool(down), "button": "left"}},
    ]}})

def qmp_click(qmp, x, y):
    qmp_move(qmp, x, y); time.sleep(0.05)
    qmp_btn(qmp, True);  time.sleep(0.05)
    qmp_btn(qmp, False)

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
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        if b"launching /bin/taskbar" not in boot_log:
            print("FAIL: init did not auto-spawn taskbar")
            return 1
        print("PASS: init auto-spawned /bin/taskbar")

        if b"launching /bin/launcher" not in boot_log:
            print("FAIL: init did not auto-spawn launcher")
            return 1
        print("PASS: init auto-spawned /bin/launcher")

        # The taskbar's create line should mention flags=0x3
        # (NO_DECORATION | ALWAYS_ON_TOP).  serial_puthex emits a
        # 16-digit hex word with leading zeros.
        more = drain(ser, time.time() + 1.0)
        boot_log += more
        flags_needle = b"flags=0x0000000000000003"
        if flags_needle not in boot_log:
            print("FAIL: no window with flags=0x3 created (decoration|always-on-top)")
            print("--- recent serial ---")
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: taskbar window created with flags=0x3")

        # Spawn a real app so the taskbar has at least one cell
        # to draw (the launcher itself is filtered out of the
        # cell list by taskbar.c).  notepad is the smallest GUI
        # app handy; spawn over serial so the focused launcher
        # (when it gets summoned later) doesn't swallow input.
        ser.sendall(b"notepad &\n")
        log = wait_for(ser, b"[wm] window created", 5.0)
        if b"[wm] window created" not in log:
            print("FAIL: notepad did not open a window")
            return 1
        # Wait for taskbar to refresh its cell list.
        time.sleep(0.8)

        screendump(qmp, DUMP_PATH)
        print(f"  saved screendump: {DUMP_PATH}")

        ppm = read_ppm(DUMP_PATH)
        # Bar BG colour at top of strip (y == FB_H - BAR_H + 8).
        # Bar Y range = 772..799.  Sample at y=775 (above any cell
        # but below the 1px highlight at y=772).  Sample x well
        # past every cell (1100 is safely in pure-BG territory).
        bar_bg = pixel_at(ppm, 1100, 775)
        if not near(bar_bg, (24, 28, 50), tol=10):
            print(f"FAIL: bar BG pixel at (1100,775) = {bar_bg}, "
                  f"expected ~(24, 28, 50)")
            return 1
        print(f"PASS: taskbar BG painted (pixel at (1100,775) = {bar_bg})")

        # Start button is painted at (8..68, 776..796).  Its idle
        # fill is START_BG_IDLE = GUI_BGRA(0x30, 0x60, 0x30) =
        # mid-green.  Sample at (38, 786) which is dead-centre of
        # the button — the glyph row for "Start" sits a few pixels
        # below the top, so x=38, y=786 lands on the fill.
        start_pix = pixel_at(ppm, START_CX, START_CY)
        if not near(start_pix, (48, 96, 48), tol=24):
            # The label glyph might land on this exact pixel
            # depending on the font; fall back to a wider search
            # for any green-dominant pixel in the button region.
            found_green = False
            for sy in range(START_CY - 4, START_CY + 5):
                for sx in range(START_CX - 8, START_CX + 9):
                    p = pixel_at(ppm, sx, sy)
                    if p[1] > p[0] + 20 and p[1] > p[2] + 20 and p[1] >= 60:
                        found_green = True; break
                if found_green: break
            if not found_green:
                print(f"FAIL: Start button green fill not visible "
                      f"near ({START_CX}, {START_CY}); sampled {start_pix}")
                return 1
            print(f"PASS: Start button green fill visible near "
                  f"({START_CX}, {START_CY})")
        else:
            print(f"PASS: Start button painted (pixel at "
                  f"({START_CX}, {START_CY}) = {start_pix})")

        # The first cell (notepad) sits at (CELLS_X0=76, bar_y=776),
        # 180x20.  Cell BG is CELL_BGRA = (48, 64, 112) when not
        # focused, or CELL_FOCUS_BGRA = (96, 144, 224) when focused.
        # Sample at a point safely inside the cell body to the
        # RIGHT of the label glyphs (label is 8×7 px wide left of
        # ~x=CELLS_X0+70).  CELLS_X0 + 100 = 176 lands well past
        # the "notepad" label, still inside the 180-wide cell.
        cell_x = CELLS_X0 + 100
        cell_pix = pixel_at(ppm, cell_x, 786)
        focus = (96, 144, 224)
        unfocus = (48, 64, 112)
        if near(cell_pix, focus, tol=15):
            print(f"PASS: notepad cell painted FOCUSED (pixel = {cell_pix})")
        elif near(cell_pix, unfocus, tol=15):
            print(f"PASS: notepad cell painted unfocused (pixel = {cell_pix})")
        else:
            print(f"FAIL: cell pixel at ({cell_x}, 786) = {cell_pix}, "
                  f"expected near {focus} or {unfocus}")
            return 1

        # Click the Start button to summon the launcher panel.
        print(f"  clicking Start button at ({START_CX}, {START_CY}) "
              f"to summon launcher")
        qmp_click(qmp, START_CX, START_CY)
        wait_for(ser, b"start -> show launcher", 3.0)
        time.sleep(0.4)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        # The launcher panel (NODECORATION) is now at
        # (LAUNCHER_X, LAUNCHER_Y) = (0, 540), size 240x232.
        # Its body is light-grey-blue (0xE8, 0xEC, 0xF0).
        # Sample at (110, LAUNCHER_Y + 6) -- inside the top
        # margin, above the first button at body-y=BTN_TOP=16,
        # so pure BG.
        launcher_bg = pixel_at(ppm, 110, LAUNCHER_Y + 6)
        if launcher_bg[0] < 220 or launcher_bg[1] < 220 or launcher_bg[2] < 220:
            print(f"FAIL: launcher panel did not appear after Start "
                  f"click — (110, {LAUNCHER_Y + 6}) = {launcher_bg}")
            return 1
        print(f"PASS: launcher panel summoned by Start button "
              f"(pixel at (110, {LAUNCHER_Y + 6}) = {launcher_bg})")

        print("\nMILESTONE 47: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

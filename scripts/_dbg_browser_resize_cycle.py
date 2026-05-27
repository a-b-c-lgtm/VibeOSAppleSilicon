#!/usr/bin/env python3
"""scripts/_dbg_browser_resize_cycle.py — chapter 118 follow-up #3
repro for the user-reported "shrink-then-grow leaves gray" bug.

The previous test (test_wsd_browser_resize.py) only checks the grow
case.  This script does a full shrink → grow-back cycle and
captures PPMs at each step for visual inspection.

Reproduces all three sub-bugs the user reported (May 2026):
  1. Grow:       gray area at bottom-right (not overpainted)
  2. Shrink:     content cut off (no relayout)
  3. Grow-back:  gray persists where content used to be

Captures /tmp/osdev-fb-cycle-{1..4}.ppm:
  1: initial
  2: after grow
  3: after shrink
  4: after grow-back
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-cycle.sock"
SERIAL_SOCK = "/tmp/osdev-serial-cycle.sock"
DUMP_BASE   = "/tmp/osdev-fb-cycle"

FB_W = 1280
FB_H = 1024

WIN_X, WIN_Y = 140, 140
TITLE_H      = 24
GRIP_SIZE    = 12

# WSD_DECO_BG_IDLE = 0xff556677 -> (R,G,B)=(0x55,0x66,0x77)
# but QEMU PPM is real RGB, and BGRA byte order means
# uint32 0xff556677 -> bytes in memory B=0x77, G=0x66, R=0x55, A=0xff
# So when displayed, R=0x55 G=0x66 B=0x77.
IDLE_RGB = (0x55, 0x66, 0x77)

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


def cursor_to(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})


def left_button(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": down, "button": "left"}}]}})


def drag(qmp, x0, y0, x1, y1, steps=8, settle=0.12):
    """Slow, deliberate drag.  Plenty of settle between cursor
    moves so wsd's input poller actually processes each step
    instead of coalescing 14 cursor moves into 2.  This better
    matches what a human user does with the mouse."""
    cursor_to(qmp, x0, y0); time.sleep(settle)
    left_button(qmp, True); time.sleep(settle)
    for i in range(1, steps + 1):
        ix = x0 + (x1 - x0) * i // steps
        iy = y0 + (y1 - y0) * i // steps
        cursor_to(qmp, ix, iy); time.sleep(settle)
    left_button(qmp, False); time.sleep(settle)


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
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
            time.sleep(0.1); break
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


def near(a, b, tol=20):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def is_white(rgb, tol=20):
    return all(c >= 255 - tol for c in rgb)


def is_idle(rgb, tol=20):
    return near(rgb, IDLE_RGB, tol)


def main():
    q = boot()
    rc = 0
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 25.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached")
            return 1
        print("PASS: shell prompt reached")

        # Open browser on the user's repro fixture.
        ser.sendall(b"browser --gui /mnt/test_layout.html\n")
        log = wait_for(ser, b"[browser] gui window=", 20.0)
        if b"[browser] gui window=" not in log:
            print("FAIL: browser did not open")
            return 1
        import re
        m = re.search(rb"\[browser\] gui window=\d+ size=(\d+)x(\d+)", log)
        if not m:
            print("FAIL: could not parse browser size")
            return 1
        WIN_W = int(m.group(1)); WIN_H = int(m.group(2))
        print(f"  browser opened {WIN_W}x{WIN_H}")
        time.sleep(2.0)

        # Move cursor away.
        cursor_to(qmp, 50, 50); time.sleep(0.2)

        # ─────────────────── Step 1: initial ─────────────────────
        screendump(qmp, f"{DUMP_BASE}-1-initial.ppm")
        ppm = read_ppm(f"{DUMP_BASE}-1-initial.ppm")
        # Sample a pixel deep inside the body.
        cx = WIN_X + WIN_W // 2
        cy = WIN_Y + TITLE_H + WIN_H // 2
        body_initial = pixel_at(ppm, cx, cy)
        # Sample at body bottom edge.
        be_x = WIN_X + WIN_W // 2
        be_y = WIN_Y + TITLE_H + WIN_H - 20
        body_bot_initial = pixel_at(ppm, be_x, be_y)
        print(f"  initial: body mid ({cx},{cy})={body_initial}  "
              f"body bot ({be_x},{be_y})={body_bot_initial}")

        # ───────────────── Step 2: grow ──────────────────────────
        grip_cx = WIN_X + WIN_W - GRIP_SIZE // 2
        grip_cy = WIN_Y + TITLE_H + WIN_H - GRIP_SIZE // 2
        new_grip_cx = min(grip_cx + 200, FB_W - 20)
        new_grip_cy = min(grip_cy + 120, FB_H - 20)
        print(f"  GROW: drag ({grip_cx},{grip_cy}) -> "
              f"({new_grip_cx},{new_grip_cy})")
        drag(qmp, grip_cx, grip_cy, new_grip_cx, new_grip_cy)
        time.sleep(2.5)  # let parser republish
        cursor_to(qmp, 50, 50); time.sleep(0.2)
        screendump(qmp, f"{DUMP_BASE}-2-grown.ppm")
        ppm = read_ppm(f"{DUMP_BASE}-2-grown.ppm")
        GROWN_W = WIN_W + (new_grip_cx - grip_cx)
        GROWN_H = WIN_H + (new_grip_cy - grip_cy)
        # Sample in the new bottom-right region.
        sx = WIN_X + WIN_W + 50
        sy = WIN_Y + TITLE_H + WIN_H + 50
        grown_corner = pixel_at(ppm, sx, sy)
        # Sample at the grown-area edge (just inside new boundary).
        gex = WIN_X + GROWN_W - 30
        gey = WIN_Y + TITLE_H + GROWN_H - 30
        grown_edge = pixel_at(ppm, gex, gey)
        print(f"  after GROW: corner ({sx},{sy})={grown_corner}  "
              f"edge ({gex},{gey})={grown_edge}")
        if is_idle(grown_corner):
            print(f"  BUG#1 REPRO: grown region is IDLE placeholder gray "
                  f"(expected: white page bg)")
            rc = 1
        elif is_white(grown_corner):
            print(f"  GROW OK: grown region is page_bg (white)")
        else:
            print(f"  GROW: grown region has unexpected color {grown_corner}")

        # ───────────────── Step 3: shrink ────────────────────────
        # Grip is now at new position.
        grip_cx2 = WIN_X + GROWN_W - GRIP_SIZE // 2
        grip_cy2 = WIN_Y + TITLE_H + GROWN_H - GRIP_SIZE // 2
        # Shrink down to LESS than original (smaller).
        shrink_grip_cx = WIN_X + (WIN_W // 2)
        shrink_grip_cy = WIN_Y + TITLE_H + (WIN_H // 2)
        print(f"  SHRINK: drag ({grip_cx2},{grip_cy2}) -> "
              f"({shrink_grip_cx},{shrink_grip_cy})")
        drag(qmp, grip_cx2, grip_cy2, shrink_grip_cx, shrink_grip_cy)
        SHRUNK_W = shrink_grip_cx - WIN_X
        SHRUNK_H = shrink_grip_cy - (WIN_Y + TITLE_H)
        time.sleep(2.5)
        cursor_to(qmp, 50, 50); time.sleep(0.2)
        screendump(qmp, f"{DUMP_BASE}-3-shrunk.ppm")
        ppm = read_ppm(f"{DUMP_BASE}-3-shrunk.ppm")
        # Sample inside the now-smaller body (should be page bg, white).
        sm_x = WIN_X + SHRUNK_W // 2
        sm_y = WIN_Y + TITLE_H + SHRUNK_H // 2
        shrunk_body = pixel_at(ppm, sm_x, sm_y)
        # Outside the shrunk window — should be wallpaper.
        out_x = WIN_X + SHRUNK_W + 50
        out_y = WIN_Y + TITLE_H + SHRUNK_H // 2
        outside = pixel_at(ppm, out_x, out_y) if out_x < FB_W else None
        print(f"  after SHRINK: body ({sm_x},{sm_y})={shrunk_body}  "
              f"outside ({out_x},{out_y})={outside}")

        # ───────────────── Step 4: grow back ─────────────────────
        # Compute new grip pos (window shrunk so it's somewhere inside).
        grip_cx3 = WIN_X + SHRUNK_W - GRIP_SIZE // 2
        grip_cy3 = WIN_Y + TITLE_H + SHRUNK_H - GRIP_SIZE // 2
        # Grow back to original WIN_W x WIN_H.
        back_grip_cx = WIN_X + WIN_W - GRIP_SIZE // 2
        back_grip_cy = WIN_Y + TITLE_H + WIN_H - GRIP_SIZE // 2
        print(f"  GROW-BACK: drag ({grip_cx3},{grip_cy3}) -> "
              f"({back_grip_cx},{back_grip_cy})")
        drag(qmp, grip_cx3, grip_cy3, back_grip_cx, back_grip_cy)
        time.sleep(3.0)
        cursor_to(qmp, 50, 50); time.sleep(0.2)
        screendump(qmp, f"{DUMP_BASE}-4-grown-back.ppm")
        ppm = read_ppm(f"{DUMP_BASE}-4-grown-back.ppm")
        # Should look like initial: white body filling whole window.
        body_mid = pixel_at(ppm, cx, cy)
        body_bot = pixel_at(ppm, be_x, be_y)
        # Also sample bottom-right grown-back area.
        gb_x = WIN_X + WIN_W - 50
        gb_y = WIN_Y + TITLE_H + WIN_H - 50
        gb_corner = pixel_at(ppm, gb_x, gb_y)
        print(f"  after GROW-BACK: mid ({cx},{cy})={body_mid}  "
              f"bot ({be_x},{be_y})={body_bot}  corner ({gb_x},{gb_y})="
              f"{gb_corner}")
        if is_idle(gb_corner):
            print(f"  BUG#3 REPRO: grow-back leaves IDLE gray where "
                  f"content used to be")
            rc = 1
        elif is_white(gb_corner):
            print(f"  GROW-BACK OK: corner is page_bg (white)")
        else:
            print(f"  GROW-BACK: corner has unexpected color {gb_corner}")

        # ─── Drain serial for any browser/wsd log clues ───
        log_late = drain(ser, time.time() + 0.5)
        if log_late:
            tail = log_late.decode("ascii", "replace")
            interesting = [l for l in tail.split("\n")
                          if any(k in l for k in ("browser:", "[browser]",
                              "[wsd]", "[wmclient]", "RESIZE", "remap",
                              "relayout"))]
            if interesting:
                print("  --- serial tail ---")
                for l in interesting[-30:]:
                    print(f"    {l}")

        print(f"\n  PPMs written: {DUMP_BASE}-1..4.ppm")
        print("DONE" if rc == 0 else "DONE (BUGS REPRODUCED)")
        return rc
    finally:
        try: q.kill()
        except Exception: pass
        try: q.wait(timeout=2)
        except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

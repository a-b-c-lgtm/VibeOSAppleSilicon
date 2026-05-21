#!/usr/bin/env python3
"""scripts/_dbg_browser_rapid_resize.py — chapter 108e follow-up #4

Reproduces the user-reported bug from May 2026:
  "Resize works a lot of the time, but frequent resizing
   can lead to the browser window becoming transparent
   except for the title bar and the resize anchor."

Strategy: drag the resize grip back and forth RAPIDLY, faster
than the browser's render thread can process GUI_EVENT_RESIZE.
Then sample inside the body and check whether the body shows
wallpaper (transparent) instead of page background.

We deliberately use short settle times between cursor moves to
flood wsd's input ring with motion events, forcing many
sys_win_fb_resize calls in quick succession.

Captures /tmp/osdev-fb-rapid-*.ppm for visual inspection.
"""
import json, os, select, socket, subprocess, sys, time, re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-rapid.sock"
SERIAL_SOCK = "/tmp/osdev-serial-rapid.sock"
DUMP_BASE   = "/tmp/osdev-fb-rapid"

FB_W = 1280
FB_H = 1024

WIN_X, WIN_Y = 140, 140
TITLE_H      = 24
GRIP_SIZE    = 12

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


def drag_rapid(qmp, x0, y0, x1, y1, steps=20, settle=0.02):
    """Fast drag — flood wsd's input ring."""
    cursor_to(qmp, x0, y0); time.sleep(0.05)
    left_button(qmp, True); time.sleep(0.05)
    for i in range(1, steps + 1):
        ix = x0 + (x1 - x0) * i // steps
        iy = y0 + (y1 - y0) * i // steps
        cursor_to(qmp, ix, iy); time.sleep(settle)
    left_button(qmp, False); time.sleep(0.05)


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
        assert magic == b"P6"
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


def main():
    q = boot()
    rc = 0
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 25.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell prompt reached")

        ser.sendall(b"browser --gui /mnt/hn.html\n")
        log = wait_for(ser, b"[browser] gui window=", 25.0)
        if b"[browser] gui window=" not in log:
            print("FAIL: browser did not open"); return 1
        m = re.search(rb"\[browser\] gui window=\d+ size=(\d+)x(\d+)", log)
        WIN_W = int(m.group(1)); WIN_H = int(m.group(2))
        print(f"  browser opened {WIN_W}x{WIN_H}")
        time.sleep(2.5)

        cursor_to(qmp, 50, 50); time.sleep(0.2)

        # initial dump for reference
        screendump(qmp, f"{DUMP_BASE}-0.ppm")

        # Do N rapid back-and-forth resize cycles. We track
        # `cur_w/h` and grow / shrink in fixed steps so each
        # drag actually changes the dims (a no-op drag where
        # start==end produces no sys_win_fb_resize call).
        cur_w, cur_h = WIN_W, WIN_H
        STEP_W, STEP_H = 120, 80
        for cycle in range(8):
            print(f"  ===== cycle {cycle}  (current {cur_w}x{cur_h}) =====")
            grip_cx = WIN_X + cur_w - GRIP_SIZE // 2
            grip_cy = WIN_Y + TITLE_H + cur_h - GRIP_SIZE // 2
            # grow
            new_cx = min(grip_cx + STEP_W, FB_W - 20)
            new_cy = min(grip_cy + STEP_H, FB_H - 20)
            real_dx = new_cx - grip_cx
            real_dy = new_cy - grip_cy
            print(f"  GROW: drag ({grip_cx},{grip_cy}) -> ({new_cx},{new_cy})")
            drag_rapid(qmp, grip_cx, grip_cy, new_cx, new_cy, steps=12, settle=0.015)
            cur_w += real_dx
            cur_h += real_dy
            # do NOT settle; immediately drag back

            grip_cx = WIN_X + cur_w - GRIP_SIZE // 2
            grip_cy = WIN_Y + TITLE_H + cur_h - GRIP_SIZE // 2
            # shrink
            new_cx = max(grip_cx - STEP_W, WIN_X + 200)
            new_cy = max(grip_cy - STEP_H, WIN_Y + TITLE_H + 100)
            real_dx = new_cx - grip_cx
            real_dy = new_cy - grip_cy
            print(f"  SHRINK: drag ({grip_cx},{grip_cy}) -> ({new_cx},{new_cy})")
            drag_rapid(qmp, grip_cx, grip_cy, new_cx, new_cy, steps=12, settle=0.015)
            cur_w += real_dx
            cur_h += real_dy

        # Let everything settle.
        print(f"  final size {cur_w}x{cur_h}; waiting 4s for parser")
        time.sleep(4.0)
        cursor_to(qmp, 50, 50); time.sleep(0.2)
        screendump(qmp, f"{DUMP_BASE}-final.ppm")
        ppm = read_ppm(f"{DUMP_BASE}-final.ppm")

        # Sample body pixels. Wallpaper at these (interior) positions
        # would be the bug.
        samples = [
            (WIN_X + cur_w // 2, WIN_Y + TITLE_H + cur_h // 2),
            (WIN_X + cur_w // 4, WIN_Y + TITLE_H + cur_h // 4),
            (WIN_X + 100,        WIN_Y + TITLE_H + 100),
            (WIN_X + cur_w - 50, WIN_Y + TITLE_H + cur_h // 2),
        ]
        # Wallpaper sample for reference.
        wp_x = min(FB_W - 50, WIN_X + cur_w + 100)
        wp_y = WIN_Y + 100
        if wp_x < FB_W:
            wp = pixel_at(ppm, wp_x, wp_y)
            print(f"  wallpaper at ({wp_x},{wp_y}) = {wp}")
        else:
            wp = None
        for (sx, sy) in samples:
            if sx >= FB_W or sy >= FB_H:
                print(f"  skip sample ({sx},{sy}) — off-screen")
                continue
            px = pixel_at(ppm, sx, sy)
            tag = ""
            if wp is not None and near(px, wp, tol=20):
                tag = "  <-- MATCHES WALLPAPER (BUG)"
                rc = 1
            print(f"  body  at ({sx},{sy}) = {px}{tag}")

        # Serial for any FATAL or recycle.
        tail = drain(ser, time.time() + 0.5)
        full = boot_log + log + tail
        # save full serial for forensic inspection
        with open("/tmp/osdev-fb-rapid-serial.log", "wb") as f:
            f.write(full)
        print("  full serial -> /tmp/osdev-fb-rapid-serial.log")
        for line in full.split(b"\n"):
            if (b"FATAL" in line or b"recycled" in line
                or b"reinstall FAILED" in line
                or b"resize id=" in line):
                print(f"  serial: {line.decode('ascii','replace').strip()}")

        if rc == 0:
            print("PASS: body opaque after rapid resize")
        else:
            print("FAIL: body shows wallpaper after rapid resize")
    finally:
        try: q.terminate(); q.wait(2.0)
        except Exception:
            try: q.kill()
            except Exception: pass
        cleanup()
    return rc


if __name__ == "__main__":
    sys.exit(main())

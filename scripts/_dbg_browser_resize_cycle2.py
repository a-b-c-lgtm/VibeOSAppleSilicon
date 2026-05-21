#!/usr/bin/env python3
"""scripts/_dbg_browser_resize_cycle2.py — full cycle with precise grip clicks
and post-drag PPM inspection.

Uses the wsd log line "[wsd] resize end win-slot=N final=WxH" to learn
the actual final size after each drag, so the next drag's grip click
lands correctly.
"""
import json, os, re, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-rsz2.sock"
SERIAL_SOCK = "/tmp/osdev-serial-rsz2.sock"
DUMP_BASE   = "/tmp/osdev-fb-rsz2"

FB_W = 1280
FB_H = 1024
WIN_X, WIN_Y = 140, 140
TITLE_H = 24
GRIP_SIZE = 12
ABS_MAX = 0x7FFF

IDLE_RGB = (0x55, 0x66, 0x77)


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


def cursor_to(qmp, x, y):
    ax = int(x * ABS_MAX / FB_W); ay = int(y * ABS_MAX / FB_H)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})


def left_button(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": down, "button": "left"}}]}})


def drag(qmp, x0, y0, x1, y1, steps=10, settle=0.04):
    cursor_to(qmp, x0, y0); time.sleep(settle)
    left_button(qmp, True); time.sleep(settle)
    for i in range(1, steps + 1):
        ix = x0 + (x1 - x0) * i // steps
        iy = y0 + (y1 - y0) * i // steps
        cursor_to(qmp, ix, iy); time.sleep(settle)
    left_button(qmp, False); time.sleep(settle)


SERIAL_BUF = []
SERIAL_RUN = True


def serial_pump(ser):
    while SERIAL_RUN:
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            try:
                c = ser.recv(8192)
            except OSError:
                return
            if not c:
                return
            SERIAL_BUF.append(c.decode("ascii", "replace"))


def all_serial():
    return "".join(SERIAL_BUF)


def last_final_size():
    """Returns the last (w, h) from a '[wsd] resize end ... final=WxH' line."""
    matches = re.findall(r"resize end win-slot=\d+ final=(\d+)x(\d+)",
                         all_serial())
    if not matches: return None
    return (int(matches[-1][0]), int(matches[-1][1]))


def screendump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {"execute": "screendump", "arguments": {"filename": path}})
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.15); break
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


def is_white(rgb, tol=20):
    return all(c >= 255 - tol for c in rgb)


def is_idle(rgb, tol=20):
    return all(abs(int(a)-int(b)) <= tol for a, b in zip(rgb, IDLE_RGB))


def grip_center(w, h):
    """Center of the WxH grip given current window dims w/h."""
    gx = WIN_X + w - GRIP_SIZE // 2
    gy = WIN_Y + TITLE_H + h - GRIP_SIZE // 2
    return (gx, gy)


def main():
    q = boot()
    rc = 0
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        pump = threading.Thread(target=serial_pump, args=(ser,), daemon=True)
        pump.start()

        # Wait for shell.
        for _ in range(50):
            time.sleep(0.5)
            if "$ " in all_serial():
                break
        print("=== shell up ===")
        time.sleep(0.5)

        ser.sendall(b"browser --gui /mnt/hn.html\n")
        for _ in range(40):
            time.sleep(0.5)
            if "[browser] gui window=" in all_serial():
                break
        m = re.search(r"\[browser\] gui window=\d+ size=(\d+)x(\d+)",
                      all_serial())
        if not m:
            print("FAIL no browser size"); return 1
        WIN_W, WIN_H = int(m.group(1)), int(m.group(2))
        print(f"=== browser opened {WIN_W}x{WIN_H} ===")
        time.sleep(2.5)
        cursor_to(qmp, 50, 50); time.sleep(0.3)

        cur_w, cur_h = WIN_W, WIN_H

        def step(name, drag_to, expect_callback):
            nonlocal cur_w, cur_h, rc
            gx, gy = grip_center(cur_w, cur_h)
            print(f"=== {name}: drag grip ({gx},{gy}) -> {drag_to} "
                  f"(from {cur_w}x{cur_h}) ===")
            drag(qmp, gx, gy, drag_to[0], drag_to[1])
            # Wait for the [wsd] resize end line to confirm the drag landed.
            deadline = time.time() + 3.0
            while time.time() < deadline:
                fs = last_final_size()
                if fs and (fs[0] != cur_w or fs[1] != cur_h):
                    cur_w, cur_h = fs
                    break
                time.sleep(0.05)
            print(f"  wsd reports final {cur_w}x{cur_h}")
            # Settle for repaint / relayout.
            time.sleep(8.0)
            cursor_to(qmp, 30, 30); time.sleep(0.3)
            ppm_path = f"{DUMP_BASE}-{name}.ppm"
            screendump(qmp, ppm_path)
            ppm = read_ppm(ppm_path)
            expect_callback(ppm, cur_w, cur_h)

        # ── Initial ──
        screendump(qmp, f"{DUMP_BASE}-0-initial.ppm")
        ppm0 = read_ppm(f"{DUMP_BASE}-0-initial.ppm")
        print(f"  initial body pixel at ({WIN_X+200},{WIN_Y+TITLE_H+200}) = "
              f"{pixel_at(ppm0, WIN_X+200, WIN_Y+TITLE_H+200)}")

        # ── GROW ──
        def check_grow(ppm, w, h):
            nonlocal rc
            # Sample near new bottom-right corner (inside the window body).
            sx = WIN_X + w - 50
            sy = WIN_Y + TITLE_H + h - 50
            p = pixel_at(ppm, sx, sy)
            print(f"  GROW: bottom-right ({sx},{sy}) = {p}")
            if is_idle(p):
                print(f"  >>> GROW BUG: bottom-right shows IDLE placeholder")
                rc = 1
            elif is_white(p):
                print(f"  GROW OK: white page bg in grown area")
            else:
                print(f"  GROW: unexpected color {p}")

        step("1-grown",
             (WIN_X + WIN_W + 200, WIN_Y + TITLE_H + WIN_H + 120),
             check_grow)

        # ── SHRINK ──
        def check_shrink(ppm, w, h):
            sx = WIN_X + w // 2
            sy = WIN_Y + TITLE_H + h // 2
            p = pixel_at(ppm, sx, sy)
            print(f"  SHRINK: middle ({sx},{sy}) = {p}")
            # Outside the now-smaller window should be wallpaper.
            ox = WIN_X + w + 50
            oy = WIN_Y + TITLE_H + h - 50
            if ox < FB_W and oy < FB_H:
                outside = pixel_at(ppm, ox, oy)
                print(f"  SHRINK: outside ({ox},{oy}) = {outside}")

        step("2-shrunk", (WIN_X + 250, WIN_Y + TITLE_H + 200), check_shrink)

        # ── GROW-BACK to original size ──
        def check_grow_back(ppm, w, h):
            nonlocal rc
            # Sample in the area that was hidden during shrink.
            # Use multiple samples to characterize the bug.
            samples = [
                (WIN_X + 350, WIN_Y + TITLE_H + 250),
                (WIN_X + 500, WIN_Y + TITLE_H + 400),
                (WIN_X + 600, WIN_Y + TITLE_H + 500),
                (WIN_X + w - 50, WIN_Y + TITLE_H + h - 50),
            ]
            any_idle = False
            for sx, sy in samples:
                if sx >= FB_W or sy >= FB_H: continue
                p = pixel_at(ppm, sx, sy)
                tag = ("IDLE" if is_idle(p) else
                       "WHITE" if is_white(p) else "?")
                print(f"  GROW-BACK sample ({sx},{sy}) = {p} [{tag}]")
                if is_idle(p): any_idle = True
            if any_idle:
                print(f"  >>> GROW-BACK BUG: at least one IDLE-gray "
                      f"pixel in grown-back body")
                rc = 1

        step("3-grown-back",
             (WIN_X + WIN_W + 200, WIN_Y + TITLE_H + WIN_H + 120),
             check_grow_back)

        print(f"\n  PPMs in {DUMP_BASE}-*.ppm")
        print("DONE" if rc == 0 else "DONE (BUGS REPRODUCED)")
        return rc
    finally:
        global SERIAL_RUN
        SERIAL_RUN = False
        try: q.kill()
        except Exception: pass
        try: q.wait(timeout=2)
        except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

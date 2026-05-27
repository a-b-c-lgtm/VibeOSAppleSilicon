#!/usr/bin/env python3
"""scripts/test_wsd_bugs.py — chapter 118 follow-up bug #1.

Regression test for the wallpaper-raise bug the user reported
against the wsd chapter-109 work:

  **Wallpaper raises on background click.**  The desktop's
  wallpaper window is a fullscreen NO_DECORATION window
  created first (so it lives at the bottom of g_z_order).
  Clicking on any "background" pixel (i.e. a wallpaper pixel
  not occluded by a foreground window) should NOT raise the
  wallpaper above the foreground windows.  Without the
  PIN_BOTTOM fix, wsd's body-click handler unconditionally
  called z_raise even on NO_DECORATION windows, which locked
  the user out of every other app.

The bug #2 (back window's title bar painting over front
window's body) and bug #3 (browser-style resize leaving the
grown region black) have their own dedicated test scripts:

  - scripts/test_wsd_bar_overlap.py
  - scripts/test_wsd_browser_resize.py
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-wsdbugs.sock"
SERIAL_SOCK = "/tmp/osdev-serial-wsdbugs.sock"
DUMP_PATH   = "/tmp/osdev-fb-wsdbugs.ppm"

FB_W = 1280
FB_H = 800

# Notepad opens at the cascade origin (140, 140) when launcher is
# already at (100, 100); 720x440 body + 24px bar.
WIN_X, WIN_Y = 140, 140
WIN_W, WIN_H = 720, 440
TITLE_H      = 24

# Notepad's editor background = GUI_BGRA(0xF8,0xF8,0xF0).
NOTEPAD_BG = (0xF8, 0xF8, 0xF0)

# wsd title-bar bg active = WSD_DECO_BG_ACTIVE = 0xff3a6ea5
#   -> R=0xa5, G=0x6e, B=0x3a  (steel blue)
# idle = WSD_DECO_BG_IDLE = 0xff556677
#   -> R=0x77, G=0x66, B=0x55  (dim gray-blue)
TITLE_BG_ACTIVE = (0xa5, 0x6e, 0x3a)
TITLE_BG_IDLE   = (0x77, 0x66, 0x55)

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


def left_click(qmp, x, y, settle=0.10):
    cursor_to(qmp, x, y); time.sleep(settle)
    left_button(qmp, True); time.sleep(settle)
    left_button(qmp, False); time.sleep(settle)


def drag(qmp, x0, y0, x1, y1, steps=10, settle=0.04):
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


def near(a, b, tol=10):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def is_black(rgb, tol=12):
    return all(c <= tol for c in rgb)


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
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # Spawn notepad — opens at cascade origin (140, 140) with
        # body 720x440 and a 24-px title bar above.
        ser.sendall(b"notepad /tmp/wsdbugs.txt\n")
        log = wait_for(ser, b"[wm] window created", 6.0)
        if b"[wm] window created" not in log:
            print("FAIL: notepad did not open")
            print(log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: notepad window opened")
        time.sleep(0.8)

        # ----------------------------------------------------------
        # BUG #1 — wallpaper raises on background click.
        # ----------------------------------------------------------
        # Sample a notepad body pixel BEFORE the background click.
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        body_sx = WIN_X + 50
        body_sy = WIN_Y + TITLE_H + 50
        pre_body = pixel_at(ppm, body_sx, body_sy)
        if not near(pre_body, NOTEPAD_BG, tol=15):
            print(f"FAIL: notepad body not visible before bg click "
                  f"({body_sx},{body_sy}) = {pre_body}")
            return 1
        print(f"PASS: notepad body visible pre-bg-click ({pre_body})")

        # Click on a wallpaper pixel that is NOT covered by any
        # known window.  Notepad ends at x=140+720=860, taskbar
        # starts around y=FB_H-28=772.  Click at (1000, 600).
        BG_CX, BG_CY = 1000, 600
        print(f"  clicking background at ({BG_CX},{BG_CY})")
        left_click(qmp, BG_CX, BG_CY)
        time.sleep(0.6)   # let wsd compose + cursor settle

        # Move cursor away from the notepad body sample so the
        # cursor sprite doesn't pollute the read.
        cursor_to(qmp, 10, 10); time.sleep(0.4)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        post_body = pixel_at(ppm, body_sx, body_sy)
        if not near(post_body, NOTEPAD_BG, tol=15):
            print(f"BUG#1 FAIL: notepad body covered after bg click "
                  f"({body_sx},{body_sy}) = {post_body} — "
                  f"wallpaper raised on top")
            rc = 1
        else:
            print(f"BUG#1 PASS: notepad body still visible post-bg-click "
                  f"({post_body})")

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

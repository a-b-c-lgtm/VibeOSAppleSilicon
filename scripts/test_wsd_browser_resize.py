#!/usr/bin/env python3
"""scripts/test_wsd_browser_resize.py — chapter 108e bug #3.

Exercise the asynchronous-repaint resize path that fails for
notepad-style tests.  Browser handles GUI_EVENT_RESIZE by:

  1. wm_window_remap_fb (cheap, in-loop)
  2. Update viewport, request_relayout on the parser thread
  3. Set s.dirty so the NEXT loop iteration paints (but with
     the OLD paint buffer until the parser publishes)

This means: when the user drags the grip continuously, every
poll iteration of wsd's input thread triggers a kernel resize,
which zeros the new pages.  Until the browser's parser thread
finishes and publishes a paint buffer big enough for the new
viewport, the grown region of the FB is all zeros (black) --
even though browser HAS marked itself dirty, because each new
resize re-zeros the pages.

The fix: in wsd's resize_apply, fill the GROWN region of the
new FB with a neutral colour (WSD_DECO_BG_IDLE, the unfocused
title gray-blue) BEFORE delivering GUI_EVENT_RESIZE.  This
gives the user a visual "the window is growing" feedback in
the interim instead of a black flash.  Once the client paints
its real content, the placeholder is overwritten.

Test:
  1. Boot, spawn browser via `browser --gui /mnt/test_layout.html`
  2. Verify browser body is white (page bg)
  3. Drag browser's grip down-right with several intermediate
     positions so multiple SYS_WIN_FB_RESIZE calls happen
  4. Sample the GROWN region:
       - BUG: black (0, 0, 0)
       - FIX: WSD_DECO_BG_IDLE (= R 0x77, G 0x66, B 0x55) OR
              browser-painted (page bg, ~white) if the parser
              finished within the settle time
     ANY non-black pixel passes; black fails.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-brrsz.sock"
SERIAL_SOCK = "/tmp/osdev-serial-brrsz.sock"
DUMP_PATH   = "/tmp/osdev-fb-brrsz.ppm"

FB_W = 1280
FB_H = 1024   # taller than default 800 so the browser's grip
              # at WIN_Y + TITLE_H + WIN_H (~140+24+720=884) is
              # comfortably on-screen for the drag.  The user's
              # original bug repro was on a desktop with enough
              # vertical room that the grip was reachable.

# Browser opens at cascade origin (140, 140) when launcher is at
# (100, 100).  Default viewport=800, height=720 per browser.c
# (BR_GUI_DEFAULT_H).  Grip is at bottom-right of body.
WIN_X, WIN_Y = 140, 140
WIN_W, WIN_H = 800, 720
TITLE_H      = 24
GRIP_SIZE    = 12

# WSD_DECO_BG_IDLE = 0xff556677 -> (R,G,B)=(0x77,0x66,0x55)
IDLE_RGB = (0x77, 0x66, 0x55)

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


def drag(qmp, x0, y0, x1, y1, steps=12, settle=0.05):
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


def is_black(rgb, tol=10):
    return all(c <= tol for c in rgb)


def near(a, b, tol=15):
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
            print("FAIL: shell prompt not reached")
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # Spawn browser on the largest fixture we have (~35 KB)
        # so the parser thread takes long enough to expose the
        # async-repaint bug.  Smaller pages (test_layout.html ~3
        # KB) finish the relayout inside one wsd tick and never
        # show the black flash.
        ser.sendall(b"browser --gui /mnt/hn.html\n")
        log = wait_for(ser, b"[browser] gui window=", 20.0)
        if b"[browser] gui window=" not in log:
            print("FAIL: browser did not open")
            print(log[-1500:].decode("ascii", "replace"))
            return 1
        # Parse "[browser] gui window=<id> size=<W>x<H> ..."
        import re
        m = re.search(rb"\[browser\] gui window=\d+ size=(\d+)x(\d+)", log)
        if not m:
            print("FAIL: could not parse browser size")
            return 1
        global WIN_W, WIN_H, WIN_X, WIN_Y
        WIN_W = int(m.group(1)); WIN_H = int(m.group(2))
        print(f"PASS: browser window opened ({WIN_W}x{WIN_H})")
        # Give the parser thread time to publish the first
        # paint buffer.
        time.sleep(2.0)

        # Pre-resize: browser body should be white (page bg).
        cursor_to(qmp, 1200, 700); time.sleep(0.3)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        SX = WIN_X + 200
        SY = WIN_Y + TITLE_H + 120
        pre = pixel_at(ppm, SX, SY)
        # Loose check: any non-black, non-WSD-deco pixel is fine.
        # White page bg = (255, 255, 255).
        if is_black(pre):
            print(f"FAIL: browser body at ({SX},{SY}) is black "
                  f"pre-resize -- browser failed to paint at all")
            return 1
        print(f"PASS: browser body painted pre-resize ({pre})")

        # Drag the grip down-right.  Cap the target to stay
        # well inside the FB (max scanout = FB_W x FB_H).
        # New grip position must satisfy
        #   new_grip_x + 1 < FB_W  and  new_grip_y + 1 < FB_H
        # so the cursor stays on-screen for QMP abs events.
        grip_cx = WIN_X + WIN_W - GRIP_SIZE // 2
        grip_cy = WIN_Y + TITLE_H + WIN_H - GRIP_SIZE // 2
        # Aim for +200 grow but clamp to leave 20 px margin.
        new_grip_cx = min(grip_cx + 200, FB_W - 20)
        new_grip_cy = min(grip_cy + 120, FB_H - 20)
        if new_grip_cx <= grip_cx or new_grip_cy <= grip_cy:
            print(f"FAIL: no room to grow window past "
                  f"({grip_cx},{grip_cy}) inside {FB_W}x{FB_H}")
            return 1
        grow_dx = new_grip_cx - grip_cx
        grow_dy = new_grip_cy - grip_cy
        print(f"  drag browser grip ({grip_cx},{grip_cy}) -> "
              f"({new_grip_cx},{new_grip_cy})  (+{grow_dx},+{grow_dy})")
        drag(qmp, grip_cx, grip_cy, new_grip_cx, new_grip_cy,
             steps=16, settle=0.04)
        # IMMEDIATELY (no settle) sample the grown region -- this
        # is the worst case for the bug.  The browser's parser
        # thread is still relayouting; the FB has the wsd
        # placeholder fill (with our fix) or all zeros (without).
        # Move cursor away first so the sprite doesn't pollute
        # the sample.
        cursor_to(qmp, 50, 50); time.sleep(0.05)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        # Sample in the GROWN region.  The window grew by
        # +grow_dx wide and +grow_dy tall.  Sample inside the
        # new right-strip (x past the OLD right edge, y inside
        # the OLD-or-NEW height range) and the new bottom-strip
        # similarly.  Right-strip centre:
        if grow_dx >= 20:
            GX = WIN_X + WIN_W + max(8, grow_dx // 2)
            GY = WIN_Y + TITLE_H + min(WIN_H // 2, 200)
        else:
            GX = WIN_X + WIN_W // 2
            GY = WIN_Y + TITLE_H + WIN_H + max(8, grow_dy // 2)
        immediately = pixel_at(ppm, GX, GY)
        print(f"  grown region immediately post-drag at ({GX},{GY}) = "
              f"{immediately}")
        if is_black(immediately):
            print(f"BUG#3 FAIL: grown region is BLACK immediately after "
                  f"drag -- wsd composed zero-filled new pages before "
                  f"client repaint")
            rc = 1
        else:
            ok = near(immediately, IDLE_RGB, tol=20) or not is_black(immediately)
            if near(immediately, IDLE_RGB, tol=20):
                print(f"BUG#3 PASS: grown region shows wsd placeholder "
                      f"({immediately}) ~ IDLE colour")
            else:
                print(f"BUG#3 PASS: grown region shows client repaint "
                      f"({immediately}); parser was fast enough")

        # Settle and re-sample -- after the parser finishes, the
        # grown region should be the browser's page bg (white)
        # since the placeholder gets overwritten by the next
        # browser render.  We don't fail if the placeholder
        # persists (a real bug if so, but separate from #3) --
        # just print.
        time.sleep(3.0)
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        settled = pixel_at(ppm, GX, GY)
        if is_black(settled):
            print(f"FAIL: grown region still BLACK after 3 s settle "
                  f"({GX},{GY}) = {settled} -- browser repaint never landed")
            rc = 1
        elif near(settled, IDLE_RGB, tol=20):
            print(f"WARN: grown region still placeholder after 3 s settle "
                  f"({settled}) -- browser repaint never landed but "
                  f"placeholder is preventing the black flash")
        else:
            print(f"PASS: grown region shows client content after settle "
                  f"({settled})")

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

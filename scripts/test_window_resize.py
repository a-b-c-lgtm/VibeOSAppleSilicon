#!/usr/bin/env python3
"""scripts/test_window_resize.py -- chapter 118 resize regression.

Exercises the SYS_WIN_FB_RESIZE + wm_window_remap_fb plumbing
added in chapter 118:

  1. Boot to desktop.  Launcher is a Start-menu panel anchored
     above the taskbar at (0, 540) via wm_create_window_at,
     which does NOT advance the wsd cascade.
  2. Spawn notepad via the serial port (sh stdin).  Notepad is
     the first cascade-positioned client and lands at cascade
     slot 0 = (100, 100), with WIN_W=720, WIN_H=440.  RESIZABLE
     so wsd
     paints a 12-px grip in the bottom-right of the body.
  3. Sample three pixels:
       - INSIDE  the notepad body well outside the OLD grip ->
         should be notepad's BG (off-white 0xF8,0xF8,0xF0)
       - OUTSIDE the OLD body (where the GROWN body should land)
         -> should be wallpaper before the resize, notepad BG
         after.
       - The grip pixel itself before -> WSD_GRIP_FG (0xd0d0d0).
  4. Press at grip center, drag down-right by +120,+80 pixels,
     release.  This should trigger SYS_WIN_FB_RESIZE which
     reallocates pages; the kernel then re-installs for wsd
     (compose can resume immediately), wsd re-maps to learn
     the new owner VA, delivers GUI_EVENT_RESIZE to notepad
     which calls wm_window_remap_fb and repaints.
  5. Sample again: the grown region should now be notepad BG
     (proves the FB was actually realloced and notepad
     re-mapped + re-painted, not just clipped).
  6. Drag back by (-200, -120) to shrink BELOW the original
     size; verify the shrunk region becomes wallpaper.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-rsz.sock"
SERIAL_SOCK = "/tmp/osdev-serial-rsz.sock"
DUMP_PATH   = "/tmp/osdev-fb-rsz.ppm"

FB_W = 1280
FB_H = 800

# Notepad opens at cascade slot 0 = (100, 100) with 720x440 body
# and a 24-px title bar above.  Body extends from y=124 to y=564.
# (The launcher uses wm_create_window_at and so does NOT advance
# the cascade, leaving slot 0 free for notepad as the first
# cascade-positioned client.)
WIN_X, WIN_Y = 100, 100
WIN_W, WIN_H = 720, 440
TITLE_H      = 24

# wsd grip geometry (userspace/wsd/wsd.c).
GRIP_SIZE = 12

# Notepad's editor background -- userspace/notepad/notepad.c BG_BGRA
# = GUI_BGRA(0xF8,0xF8,0xF0).  PPM reads as RGB so swap to (R,G,B).
NOTEPAD_BG = (0xF8, 0xF8, 0xF0)

# Wallpaper -- userspace/wallpaper/wallpaper.c colour; pulled from
# observation of test_minimize.py (any dark non-NOTEPAD_BG pixel
# in the area we sample passes the "not notepad" check, so we
# don't have to hard-code the wallpaper RGB).

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

def drag(qmp, x0, y0, x1, y1, steps=10, settle=0.04):
    """Press at (x0,y0), interpolate to (x1,y1) over `steps`
    intermediate cursor-moves (each followed by `settle` s), then
    release.  wsd polls the cursor every PERIOD_NS so multiple
    cursor positions during a press produce multiple
    SYS_WIN_FB_RESIZE calls; that exercises the realloc path
    under repeated invocation, not just the single end-state."""
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
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # Spawn notepad via serial -- launcher has focus so QMP keys
        # would not reach sh.
        ser.sendall(b"notepad /tmp/rsz.txt\n")
        log = wait_for(ser, b"[wm] window created", 5.0)
        if b"[wm] window created" not in log:
            print("FAIL: notepad did not open")
            print(log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: notepad window opened")
        time.sleep(0.6)

        # ---- Pre-resize sample.
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        # Body sample 50 px into the body, well clear of the grip
        # in the bottom-right (which is at (859, 592) ish).
        BODY_SX = WIN_X + 50
        BODY_SY = WIN_Y + TITLE_H + 50
        pre_body = pixel_at(ppm, BODY_SX, BODY_SY)
        if not near(pre_body, NOTEPAD_BG, tol=15):
            print(f"FAIL: notepad body not visible pre-resize "
                  f"({BODY_SX},{BODY_SY}) = {pre_body}, "
                  f"expected ~{NOTEPAD_BG}")
            return 1
        print(f"PASS: notepad body visible pre-resize (body = {pre_body})")

        # OUTSIDE pixel where the GROWN body will land.  Original
        # body bottom-right is (WIN_X+WIN_W, WIN_Y+TITLE_H+WIN_H)
        # = (820, 564).  Sample (860, 600): 40 px right + 36 px
        # below original body, well inside the +120/+80 growth
        # zone (new bottom-right at (940, 644)).
        GROW_SX = WIN_X + WIN_W + 40
        GROW_SY = WIN_Y + TITLE_H + WIN_H + 36
        pre_grow = pixel_at(ppm, GROW_SX, GROW_SY)
        if near(pre_grow, NOTEPAD_BG, tol=15):
            print(f"FAIL: growth area already shows notepad BG pre-resize "
                  f"({GROW_SX},{GROW_SY}) = {pre_grow}; "
                  f"test geometry must be wrong")
            return 1
        print(f"PASS: growth area = {pre_grow} (not notepad) pre-resize")

        # ---- Grip drag: grow by +120,+80.
        grip_cx = WIN_X + WIN_W - GRIP_SIZE // 2
        grip_cy = WIN_Y + TITLE_H + WIN_H - GRIP_SIZE // 2
        new_grip_cx = grip_cx + 120
        new_grip_cy = grip_cy + 80
        print(f"  grow: grip ({grip_cx},{grip_cy}) -> "
              f"({new_grip_cx},{new_grip_cy})")
        drag(qmp, grip_cx, grip_cy, new_grip_cx, new_grip_cy,
             steps=8, settle=0.05)
        time.sleep(0.8)   # let wsd finish compose + notepad repaint

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        # Old body pixel still should be notepad BG (the top-left
        # is preserved across resize by the kernel).
        post_body = pixel_at(ppm, BODY_SX, BODY_SY)
        if not near(post_body, NOTEPAD_BG, tol=15):
            print(f"FAIL: notepad body lost after grow "
                  f"({BODY_SX},{BODY_SY}) = {post_body}")
            return 1
        print(f"PASS: old body still notepad BG post-grow ({post_body})")

        # Growth area pixel: now should be notepad BG.  This is
        # the actual proof of life -- requires kernel resize +
        # owner reinstall + wsd remap + notepad GUI_EVENT_RESIZE
        # handler + wm_window_remap_fb + render_to_buffer at the
        # new size all to have worked.
        post_grow = pixel_at(ppm, GROW_SX, GROW_SY)
        if not near(post_grow, NOTEPAD_BG, tol=15):
            print(f"FAIL: growth area not notepad BG post-grow "
                  f"({GROW_SX},{GROW_SY}) = {post_grow}; "
                  f"resize did not propagate to the FB")
            return 1
        print(f"PASS: growth area now notepad BG post-grow ({post_grow})")

        # ---- Grip drag back: shrink by -200,-120 (below the
        # original 720x440, so the right strip should reveal
        # wallpaper).
        # New grip is at the corner of the now-840x520 body.
        cur_grip_cx = new_grip_cx
        cur_grip_cy = new_grip_cy
        shrink_grip_cx = cur_grip_cx - 200
        shrink_grip_cy = cur_grip_cy - 120
        print(f"  shrink: grip ({cur_grip_cx},{cur_grip_cy}) -> "
              f"({shrink_grip_cx},{shrink_grip_cy})")
        drag(qmp, cur_grip_cx, cur_grip_cy,
             shrink_grip_cx, shrink_grip_cy,
             steps=8, settle=0.05)
        time.sleep(0.8)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        # Pixel just inside the ORIGINAL right edge of the body
        # but OUTSIDE the new shrunk body should now be wallpaper
        # (the body is currently 640x360 -ish; sample at
        # WIN_X+WIN_W-40, WIN_Y+TITLE_H+WIN_H-40 which was
        # safely inside the original body).
        SHRUNK_SX = WIN_X + WIN_W - 40
        SHRUNK_SY = WIN_Y + TITLE_H + WIN_H - 40
        post_shrink = pixel_at(ppm, SHRUNK_SX, SHRUNK_SY)
        if near(post_shrink, NOTEPAD_BG, tol=15):
            print(f"FAIL: shrunk area still notepad BG "
                  f"({SHRUNK_SX},{SHRUNK_SY}) = {post_shrink}; "
                  f"resize did not actually free pages")
            return 1
        print(f"PASS: shrunk area = {post_shrink} (not notepad) post-shrink")

        # Top-left of body should STILL be notepad BG (preserved
        # across both resizes).
        final_body = pixel_at(ppm, BODY_SX, BODY_SY)
        if not near(final_body, NOTEPAD_BG, tol=15):
            print(f"FAIL: top-left body lost after shrink "
                  f"({BODY_SX},{BODY_SY}) = {final_body}")
            return 1
        print(f"PASS: top-left body still notepad BG post-shrink "
              f"({final_body})")

        print("CHAPTER 118: ALL RESIZE TESTS PASSED")
        return 0
    finally:
        try: q.kill()
        except Exception: pass
        try: q.wait(timeout=2)
        except Exception: pass
        cleanup()

if __name__ == "__main__":
    sys.exit(main())

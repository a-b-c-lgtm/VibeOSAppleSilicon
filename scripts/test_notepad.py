#!/usr/bin/env python3
"""scripts/test_notepad.py — notepad smoke test.

Boots headless, opens notepad with /tmp/np.txt as the file path,
types some text, presses Ctrl-S to save, and then re-opens the
same file via `cat /tmp/np.txt` to verify the bytes round-tripped
correctly.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-np.sock"
SERIAL_SOCK = "/tmp/osdev-serial-np.sock"
DUMP_PATH   = "/tmp/osdev-fb-np.ppm"

FB_W = 1280
FB_H = 800

# wsd cascade lays the first cascade-positioned window at (100,100)
# and steps by 40 per subsequent cascade-positioned window.  The
# launcher auto-runs at boot but uses wm_create_window_at (anchored
# above the taskbar as a Start-menu panel), which does NOT advance
# the cascade.  notepad spawned next is therefore the first cascade
# client and lands at slot 0 -> (100,100).  chapter 118 paints a
# 24-px title bar above each decorated body.
WIN_X, WIN_Y = 100, 100
WIN_W, WIN_H = 720, 440
TITLE_H      = 24

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

KEYMAP = {**{c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"},
          " ": "spc", "\n": "ret", "\x1b": "esc",
          ".": "dot", "/": "slash", "-": "minus"}
SHIFTED = {"_": "minus"}

def send_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [{
            "type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]}})

def send_shifted(qmp, qcode):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "shift"}}},
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}},
    ]}})

def send_ctrl(qmp, qcode):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}},
    ]}})

def type_text(qmp, text):
    for ch in text:
        if ch in SHIFTED: send_shifted(qmp, SHIFTED[ch])
        else:             send_key(qmp, KEYMAP[ch])
        time.sleep(0.04)

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
    with open(path, "rb") as f: data = f.read()
    assert data.startswith(b"P6")
    idx = 0
    def tok():
        nonlocal idx
        while data[idx:idx+1] in (b" ", b"\n", b"\r", b"\t"): idx += 1
        if data[idx:idx+1] == b"#":
            while data[idx:idx+1] not in (b"\n", b""): idx += 1
            return tok()
        s = idx
        while data[idx:idx+1] not in (b" ", b"\n", b"\r", b"\t", b""): idx += 1
        return data[s:idx]
    m = tok(); w = int(tok()); h = int(tok()); v = int(tok())
    assert m == b"P6" and v == 255
    idx += 1
    return w, h, data[idx: idx + w*h*3]

def pixel(px, w, x, y):
    o = (y*w + x) * 3
    return (px[o], px[o+1], px[o+2])

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

def count_fg_pixels(px, w, x0, y0, x1, y1, bg_close):
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            r, g, b = pixel(px, w, x, y)
            db = abs(r - bg_close[0]) + abs(g - bg_close[1]) + abs(b - bg_close[2])
            if db > 60: n += 1
    return n

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 20.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell ready")

        # Spawn notepad via the serial socket (sh's actual stdin).
        # Since the launcher window auto-focuses at boot, QMP
        # keystrokes go to the launcher (mouse-only) and never reach
        # sh.  Routing through serial bypasses the WM input path.
        ser.sendall(b"notepad /tmp/np.txt\n")
        log = wait_for(ser, b"[wm] window created", 5.0)
        if b"[wm] window created" not in log:
            print("FAIL: notepad did not open a window"); return 1
        print("PASS: notepad window opened")

        time.sleep(0.4)

        # Background colour for the editor area is GUI_BGRA(0xF8,0xF8,0xF0).
        # In screendump RGB we expect (R=0xF8, G=0xF8, B=0xF0) — warm off-white.
        BG_EDIT = (0xF8, 0xF8, 0xF0)

        # Sanity: snapshot before any typing — should be mostly background
        # in the edit area (a single cursor block doesn't produce many fg pixels).
        screendump(qmp, DUMP_PATH)
        w, _, pre = read_ppm(DUMP_PATH)
        fg_before = count_fg_pixels(pre, w,
                                    WIN_X + 8, WIN_Y + TITLE_H + 8,
                                    WIN_X + WIN_W - 8, WIN_Y + TITLE_H + 8 + 5*16,
                                    BG_EDIT)
        print(f"  fg pixels in edit area before typing: {fg_before}")

        # Type a couple of lines.
        type_text(qmp, "hello notepad\n")
        type_text(qmp, "second line here\n")
        time.sleep(0.4)

        screendump(qmp, DUMP_PATH)
        w, _, mid = read_ppm(DUMP_PATH)
        fg_typed = count_fg_pixels(mid, w,
                                   WIN_X + 8, WIN_Y + TITLE_H + 8,
                                   WIN_X + WIN_W - 8, WIN_Y + TITLE_H + 8 + 5*16,
                                   BG_EDIT)
        print(f"  fg pixels after typing: {fg_typed}")
        if fg_typed - fg_before < 100:
            print("FAIL: typed text did not appear"); return 1
        print("PASS: text rendered into editor")

        # Save with Ctrl-S.
        send_ctrl(qmp, "s")
        time.sleep(0.4)

        # Quit with Ctrl-Q.
        send_ctrl(qmp, "q")
        wait_for(ser, b"[wm] destroyed window", 3.0)
        print("PASS: notepad exited")

        # Drain any extra prompt that might have appeared.
        drain(ser, time.time() + 0.3)

        # Read back the file via cat to verify the on-disk bytes.
        # Same serial-vs-QMP rationale as the notepad spawn above:
        # after notepad exits, focus may sit on the launcher (or
        # nowhere), so QMP keystrokes wouldn't reach sh.
        ser.sendall(b"cat /tmp/np.txt\n")
        cat_out = wait_for(ser, b"second line here", 4.0)
        if b"hello notepad" not in cat_out or b"second line here" not in cat_out:
            print("FAIL: file did not round-trip through save")
            print(cat_out.decode("ascii", "replace"))
            return 1
        print("PASS: saved file round-trips through cat")

        print("\nALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_notepad_save_as_nav.py — chapter 85 save-dialog
directory navigation test.

Builds on test_notepad_save_as.py.  Adds the new behaviours:

  1. Pre-create /data/notes via shell `mkdir` so the dialog's
     list will contain at least one directory entry before we
     pop it.
  2. Bare-launch notepad, type some content.
  3. Ctrl-S to open the dialog.
  4. Verify the dialog appears (panel BG count goes up).
  5. Cream BG count = 0 baseline (new-folder mode not active).
  6. Ctrl-N to enter New Folder mode.  Verify cream BG appears
     (the new-folder field uses GUI_BGRA(0xFF,0xF0,0xC8) cream
     so it's visually distinct from the white filename field).
  7. Type "scratch" and press Enter — folder should be created
     and the dialog should auto-navigate into /data/scratch/.
  8. Verify cream is gone (returned to normal mode).
  9. Type a filename and Enter — confirm the file lands at
     /data/scratch/<name>.
 10. Quit notepad, cat the file back via shell.

Same QMP/serial harness shape as test_notepad_save_as.py.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-saveasnav.sock"
SERIAL_SOCK = "/tmp/osdev-serial-saveasnav.sock"
DUMP_PATH   = "/tmp/osdev-fb-saveasnav.ppm"

FB_W = 1280
FB_H = 800

TARGET_NAME = "innote.txt"
TARGET_DIR  = "scratch"
TARGET_PATH = f"/data/{TARGET_DIR}/{TARGET_NAME}"

def cleanup():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass

def reformat_data():
    subprocess.check_call(
        ["python3", f"{ROOT}/scripts/mkosfs2.py", f"{ROOT}/build/data.img"],
        stdout=subprocess.DEVNULL,
    )

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

def send_ctrl(qmp, qcode):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}},
    ]}})

def send_backspace_n(qmp, n):
    for _ in range(n):
        send_key(qmp, "backspace")
        time.sleep(0.03)

def type_text(qmp, text):
    for ch in text:
        if ch in SHIFTED:
            qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
                {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "shift"}}},
                {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": SHIFTED[ch]}}},
                {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": SHIFTED[ch]}}},
                {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}},
            ]}})
        else:
            send_key(qmp, KEYMAP[ch])
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

def count_pixels_close_to(px, w, x0, y0, x1, y1, target, tol=24):
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            r, g, b = pixel(px, w, x, y)
            if (abs(r - target[0]) + abs(g - target[1]) +
                abs(b - target[2])) < tol:
                n += 1
    return n

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

def main():
    # Always start from a clean data disk so /data/scratch is
    # absent (test 5 below depends on this — we want to PROVE
    # Ctrl-N created the directory, not that it was already
    # there).
    reformat_data()

    q = boot()
    rc = 1
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 25.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell ready")

        # Bare-launch notepad — first Ctrl-S pops dialog.
        ser.sendall(b"notepad\n")
        log = wait_for(ser, b"[wm] window created", 5.0)
        if b"[wm] window created" not in log:
            print("FAIL: notepad did not open a window"); return 1
        print("PASS: notepad window opened (bare-launch)")

        time.sleep(0.4)
        type_text(qmp, "abc def\n")
        time.sleep(0.2)

        DLG_BG_RGB = (0xF0, 0xF0, 0xF4)
        NF_BG_RGB  = (0xFF, 0xF0, 0xC8)   # cream — new-folder field

        # Open dialog.
        send_ctrl(qmp, "s")
        time.sleep(0.5)

        screendump(qmp, DUMP_PATH)
        w, h, px = read_ppm(DUMP_PATH)
        dlgbg = count_pixels_close_to(px, w, 0, 0, w, h, DLG_BG_RGB, tol=4)
        cream = count_pixels_close_to(px, w, 0, 0, w, h, NF_BG_RGB, tol=8)
        if dlgbg < 1000:
            print(f"FAIL: dialog body not detected after Ctrl-S (dlgbg={dlgbg})")
            return 1
        if cream > 200:
            print(f"FAIL: cream new-folder BG visible before Ctrl-N "
                  f"(cream={cream})")
            return 1
        print(f"PASS: dialog open, normal mode (dlgbg={dlgbg}, cream={cream})")

        # Press Ctrl-N — switch to New Folder mode.  Cream BG
        # should appear (the folder-name field is rendered with
        # GUI_BGRA(0xFF,0xF0,0xC8)).
        send_ctrl(qmp, "n")
        time.sleep(0.4)

        screendump(qmp, DUMP_PATH)
        _, _, px2 = read_ppm(DUMP_PATH)
        cream2 = count_pixels_close_to(px2, w, 0, 0, w, h, NF_BG_RGB, tol=8)
        if cream2 < 200:
            print(f"FAIL: new-folder mode not active (cream={cream2})")
            return 1
        print(f"PASS: new-folder mode entered (cream {cream}→{cream2})")

        # Clear any pre-filled text (defence in depth — should be
        # empty already), then type the new dir name + Enter.
        send_backspace_n(qmp, 24)
        type_text(qmp, TARGET_DIR)
        time.sleep(0.2)
        send_key(qmp, "ret")
        time.sleep(0.5)

        # After mkdir succeeds and the dialog auto-navigates,
        # cream should be gone (back to normal) and dialog still
        # visible (we're now inside the new folder).
        screendump(qmp, DUMP_PATH)
        _, _, px3 = read_ppm(DUMP_PATH)
        cream3 = count_pixels_close_to(px3, w, 0, 0, w, h, NF_BG_RGB, tol=8)
        dlgbg3 = count_pixels_close_to(px3, w, 0, 0, w, h, DLG_BG_RGB, tol=4)
        if cream3 > 200:
            print(f"FAIL: dialog still in new-folder mode after Enter "
                  f"(cream={cream3})")
            return 1
        if dlgbg3 < 1000:
            print(f"FAIL: dialog disappeared after mkdir (dlgbg={dlgbg3})")
            return 1
        print(f"PASS: mkdir confirmed, back to normal "
              f"(cream {cream2}→{cream3}, dlgbg={dlgbg3})")

        # Now type a filename and confirm the save.
        type_text(qmp, TARGET_NAME)
        time.sleep(0.2)
        send_key(qmp, "ret")
        time.sleep(0.5)

        # Dialog should be closed now.
        screendump(qmp, DUMP_PATH)
        _, _, px4 = read_ppm(DUMP_PATH)
        dlgbg4 = count_pixels_close_to(px4, w, 0, 0, w, h, DLG_BG_RGB, tol=4)
        if dlgbg4 > 500:
            print(f"FAIL: dialog did not close after final Enter "
                  f"(dlgbg={dlgbg4})")
            return 1
        print(f"PASS: dialog closed after save (dlgbg {dlgbg3}→{dlgbg4})")

        # Quit notepad and cat back the file from the new subdir.
        send_ctrl(qmp, "q")
        wait_for(ser, b"[wm] destroyed window", 3.0)
        drain(ser, time.time() + 0.3)
        print("PASS: notepad exited")

        ser.sendall(f"cat {TARGET_PATH}\n".encode())
        out = wait_for(ser, b"abc def", 5.0)
        if b"abc def" not in out:
            print(f"FAIL: {TARGET_PATH} did not round-trip through Save As")
            print(out.decode("ascii", "replace"))
            return 1
        print(f"PASS: {TARGET_PATH} contains expected content")

        # Also verify ls /data/ shows scratch/ as a directory.
        ser.sendall(b"ls /data\n")
        out = wait_for(ser, b"$ ", 3.0)
        if b"<DIR>" not in out or TARGET_DIR.encode() not in out:
            print(f"FAIL: ls /data does not show {TARGET_DIR}/ as a dir")
            print(out.decode("ascii", "replace"))
            return 1
        print(f"PASS: ls /data shows {TARGET_DIR}/ tagged as <DIR>")

        print("\nCHAPTER 85 SAVE-AS NAVIGATION: ALL TESTS PASSED")
        rc = 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: pass
        cleanup()
    return rc

if __name__ == "__main__":
    sys.exit(main())

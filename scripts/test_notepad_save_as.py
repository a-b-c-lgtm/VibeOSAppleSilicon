#!/usr/bin/env python3
"""scripts/test_notepad_save_as.py — chapter-84 smoke test.

Exercises the "Save As" path:

  1. Boots the system (RW data disk attached).
  2. Spawns `notepad` with NO argv — bare-launch sets
     g_path_chosen = 0, so the first Ctrl-S pops the dialog.
  3. Types a couple of lines of text.
  4. Hits Ctrl-S — confirms the dialog appears (panel pixels
     show up in the centre of the window).
  5. Types a filename (replacing the pre-filled "untitled.txt"),
     then Enter.
  6. Quits notepad.
  7. Reads back /data/<chosen> via `cat` to verify the file
     reached disk under the chosen name.

The dialog's blocking event loop lives in
userspace/libgui/save_dialog.c — this is the test that proves
the library extraction didn't break the contract.

Also exercises an OSFS-2 round-trip: save a fresh file, read it
back from a different process, confirm bytes match.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-saveas.sock"
SERIAL_SOCK = "/tmp/osdev-serial-saveas.sock"
DUMP_PATH   = "/tmp/osdev-fb-saveas.ppm"

FB_W = 1280
FB_H = 800

# Notepad geometry (from userspace/notepad/notepad.c).
WIN_X, WIN_Y = 80, 60
WIN_W, WIN_H = 720, 440
TITLE_H      = 24

# Dialog detection is coordinate-agnostic: the WM auto-cascades
# new windows by 32 px per spawn, so notepad's actual on-screen
# (x, y) drifts depending on what else has been spawned (taskbar,
# launcher, etc.).  Rather than chase that, the test counts
# distinct dialog colours across the whole screen.

# Where save_file writes (chosen new name lives under /data/).
TARGET_NAME = "saveas_test.txt"
TARGET_PATH = f"/data/{TARGET_NAME}"

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

def send_backspace_n(qmp, n):
    for _ in range(n):
        send_key(qmp, "backspace")
        time.sleep(0.03)

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

def colour_match(px, w, x, y, target, tol=20):
    r, g, b = pixel(px, w, x, y)
    return (abs(r - target[0]) + abs(g - target[1]) + abs(b - target[2])) < tol

def count_pixels_close_to(px, w, x0, y0, x1, y1, target, tol=24):
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            r, g, b = pixel(px, w, x, y)
            if (abs(r - target[0]) + abs(g - target[1]) +
                abs(b - target[2])) < tol:
                n += 1
    return n

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 25.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell ready")

        # Make sure no leftover from a previous run is on the data
        # disk (the test re-uses /data/saveas_test.txt each run).
        ser.sendall(b"rm /data/saveas_test.txt\n")
        time.sleep(0.3)
        drain(ser, time.time() + 0.3)

        # Bare-launch notepad — no argv.  This sets g_path_chosen=0
        # so the first Ctrl-S will pop the Save As dialog.
        ser.sendall(b"notepad\n")
        log = wait_for(ser, b"[wm] window created", 5.0)
        if b"[wm] window created" not in log:
            print("FAIL: notepad did not open a window"); return 1
        print("PASS: notepad window opened (bare-launch)")

        time.sleep(0.4)

        # Type some content.
        type_text(qmp, "hello save as\n")
        type_text(qmp, "second line\n")
        time.sleep(0.3)

        # Sanity: dialog NOT visible yet.  We discriminate the
        # dialog from the rest of the screen by its panel BG
        # colour GUI_BGRA(0xF0,0xF0,0xF4) — a very slight cool
        # tint of off-white that doesn't appear in any other UI
        # element (editor BG is warm 0xF8/0xF8/0xF0, the WM
        # frame is brighter blue, etc).
        #
        # The dialog frame is GUI_BGRA(0x30,0x40,0x70) navy, but
        # the taskbar cell BG happens to use the same colour, so
        # we DON'T use raw navy count as a primary detector —
        # instead we verify that navy *increases substantially*
        # between pre- and post-Ctrl-S.
        DLG_BG_RGB    = (0xF0, 0xF0, 0xF4)
        DLG_FRAME_RGB = (0x30, 0x40, 0x70)
        EDIT_BG_RGB   = (0xF8, 0xF8, 0xF0)

        screendump(qmp, DUMP_PATH)
        w_pre, h_pre, pre = read_ppm(DUMP_PATH)
        navy_pre = count_pixels_close_to(
            pre, w_pre, 0, 0, w_pre, h_pre,
            DLG_FRAME_RGB, tol=10)
        dlgbg_pre = count_pixels_close_to(
            pre, w_pre, 0, 0, w_pre, h_pre,
            DLG_BG_RGB, tol=4)
        if dlgbg_pre > 200:
            print(f"FAIL: dialog BG colour present BEFORE Ctrl-S "
                  f"(dlgbg={dlgbg_pre}, navy={navy_pre})")
            return 1
        print(f"PASS: dialog not yet visible "
              f"(dlgbg={dlgbg_pre}, navy_baseline={navy_pre})")

        # Now Ctrl-S — should open the dialog.
        send_ctrl(qmp, "s")
        time.sleep(0.5)

        screendump(qmp, DUMP_PATH)
        w_dlg, h_dlg, dlg_frame = read_ppm(DUMP_PATH)

        navy_post = count_pixels_close_to(
            dlg_frame, w_dlg, 0, 0, w_dlg, h_dlg,
            DLG_FRAME_RGB, tol=10)
        dlgbg_post = count_pixels_close_to(
            dlg_frame, w_dlg, 0, 0, w_dlg, h_dlg,
            DLG_BG_RGB, tol=4)
        if dlgbg_post < 1000:
            print(f"FAIL: dialog body BG not detected after Ctrl-S "
                  f"(dlgbg_post={dlgbg_post}, was {dlgbg_pre})")
            return 1
        # Navy should also jump appreciably (frame + title bar
        # add ~10 000 pixels of dialog frame colour on top of the
        # baseline taskbar cell pixels).
        if navy_post - navy_pre < 2000:
            print(f"FAIL: dialog frame colour didn't grow enough "
                  f"(navy {navy_pre}→{navy_post})")
            return 1
        print(f"PASS: dialog opened "
              f"(navy {navy_pre}→{navy_post}, "
              f"dlgbg {dlgbg_pre}→{dlgbg_post})")

        # The field is pre-filled with "untitled.txt" (12 chars).
        # We want to replace it with TARGET_NAME.  Since the
        # cursor is at the end of the pre-filled field, send 12
        # backspaces, then type the new name.
        send_backspace_n(qmp, 12)
        time.sleep(0.3)
        # Now type "saveas_test.txt" (uses the underscore which
        # is a shifted minus — KEYMAP/SHIFTED already covers it).
        type_text(qmp, TARGET_NAME)
        time.sleep(0.3)

        # Confirm — Enter.
        send_key(qmp, "ret")
        time.sleep(0.5)

        # The dialog should now be closed.  As before, only the
        # dialog-BG colour is reliable as a presence detector
        # (the navy frame colour is shared with the taskbar
        # cell).
        screendump(qmp, DUMP_PATH)
        w_post, h_post, post = read_ppm(DUMP_PATH)
        dlgbg_after = count_pixels_close_to(
            post, w_post, 0, 0, w_post, h_post,
            DLG_BG_RGB, tol=4)
        if dlgbg_after > 500:
            print(f"FAIL: dialog did not close after Enter "
                  f"(dlgbg_after={dlgbg_after})")
            return 1
        print(f"PASS: dialog closed after Enter "
              f"(dlgbg {dlgbg_post}→{dlgbg_after})")

        # Quit the editor.
        send_ctrl(qmp, "q")
        wait_for(ser, b"[wm] destroyed window", 3.0)
        print("PASS: notepad exited")
        drain(ser, time.time() + 0.3)

        # Read the file back via cat — proves it landed on disk
        # under the chosen path (and therefore that the dialog's
        # out_path was used by save_file).
        ser.sendall(f"cat {TARGET_PATH}\n".encode())
        cat_out = wait_for(ser, b"second line", 5.0)
        if (b"hello save as" not in cat_out or
            b"second line" not in cat_out):
            print(f"FAIL: {TARGET_PATH} did not round-trip through Save As")
            print(cat_out.decode("ascii", "replace"))
            return 1
        print(f"PASS: {TARGET_PATH} contains expected content")

        # Tidy up so the next run starts clean.
        ser.sendall(f"rm {TARGET_PATH}\n".encode())
        time.sleep(0.3)
        drain(ser, time.time() + 0.3)

        print("\nCHAPTER 84 SAVE-AS DIALOG: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: pass
        cleanup()

if __name__ == "__main__":
    sys.exit(main())

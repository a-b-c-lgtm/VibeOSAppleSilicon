#!/usr/bin/env python3
"""scripts/_dbg_browser_term_input.py — repro for the post-resize
gui_term-input regression.

User report (verbatim):
    "If I start the gui browser and resize the window and then
     open up the gui terminal, the terminal doesn't accept any
     input. Other applications like notepad and paint still work
     as expected."

Strategy: drive the desktop with QMP mouse + keyboard, exactly
the way a user does.  The kernel sh has no `&` background
support, so we cannot script this from serial; the launcher GUI
is the only way to spawn three windows from outside.

Sequence:
  1. Boot GUI -smp 2.  Wait for the launcher window to appear
     in the kernel log.
  2. Click "browser" button on the launcher
     (launcher cascade slot 0 → window at (80,60); body at
     (84,84); button #3 "browser" centred at (204,250)).
  3. Wait for [browser] gui window= line.
  4. Move the browser by dragging its title bar to (700,500),
     so the launcher (at (80,60)..(320,324)) is fully exposed.
  5. Drag the (now-relocated) browser's resize grip to a
     different size — this is THE step that triggers the bug
     per the user's report.  Skipped under --no-resize.
  6. Click "gui_term" button on the launcher
     (button #0, centred at screen (204,118)).
  7. Wait for the new [wm] window created line.
  8. Type 'echo zzzblah\\n' via QMP keyboard.  gui_term gets
     focus on create (wm_create_window_ex sets g_focus_id), so
     the keys go to gui_term, which forwards them to its child
     /bin/sh.  Sh echoes the typed line to its pty, gui_term
     reads & renders it inside its window.
  9. Screendump.  Count fg-text-coloured pixels inside the
     gui_term body region.  Lots of bright pixels = PASS.
     Empty body = the bug.

Debug helper (leading underscore = excluded from nightly sweep).
Keep forever per /memories/debug-scripts-policy."""
import json, os, re, select, socket, subprocess, sys, time

ROOT        = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-brterm.sock"
SERIAL_SOCK = "/tmp/osdev-serial-brterm.sock"

FB_W, FB_H = 1280, 800
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
        "-qmp",    f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", f"virtio-gpu-device,xres={FB_W},yres={FB_H}",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn(path, timeout=10.0):
    deadline = time.time() + timeout
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


def drain(s, until_t):
    out = b""
    while time.time() < until_t:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
        else:
            break
    return out


def wait_after(s, prior, needle, timeout):
    """Wait for needle in NEW data after prior cutoff."""
    if isinstance(needle, str): needle = needle.encode()
    cutoff = len(prior)
    deadline = time.time() + timeout
    buf = bytearray(prior)
    while time.time() < deadline:
        if needle in buf[cutoff:]:
            return True, bytes(buf)
        r,_,_ = select.select([s],[],[],0.2)
        if r:
            c = s.recv(8192)
            if not c: break
            buf.extend(c)
    return needle in buf[cutoff:], bytes(buf)


def screen_to_abs(x, y):
    return (int(x * ABS_MAX / FB_W), int(y * ABS_MAX / FB_H))


def move(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})


def button(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": bool(down), "button": "left"}},
    ]}})


def click(qmp, x, y):
    move(qmp, x, y); time.sleep(0.10)
    button(qmp, True); time.sleep(0.10)
    button(qmp, False); time.sleep(0.30)


def drag(qmp, x0, y0, x1, y1, steps=10):
    move(qmp, x0, y0); time.sleep(0.15)
    button(qmp, True); time.sleep(0.15)
    for i in range(1, steps + 1):
        tx = x0 + (x1 - x0) * i // steps
        ty = y0 + (y1 - y0) * i // steps
        move(qmp, tx, ty); time.sleep(0.06)
    button(qmp, False); time.sleep(0.40)


KEYMAP = {**{c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"},
          " ": "spc", "\n": "ret", "\x1b": "esc"}


def send_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"down": down,
                "key": {"type": "qcode", "data": qcode}}},
        ]}})


def type_text(qmp, text):
    for ch in text:
        if ch in KEYMAP:
            send_key(qmp, KEYMAP[ch])
        else:
            print(f"WARN: no keymap for {ch!r}", file=sys.stderr)
        time.sleep(0.06)


# ---- Geometry of the launcher window ---------------------------
# Launcher uses GUI_WIN_POS_AUTO -> cascade slot 0 -> (80, 60).
# WIN_W=240, WIN_H=232.  Decoration: 4-px border + 24-px title.
# Body origin in screen coords = (84, 84).  Buttons (BTN_X=16,
# BTN_W=208, BTN_H=36, BTN_GAP=8, BTN_TOP=16):
#   slot 0 gui_term : body y = 16..52, screen centre (204, 118)
#   slot 1 paint    : body y = 60..96, screen centre (204, 162)
#   slot 2 notepad  : body y = 104..140, screen centre (204, 206)
#   slot 3 browser  : body y = 148..184, screen centre (204, 250)
LAUNCH_BTN = {
    "gui_term": (204, 118),
    "paint":    (204, 162),
    "notepad":  (204, 206),
    "browser":  (204, 250),
}


def main():
    skip_resize = "--no-resize" in sys.argv
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        # 1. Wait for the launcher window line.  init spawns the
        #    desktop, taskbar, launcher, then sh.  Launcher is the
        #    third [wm] window created line.  Use the sh prompt
        #    "/$ " as the canonical "stack is up" signal — by the
        #    time sh prints its prompt, all three GUI services
        #    have already opened their windows.
        ok, log = wait_after(ser, b"", b"/$ ", 30.0)
        if not ok:
            print("FAIL: never reached desktop")
            print(log[-800:].decode("ascii","replace")); return 1
        # Give the WM a moment for first compose
        time.sleep(1.5)
        log += drain(ser, time.time() + 0.5)
        print("[dbg] step 1 OK: desktop ready")

        # 2. Click "browser" button on launcher
        bx, by = LAUNCH_BTN["browser"]
        prior = log
        click(qmp, bx, by)
        ok, log = wait_after(ser, prior,
                              b"[browser] gui window=", 60.0)
        if not ok:
            print("FAIL: browser never opened")
            print(log[len(prior):][-1500:].decode("ascii","replace"))
            return 1
        ok, log = wait_after(ser, log, b"\n", 5.0)
        idx = log.rfind(b"[browser] gui window=")
        line = log[idx:].split(b"\n", 1)[0].decode("ascii", "replace")
        print(f"[dbg] step 2 OK: {line}")
        m = re.search(r"size=(\d+)x(\d+)", line)
        if not m:
            print(f"FAIL: couldn't parse browser size from: {line!r}")
            return 1
        bw, bh = int(m.group(1)), int(m.group(2))

        # Browser took cascade slot 1 → window at (112, 92).
        # Decoration: deco_w = bw + 8, deco_h = bh + 28.
        BORDER, TITLE_H, GRIP = 4, 24, 14
        win_x, win_y = 112, 92
        deco_w = bw + 2 * BORDER
        deco_h = bh + TITLE_H + BORDER

        # Wait for first paint to settle
        time.sleep(2.0)
        log += drain(ser, time.time() + 0.5)

        # 4. Move browser title bar away from the launcher region.
        #    Drag title-bar centre to a far-away point so launcher
        #    is fully visible afterwards.
        title_cx = win_x + deco_w // 2
        title_cy = win_y + TITLE_H // 2
        move_to_x, move_to_y = 700, 500
        drag(qmp, title_cx, title_cy, move_to_x, move_to_y, steps=10)
        # New top-left of decorated frame:
        new_win_x = win_x + (move_to_x - title_cx)
        new_win_y = win_y + (move_to_y - title_cy)
        print(f"[dbg] step 4 OK: browser moved to ({new_win_x},{new_win_y})")

        # 5. Drag resize grip (skip with --no-resize).
        if not skip_resize:
            grip_cx = new_win_x + deco_w - BORDER - GRIP // 2
            grip_cy = new_win_y + deco_h - BORDER - GRIP // 2
            # Drag grip 200 right and 100 down for a clearly
            # visible resize.
            end_x = min(grip_cx + 200, FB_W - 30)
            end_y = min(grip_cy + 100, FB_H - 50)
            drag(qmp, grip_cx, grip_cy, end_x, end_y, steps=12)
            print(f"[dbg] step 5 OK: dragged grip from "
                  f"({grip_cx},{grip_cy}) to ({end_x},{end_y})")
        else:
            print("[dbg] step 5 SKIPPED (--no-resize)")

        log += drain(ser, time.time() + 1.0)

        # 6. Click launcher's gui_term button.  Launcher is BEHIND
        #    browser in z-order but visible (we moved browser away).
        #    Clicking the launcher first raises it; we double-click
        #    to be safe (first click raises + focuses, second click
        #    activates the button).  Actually wm_pointer_button
        #    raises + dispatches in the same down-event — one click
        #    should be enough to fire the launcher's hit_test.
        gx, gy = LAUNCH_BTN["gui_term"]
        prior = log
        click(qmp, gx, gy)
        ok, log = wait_after(ser, prior,
                              b"[wm] window created id=", 15.0)
        if not ok:
            print("FAIL: gui_term window never created")
            print(log[len(prior):][-1500:].decode("ascii","replace"))
            return 1
        new = log[len(prior):]
        m = re.search(rb"\[wm\] window created id=0x([0-9a-fA-F]+) "
                      rb"pid=0x[0-9a-fA-F]+ size=0x([0-9a-fA-F]+)x"
                      rb"0x([0-9a-fA-F]+)", new)
        if m:
            term_id = int(m.group(1), 16)
            tw = int(m.group(2), 16); th = int(m.group(3), 16)
            print(f"[dbg] step 6 OK: gui_term win id={term_id} {tw}x{th}")
        else:
            print("[dbg] step 6 OK: gui_term window created")

        # gui_term spawns its child sh; banner draws to pty.
        time.sleep(2.0)
        log += drain(ser, time.time() + 0.5)

        # 7. Type 'echo zzzblah\n' via QMP.
        prior = log
        type_text(qmp, "echo zzzblah\n")
        time.sleep(2.0)
        log += drain(ser, time.time() + 0.5)

        new_serial = log[len(prior):].decode("ascii", "replace")
        if "zzzblah" in new_serial:
            print("[dbg] !! 'zzzblah' on SERIAL: keys leaked to "
                  "the kernel sh, NOT to gui_term")

        # 8. Screendump
        ppm = "/tmp/osdev-fb-brterm.ppm"
        png = "/tmp/osdev-fb-brterm.png"
        try: os.unlink(ppm)
        except FileNotFoundError: pass
        qsend(qmp, {"execute": "screendump",
                     "arguments": {"filename": ppm}})
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                time.sleep(0.1); break
            time.sleep(0.05)
        subprocess.run(["sips", "-s", "format", "png", ppm,
                        "--out", png],
                       stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        print(f"[dbg] screenshot: {png}")
        with open("/tmp/osdev-brterm-serial.log", "wb") as f:
            f.write(log)
        print(f"[dbg] serial log: /tmp/osdev-brterm-serial.log "
              f"({len(log)}B)")

        # 9. There is no headless OCR available, so we cannot
        # programmatically tell PASS from FAIL.  Earlier versions
        # of this script tried a "count fg-coloured pixels in
        # the gui_term body region" heuristic, but it was a
        # false-positive trap: when the browser's white HTML
        # page peeked through the gui_term rect (browser focused
        # / on top), the bright page pixels were counted as
        # "lots of rendered terminal text" and the script
        # reported PASS even though gui_term was actually
        # frozen.
        #
        # Just print the screenshot path and let the human
        # verify.  The user-visible signal is: does the gui_term
        # window contain "zzzblah" + a fresh prompt?  If yes,
        # the bug is fixed; if the body shows only the static
        # banner "[sh] tiny shell ready.", the bug reproduces.
        print(f"[dbg] inspect screenshot manually: {png}")
        print(f"[dbg]   PASS = gui_term shows 'zzzblah' line")
        print(f"[dbg]   FAIL = gui_term body shows only the "
              f"banner / no prompt update")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

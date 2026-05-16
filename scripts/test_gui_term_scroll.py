#!/usr/bin/env python3
"""scripts/test_gui_term_scroll.py — gui_term PgUp/PgDn scrollback.

Boots the OS headless, opens gui_term inside the inner /bin/sh,
generates more output than the visible window can hold, then
verifies that PageUp brings older lines back on-screen and
PageDown returns to the live cursor.

Outline
-------
1. Boot, wait for outer-shell '$ '.
2. Spawn `gui_term` over serial (so it's a child of /bin/sh).
3. Wait for the WM `[wm] window created` log line.
4. Type `ls` inside gui_term — the flat dump of the namespace
   is ~80 entries (every osfs file plus every binary), which
   far exceeds the ~24 visible rows.  This is what the user
   originally complained they couldn't read.
5. Hash the bottom row of the gui_term content area (where
   the live cursor / banner lives).  Snapshot A.
6. Send PageUp via QMP keyboard.  The bottom row should now
   contain the "-- scrollback (PgDn to return) --" banner,
   so the row hash MUST differ from snapshot A.
7. Send PageDown to scroll back to the live view.  The bottom
   row hash must match snapshot A again — proving the offset
   reset and the live cursor returned.

PageUp / PageDown enter the system as evdev keycodes 104 / 109
on the virtio-keyboard device, get translated to the standard
xterm CSI sequences `ESC [ 5 ~` and `ESC [ 6 ~` by
virtio_input.c, and are decoded by the WM CSI parser into the
GUI_KEY_PGUP / GUI_KEY_PGDN events that gui_term acts on.
"""

import json, os, select, socket, subprocess, sys, time, hashlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-scroll.sock"
SERIAL_SOCK = "/tmp/osdev-serial-scroll.sock"
DUMP_PATH   = "/tmp/osdev-fb-scroll.ppm"

FB_W, FB_H  = 1280, 800

# gui_term default geometry (matches gui_term.c).
WIN_W, WIN_H, TITLE_H = 720, 440, 24
WIN_X, WIN_Y = 80, 60
GUTTER_X, GUTTER_Y, GLYPH_H = 8, 6, 16
VISIBLE_ROWS = ((WIN_H) - 2 * GUTTER_Y - GLYPH_H) // GLYPH_H


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


# QEMU qcodes for the keys we care about.  See qemu/qapi/ui.json.
KEYMAP = {**{c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"},
          " ": "spc", "\n": "ret", "/": "slash", ".": "dot"}


def send_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [{
            "type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]}})


def type_text(qmp, text):
    for ch in text:
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
    idx = [0]
    def tok():
        while data[idx[0]:idx[0]+1] in (b" ", b"\n", b"\r", b"\t"): idx[0] += 1
        if data[idx[0]:idx[0]+1] == b"#":
            while data[idx[0]:idx[0]+1] not in (b"\n", b""): idx[0] += 1
            return tok()
        s = idx[0]
        while data[idx[0]:idx[0]+1] not in (b" ", b"\n", b"\r", b"\t", b""): idx[0] += 1
        return data[s:idx[0]]
    m = tok(); w = int(tok()); h = int(tok()); v = int(tok())
    assert m == b"P6" and v == 255
    idx[0] += 1
    return w, h, data[idx[0]: idx[0] + w*h*3]


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


# Hash a horizontal band of pixel rows.  Used to detect that the
# bottom-row of the gui_term content area has changed (live cursor
# vs scrollback banner) without committing to exact RGB values.
def band_hash(qmp, y0, y1):
    screendump(qmp, DUMP_PATH)
    w, _, px = read_ppm(DUMP_PATH)
    x0 = WIN_X + GUTTER_X
    x1 = WIN_X + WIN_W - GUTTER_X
    h = hashlib.sha1()
    for y in range(y0, y1):
        o = (y * w + x0) * 3
        h.update(px[o : o + (x1 - x0) * 3])
    return h.hexdigest()


def main():
    failed = []
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 20.0)
        if b"$ " not in boot_log:
            print("FAIL: outer shell prompt not reached")
            print(boot_log[-800:].decode("ascii", "replace"))
            return 1
        print("PASS: outer shell ready on serial")

        # Spawn gui_term over the serial-attached /bin/sh.
        ser.sendall(b"gui_term\n")
        log = wait_for(ser, b"[wm] window created", 6.0)
        if b"[wm] window created" not in log:
            print("FAIL: gui_term did not open a window")
            print(log[-400:].decode("ascii", "replace"))
            return 1
        print("PASS: gui_term window opened")
        time.sleep(1.5)         # let inner sh print its prompt

        # Generate more output than the visible window can hold.
        # bare `ls` dumps every entry across every mount — easily
        # exceeds VISIBLE_ROWS-1 (~24).
        type_text(qmp, "ls\n")
        time.sleep(2.0)

        # Bottom row band: where cur_line / cursor / scrollback
        # banner live.  We hash a 3-row tall slice to be robust
        # against per-pixel variance.
        bottom_y = WIN_Y + TITLE_H + GUTTER_Y + (VISIBLE_ROWS - 1) * GLYPH_H
        y0 = bottom_y
        y1 = bottom_y + GLYPH_H
        live_hash = band_hash(qmp, y0, y1)
        print(f"  live  bottom-row hash: {live_hash[:12]}")

        # PageUp: bottom row should switch to the scrollback banner
        # (and the rows above shift to older content).
        send_key(qmp, "pgup")
        time.sleep(0.6)
        scrolled_hash = band_hash(qmp, y0, y1)
        print(f"  pgup  bottom-row hash: {scrolled_hash[:12]}")
        if live_hash == scrolled_hash:
            print("FAIL: PageUp did not change the bottom row "
                  "(scrollback banner missing or PgUp not delivered)")
            failed.append("pgup-no-change")
        else:
            print("PASS: PageUp switched bottom row away from live cursor")

        # Also verify the body above the bottom row changed —
        # different older content should now be visible.
        upper_y0 = WIN_Y + TITLE_H + GUTTER_Y
        upper_y1 = upper_y0 + (VISIBLE_ROWS - 4) * GLYPH_H
        live_upper = band_hash(qmp, upper_y0, upper_y1)
        # Re-sample after pgup (we already pgup'd above).
        scrolled_upper = band_hash(qmp, upper_y0, upper_y1)
        # We didn't grab live_upper before pgup, so do another
        # round trip to validate: PgDn first to get back to live,
        # snapshot, then PgUp again.
        send_key(qmp, "pgdn")
        time.sleep(0.6)
        live_upper_2 = band_hash(qmp, upper_y0, upper_y1)
        send_key(qmp, "pgup")
        time.sleep(0.6)
        scrolled_upper_2 = band_hash(qmp, upper_y0, upper_y1)
        if live_upper_2 == scrolled_upper_2:
            print("FAIL: PageUp did not change the body content area")
            failed.append("pgup-body-no-change")
        else:
            print("PASS: PageUp also shifted older lines into the body")

        # PageDown: hash should return to live view.
        send_key(qmp, "pgdn")
        time.sleep(0.6)
        send_key(qmp, "pgdn")   # extra in case more than one page back
        time.sleep(0.6)
        returned_hash = band_hash(qmp, y0, y1)
        print(f"  pgdn  bottom-row hash: {returned_hash[:12]}")
        if returned_hash != live_hash:
            print("FAIL: PageDown did not restore the live view")
            failed.append("pgdn-no-restore")
        else:
            print("PASS: PageDown restored the live cursor")

        if failed:
            print(f"\nSCROLLBACK: {len(failed)} test(s) FAILED: {failed}")
            return 1
        print("\nSCROLLBACK: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

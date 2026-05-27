#!/usr/bin/env python3
"""scripts/test_wsd_bar_overlap.py -- chapter 118 follow-up.

The exact scenario from the user's screenshot:
  - gui_term is the FRONT window
  - browser is the BACK window (partially covered by gui_term)
  - browser keeps painting (toolbar updates etc.)
  - bug: browser's title text shows OVER gui_term's body wherever
    they overlap, because compose_rect repaints the back bar
    on every browser-side damage without clipping the bar paint
    against front windows.

The fix: paint_decoration_clipped + dirty-rect clip in
compose_rect.  After the back-window's body+bar paint, the
front window's body blit (also clipped to dirty rect) wins in
the overlap.

We exercise this by opening browser first (back), then notepad
(front, opens at 140,140 with a cascade-by-40 push so it
overlaps browser).  Then we type a character into notepad to
force a notepad redraw on top of browser bar position?  No:
notepad bar is at y=140..164 and browser was opened at the
cascade origin (100,100) with bar at y=100..124.

Better: spawn notepad FIRST so it lands at (140, 140), then
spawn browser via stdin so it cascades to (180, 180).  Then
notepad is BACK (bar at 140..164), browser is FRONT (bar at
180..204).  The browser body (180, 204+...) overlaps notepad
body (140, 164+...).  If we trigger notepad to repaint without
moving its z-order, its bar paint must NOT overpaint browser.

But the failure mode is the inverse: BACK window repaints, its
bar overpaints FRONT.  We need BROWSER to repaint while NOTEPAD
is front.  Browser is the second-opened so it's on top; we'd
have to raise notepad first.

Simplest setup: open notepad (140, 140), then RAISE notepad by
clicking its body, then spawn browser?  No -- browser opens on
top by cascade.

We'll just open notepad, then open a second notepad-like (re-
launching notepad won't because of /bin name), or just trigger
the original bug by raising the launcher (originally at (100,
100)) to the top:
  1. Open notepad at (140, 140) -- now on top
  2. Click launcher body at (110, 200) -- launcher raises to
     top, notepad goes behind launcher in z-order
  3. Click notepad body at (500, 400) -- this is NOT inside
     launcher (launcher at x=100..340).  Notepad raises to
     top again.

Hmm.  Let's just type into notepad to force it to repaint and
check that notepad's bar (which is now behind launcher in z if
launcher is on top) doesn't bleed onto launcher.  But notepad
bar at (140, 140, 720, 24) and launcher body at (100, 124, 240,
208).  They overlap at x=140..340, y=140..164.  So if launcher
is FRONT (raised) and notepad is BACK, then when notepad sends
a damage, notepad's bar at (140, 140, 720, 24) gets painted.
Without clip, it covers x=140..340 at y=140..164, which is in
launcher's body.  With clip, the dirty rect would be (notepad
body + notepad bar) -- still (140, 140, 720, 24+440).  And the
clip is the dirty rect.  paint_decoration_clipped paints
notepad's bar within clip.  But clip == dirty rect, which
COVERS launcher's body intersection.

Wait, but the next iteration paints LAUNCHER's body (intersected
with dirty rect).  Launcher's body (100, 124, 240, 208) ∩ dirty
rect (140, 140, 720, 464) = (140, 140, 200, 192) (clamped to
both).  So launcher body BLIT covers x=140..340, y=140..332.
This OVERWRITES notepad's bar where they intersect at
y=140..164.  Great, so the fix should work.

Plan:
  1. Boot to desktop, wait for prompt.
  2. Spawn notepad (`notepad /tmp/foo.txt`).
  3. Click launcher body at (110, 200) -- raises launcher to top.
  4. Move cursor away.
  5. Type a character into notepad (we can use serial pipe to
     send characters since the launcher has kbd focus; OR
     trigger a notepad redraw by clicking notepad body once
     to focus it then typing -- but clicking notepad raises
     notepad).
  6. Easier: just exercise the visual bug.  Move cursor over
     notepad body to trigger notepad hover events that cause
     notepad to redraw, OR wait for notepad's clock-style
     periodic redraws (if any).

Actually simplest: skip the dynamic part and just check that
after the launcher comes to the top, repeated screenshots show
launcher's body STAYS launcher (no bleeding from notepad bar
which is behind).

Test sequence:
  1. Boot.
  2. notepad /tmp/foo.txt   -> notepad on top
  3. Click launcher body    -> launcher on top, notepad behind
  4. Move cursor away to (700, 700)
  5. Capture pixel at (200, 150).  This is inside the OVERLAP
     of notepad's bar (140..860, 140..164) and launcher's body
     (100..340, 124..332).  With launcher on top, the pixel
     should be LAUNCHER's body bg (light gray E8 EC F0), NOT
     notepad's bar colour (a5 6e 3a or 77 66 55).
  6. Force notepad to repaint (move cursor inside notepad's
     body to trigger MOUSE_MOVE event -> notepad updates hover
     state -> redraws).  Or just wait -- some apps redraw
     periodically.

Without the fix: notepad eventually repaints, its bar bleeds
through launcher's body.
With the fix: launcher's body stays pristine.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-overlap.sock"
SERIAL_SOCK = "/tmp/osdev-serial-overlap.sock"
DUMP_PATH   = "/tmp/osdev-fb-overlap.ppm"

FB_W = 1280
FB_H = 800

# Launcher: opened first by desktop, at (100, 100), 240x232 +
# wsd 24-px title bar.  Body in scanout coords: (100, 124) to
# (340, 332).  Light-gray bg E8 EC F0.
LAUNCHER_X, LAUNCHER_Y = 100, 100
LAUNCHER_W, LAUNCHER_H = 240, 232
LAUNCHER_BG_RGB = (0xE8, 0xEC, 0xF0)

# Notepad opens at the cascade origin (140, 140), 720x440.
WIN_X, WIN_Y = 140, 140
WIN_W, WIN_H = 720, 440
TITLE_H      = 24

# wsd title-bar bg active = 0xff3a6ea5 -> (R,G,B)=(0xa5,0x6e,0x3a)
# idle   = 0xff556677 -> (0x77,0x66,0x55)
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

        # Spawn notepad -- opens at (140, 140) with body 720x440.
        # Notepad is now on top.
        ser.sendall(b"notepad /tmp/overlap.txt\n")
        log = wait_for(ser, b"[wm] window created", 6.0)
        if b"[wm] window created" not in log:
            print("FAIL: notepad did not open")
            print(log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: notepad window opened")
        time.sleep(0.8)

        # Click launcher BODY (not its title bar) to raise it.
        # Launcher body: x∈[100,340], y∈[124,332] in scanout coords.
        # Click well inside the body and clear of any buttons.
        # Launcher buttons are at BTN_X=16, four 36-px buttons
        # spaced 8 px from y=16, in window-local coords:
        #   FB-local y in [16, 52], [60, 96], [104, 140], [148, 184]
        # In scanout coords (body starts at y=LAUNCHER_Y+TITLE_H=124):
        #   y in [140, 176], [184, 220], [228, 264], [272, 308]
        # Sample (110, 320) -- below all buttons, inside the
        # launcher body's bottom margin.  Should NOT spawn any
        # app; pure z-raise.
        print(f"  raising launcher with click at (110, 320)")
        left_click(qmp, 110, 320)
        time.sleep(0.6)
        # Move cursor away so the sprite doesn't pollute samples.
        cursor_to(qmp, 700, 700); time.sleep(0.4)

        # Pre-sample: pick a pixel that is both INSIDE launcher's
        # body AND inside notepad's bar (y=140..164).  Launcher
        # body is x=100..340, but most of x=116..324 at this y
        # is covered by button 0 (gui_term).  The launcher's
        # body BG (E8 EC F0) is visible only in the side margins
        # x=100..116 and x=324..340.  Sample x=108 (left margin
        # midpoint), y=152 (mid of notepad's bar).
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        SX, SY = 108, 152
        pre = pixel_at(ppm, SX, SY)
        if not near(pre, LAUNCHER_BG_RGB, tol=20):
            for sx, sy in [(110, 150), (110, 155), (330, 152),
                           (332, 150), (108, 145), (108, 160)]:
                rgb = pixel_at(ppm, sx, sy)
                if near(rgb, LAUNCHER_BG_RGB, tol=20):
                    SX, SY = sx, sy; pre = rgb; break
        if not near(pre, LAUNCHER_BG_RGB, tol=20):
            print(f"FAIL: cannot find launcher bg pixel post-raise "
                  f"sample at ({SX},{SY}) = {pre}, expected ~{LAUNCHER_BG_RGB}; "
                  f"layout assumption wrong")
            return 1
        print(f"PASS: launcher body on top post-raise at ({SX},{SY}) = {pre}")

        # Trigger notepad to repaint by sending a typing event to
        # the kernel-WM through serial -- notepad has stdin via
        # the kernel's keyboard route.  Actually keyboard focus
        # is on launcher (just raised), so kbd events go there.
        # Alternative: move cursor inside notepad's body to fire
        # GUI_EVENT_MOUSE_MOVE on notepad.  But notepad's body
        # (140..860, 164..604) overlaps launcher's body at
        # x=140..340, y=164..332.  Sample at (500, 400) -- well
        # outside launcher, inside notepad body.
        # Notepad's MOUSE_MOVE handler updates hover, sets dirty,
        # repaints, and damages the full window.
        print(f"  triggering notepad repaints via mouse-move "
              f"inside notepad body")
        for cx, cy in [(500, 300), (500, 400), (500, 500),
                       (700, 400), (300, 400)]:
            cursor_to(qmp, cx, cy); time.sleep(0.15)
        cursor_to(qmp, 700, 700); time.sleep(0.6)

        # Re-sample: launcher body pixel should STILL be launcher
        # bg, not notepad bar colour.
        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        post = pixel_at(ppm, SX, SY)
        if (near(post, TITLE_BG_ACTIVE, tol=20)
                or near(post, TITLE_BG_IDLE, tol=20)):
            print(f"BUG#2 FAIL: launcher body at ({SX},{SY}) = {post} -- "
                  f"notepad title bar bled through!")
            rc = 1
        elif not near(post, LAUNCHER_BG_RGB, tol=25):
            print(f"BUG#2 WARN: launcher body at ({SX},{SY}) = {post} -- "
                  f"not launcher bg ({LAUNCHER_BG_RGB}) but also not "
                  f"notepad bar.  May be a button glyph; check manually.")
        else:
            print(f"BUG#2 PASS: launcher body intact after notepad "
                  f"repaints ({post})")
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

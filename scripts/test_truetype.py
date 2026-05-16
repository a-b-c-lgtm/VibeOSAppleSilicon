#!/usr/bin/env python3
"""scripts/test_truetype.py -- chapter-102 regression test.

Boots headless to the desktop, screendumps the framebuffer, and
asserts two properties of the launcher's button labels that
together prove the in-kernel TrueType rasteriser is active:

  1. Grayscale anti-aliasing is happening. The launcher's
     button-0 ("gui_term") is filled with BTN_BGRA (a mid blue)
     and the label is drawn in TEXT_BGRA (near-black). With the
     pre-chapter-102 bitmap font, every pixel is exactly one or
     the other -- the bitmap is 1 bpp, no blending. With the
     TTF rasteriser, glyph edges are partial-coverage alpha
     values blended into the button colour. We look for pixels
     in the label strip whose colour is neither near BTN_BGRA
     nor near TEXT_BGRA: those can only exist if AA is on.

  2. Glyphs land off the 8-pixel grid. The bitmap font is
     monospace at 8 px, so every fg/bg transition in a label
     lands at an x-coordinate that is a multiple of 8. DejaVu
     Sans is proportional, so most transitions don't. We scan
     the label row for transitions and require at least one to
     fall at an x where (x % 8) != 0.

If either check fails the kernel has silently fallen back to the
bitmap font (or the variable-advance plumbing is broken).
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-ttf.sock"
SERIAL_SOCK = "/tmp/osdev-serial-ttf.sock"
DUMP_PATH   = "/tmp/osdev-fb-ttf.ppm"

FB_W = 1280
FB_H = 800

# Launcher window geometry. The launcher is the first window the WM
# creates on boot, so it lands at (80, 60) with a 240x232 body and a
# 24 px title bar. Button 0 ("gui_term") is the topmost button.
WIN_X, WIN_Y = 80, 60
WIN_W, WIN_H = 240, 232
TITLE_H      = 24

# Inside the window content area:
#   BTN_X     = 16
#   BTN_TOP   = 16
#   BTN_W     = 208
#   BTN_H     = 36
#   GLYPH_H   = 16
BTN_AREA_X0 = WIN_X + 16                       # 96
BTN_AREA_X1 = BTN_AREA_X0 + 208                # 304
BTN0_Y0     = WIN_Y + TITLE_H + 16             # 100
BTN0_Y1     = BTN0_Y0 + 36                     # 136

# Pixel colours from userspace/launcher/launcher.c (GUI_BGRA packs
# the bytes into a 32-bit BGRA word; the framebuffer hands QEMU
# pixels as R, G, B in PPM output, which is what we compare here).
BTN_BGRA  = (0xC0, 0xD0, 0xE8)   # button fill (label background)
TEXT_BGRA = (0x10, 0x18, 0x28)   # label foreground

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

def near(a, b, tol):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell prompt reached")

        # Give the launcher a beat to render its first frame.
        time.sleep(0.5)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        print(f"  saved screendump: {DUMP_PATH}")

        # Verify the launcher's first button actually painted -- look
        # for a BTN_BGRA pixel away from the label area (top-left
        # corner of the button body, a few pixels in).
        btn_corner = pixel_at(ppm, BTN_AREA_X0 + 4, BTN0_Y0 + 4)
        if not near(btn_corner, BTN_BGRA, tol=8):
            print(f"FAIL: launcher button-0 not painted "
                  f"(pixel at ({BTN_AREA_X0+4},{BTN0_Y0+4}) = {btn_corner}, "
                  f"expected ~{BTN_BGRA})")
            return 1
        print(f"PASS: launcher button-0 painted "
              f"(corner pixel = {btn_corner})")

        # -------- Check 1: grayscale AA along the label row. -------
        #
        # Walk a band of rows that cover the label's pixel rows. The
        # label cell is GLYPH_H=16 tall and starts at by + (BTN_H -
        # GLYPH_H)/2 = by + 10 (relative to the button top). For
        # robustness we scan rows BTN0_Y0+12 .. BTN0_Y0+26 (15 rows
        # spanning the rendered glyphs).
        intermediate = 0
        intermediate_samples = []
        for y in range(BTN0_Y0 + 12, BTN0_Y0 + 27):
            for x in range(BTN_AREA_X0 + 2, BTN_AREA_X1 - 2):
                p = pixel_at(ppm, x, y)
                if near(p, BTN_BGRA, tol=12):  continue   # background
                if near(p, TEXT_BGRA, tol=24): continue   # solid text
                intermediate += 1
                if len(intermediate_samples) < 4:
                    intermediate_samples.append((x, y, p))

        if intermediate < 4:
            print(f"FAIL: only {intermediate} intermediate-alpha pixels "
                  f"found in the label band; expected >= 4")
            print(f"      a bitmap (non-AA) font produces 0; the TTF "
                  f"rasteriser produces tens")
            return 1
        print(f"PASS: {intermediate} intermediate-alpha pixels found "
              f"(proves grayscale AA is on)")
        for x, y, p in intermediate_samples:
            print(f"        e.g. ({x},{y}) -> {p}")

        # -------- Check 2: glyphs land off the 8-pixel grid. -------
        #
        # Scan the centre row of the label for transitions from
        # background-like to foreground-like pixels. Record each
        # transition's x. With the monospace 8x16 bitmap every
        # transition is at a multiple of 8 (the cell grid). With the
        # TTF font most transitions are at non-multiples.
        scan_y = BTN0_Y0 + 18  # near the middle of the label row
        transitions = []
        prev_state = None  # 'bg' or 'fg'
        for x in range(BTN_AREA_X0 + 2, BTN_AREA_X1 - 2):
            p = pixel_at(ppm, x, scan_y)
            if near(p, BTN_BGRA, tol=20):
                state = "bg"
            elif p[0] < 120 and p[1] < 130 and p[2] < 140:
                # significantly darker than BTN_BGRA -- treat as fg
                state = "fg"
            else:
                state = "mid"
            if prev_state == "bg" and state in ("fg", "mid"):
                transitions.append(x)
            prev_state = state

        if not transitions:
            print("FAIL: no bg->fg transitions found in label scanline -- "
                  "label did not render?")
            return 1

        off_grid = [x for x in transitions if (x - BTN_AREA_X0) % 8 != 0]
        if not off_grid:
            print(f"FAIL: every label transition landed on the 8-pixel "
                  f"grid -- this looks like the monospace bitmap font.")
            print(f"      transitions (relative to button left): "
                  f"{[x - BTN_AREA_X0 for x in transitions]}")
            return 1
        print(f"PASS: {len(off_grid)}/{len(transitions)} label "
              f"transitions are off the 8-pixel grid")
        # Print the first few as evidence.
        rel = [(x - BTN_AREA_X0) for x in transitions[:8]]
        print(f"        first {min(8, len(rel))} transitions "
              f"(relative x): {rel}")

        print("\nCHAPTER 102: TRUETYPE FONT TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

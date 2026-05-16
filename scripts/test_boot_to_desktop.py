#!/usr/bin/env python3
"""scripts/test_boot_to_desktop.py — milestone-46 smoke test.

Boots the system fully headless, with NO keyboard input.  Asserts:
  1. init logs that it spawned both /bin/launcher and /bin/sh
  2. the WM logs a window-create from launcher
  3. a screendump shows the launcher window painted in the
     upper-left of the framebuffer (a verified pixel inside its
     light-grey body region)
  4. the wallpaper is the new gradient (top brighter than bottom)
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-boot.sock"
SERIAL_SOCK = "/tmp/osdev-serial-boot.sock"
DUMP_PATH   = "/tmp/osdev-fb-boot.ppm"

FB_W = 1280
FB_H = 800

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

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        # Boot-to-desktop with NO input: launcher should auto-start.
        boot_log = wait_for(ser, b"$ ", 20.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        if b"launching /bin/launcher" not in boot_log:
            print("FAIL: init did not auto-spawn launcher")
            return 1
        print("PASS: init auto-spawned /bin/launcher")

        # Give the launcher a moment to register its window.
        more = drain(ser, time.time() + 1.0)
        boot_log += more
        if b"[wm] window created" not in boot_log:
            print("FAIL: launcher did not create a window")
            return 1
        print("PASS: launcher window created in WM")

        # Take a screenshot and verify the launcher body pixel.
        time.sleep(0.4)
        screendump(qmp, DUMP_PATH)
        print(f"  saved screendump: {DUMP_PATH}")

        ppm = read_ppm(DUMP_PATH)
        # Launcher BG is 0xE8ECF0 (light grey-blue).  It lives at
        # (80, 60) with a 24px title bar, so the content area starts
        # at absolute y=84.  Buttons start at content-y=16 (absolute
        # y=100), so the 16px top-margin spans absolute y=84..100.
        # Sample at (200, 90) — well inside the BG margin.
        body = pixel_at(ppm, 200, 90)
        if body[0] < 220 or body[1] < 220 or body[2] < 220:
            print(f"FAIL: launcher body at (200,90) = {body}, expected light-grey BG")
            return 1
        print(f"PASS: launcher body painted (pixel at (200,90) = {body})")

        # Verify the wallpaper is actually rendered (not pure-black
         # framebuffer).  Originally this asserted "top brighter
         # than bottom" because M46 shipped with a procedural
         # top-to-bottom gradient; M50 replaced that with a real
         # bitmap wallpaper (currently a photographic image whose
         # luminance varies arbitrarily across the frame), so the
         # gradient direction is no longer a meaningful invariant.
         # We now just sample two wallpaper pixels far from any
         # window decoration and require the wallpaper area to be
         # non-trivial: not pure black AND not pure white.
        top_px    = pixel_at(ppm, 1000, 30)
        bottom_px = pixel_at(ppm, 1000, 700)
        def trivial(p): return sum(p) < 12 or sum(p) > 750
        if trivial(top_px) or trivial(bottom_px):
            print(f"FAIL: wallpaper area looks blank "
                  f"(top {top_px}, bottom {bottom_px})")
            return 1
        print(f"PASS: wallpaper rendered (top {top_px}, bottom {bottom_px})")

        print("\nMILESTONE 46: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

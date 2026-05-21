#!/usr/bin/env python3
"""scripts/test_browser_intrinsic_size.py — chapter 98b regression.

Reproduction of the bug reported on the chapter 98 PNG arc:
real-world PNGs embedded with no width=""/height="" attributes
rendered at the layout's 16x16 placeholder size, clipping every
400x400 source image down to a tiny corner square.

Chapter 98b added a `layout_set_img_size_lookup` hook in
[userspace/libc/layout.h](userspace/libc/layout.h) and wired it
from [userspace/browser/browser.c](userspace/browser/browser.c)
to the per-page image cache.  load_page() now decodes images
first, then re-runs layout with the hook live so <img> tags
without explicit dimensions get sized to the intrinsic pixel
dimensions instead of the 16x16 fallback.

This test loads a tiny HTML page that references a 64x64 PNG
with no width/height attributes:

    <img src="/icon_large.png" alt="missing big icon" />

The PNG is a four-quadrant palette image: red / green / blue /
white in 32x32 quadrants.  Pre-fix this would render as a 16x16
placeholder showing only the all-red top-left corner (~256 red
pixels, 0 each of green / blue / white).  Post-fix the image
renders at 64x64 with the full 4096 pixels (1024 per quadrant).

We require >= 600 pure pixels of EACH non-red colour as the
post-fix signal — well above any pre-fix value and well below
the true 1024 (giving slack for the line of <p> text above the
image, anti-aliased text, and any window-chrome reds).
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-intrinsic.sock"
SERIAL_SOCK = "/tmp/osdev-serial-intrinsic.sock"
DUMP_PATH   = "/tmp/osdev-fb-intrinsic.ppm"

FB_W = 1280
FB_H = 800

# Each pure-colour quadrant of icon_large.png is 32x32 = 1024
# pixels.  We demand 600 of each non-red colour as the post-fix
# signal (well above the 0 each that the pre-fix 16x16 placeholder
# would have shown).
MIN_QUADRANT_PIXELS = 600


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
        "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-audiodev", "none,id=audio0",
        "-device", "virtio-sound-device,audiodev=audio0",
        "-object", "rng-random,id=rng0,filename=/dev/urandom",
        "-device", "virtio-rng-device,rng=rng0",
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
    deadline = time.time() + 3.0
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


def count_color(ppm, target_rgb, tol=4):
    w, h, data = ppm
    tr, tg, tb = target_rgb
    c = 0
    n = w * h
    for i in range(n):
        o = i * 3
        if (abs(data[o]   - tr) <= tol and
            abs(data[o+1] - tg) <= tol and
            abs(data[o+2] - tb) <= tol):
            c += 1
    return c


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 90.0):
            print("FAIL: shell prompt not reached")
            return 1

        ser.sendall(b"browser --gui /mnt/intrinsic.html 800 &\n")
        time.sleep(6.0)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        if (ppm[0], ppm[1]) != (FB_W, FB_H):
            print(f"FAIL: bad framebuffer size {ppm[0]}x{ppm[1]}")
            return 1

        red   = count_color(ppm, (255,   0,   0))
        green = count_color(ppm, (  0, 255,   0))
        blue  = count_color(ppm, (  0,   0, 255))
        print(f"on-screen pixel counts: red={red} green={green} blue={blue}")

        # Pre-fix: the 16x16 placeholder would have shown only the
        # top-left red quadrant of the 64x64 image, giving ~256
        # pure-red pixels and 0 of green / blue.  Post-fix every
        # quadrant must be visible.
        for label, count in (("green", green), ("blue", blue)):
            if count < MIN_QUADRANT_PIXELS:
                print(f"FAIL: only {count} pure-{label} pixels on screen "
                      f"(expected >= {MIN_QUADRANT_PIXELS}).  Either "
                      f"layout's intrinsic-size hook didn't fire (likely "
                      f"a layout regression in <img> sizing) or the "
                      f"second layout pass in load_page() didn't run.")
                return 1
        # Red is fuzzier (page text, window-chrome reds) so we just
        # require the quadrant exists.
        if red < MIN_QUADRANT_PIXELS:
            print(f"FAIL: only {red} pure-red pixels (expected >= "
                  f"{MIN_QUADRANT_PIXELS} from the icon's 32x32 red "
                  f"quadrant).")
            return 1

        print(f"PASS: 64x64 intrinsic-size image fully rendered "
              f"(red={red} green={green} blue={blue})")
        print("PASS: chapter 98b intrinsic-size hook smoke test")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

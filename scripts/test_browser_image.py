#!/usr/bin/env python3
"""scripts/test_browser_image.py — chapter 98 end-to-end test.

Boots the kernel graphically, drops to /bin/sh, opens the test
page in the GUI browser, and asserts that the icon.png we bake
at build time actually shows up on the framebuffer.

Detection strategy: search the framebuffer for pure-blue pixels
(R=0, G=0, B=255).  Our test PNG (scripts/make_test_png.py) is
mostly red but has a 4x4 block of pure blue in its bottom-right
corner — the rarest colour in any of our test pages.  A score of
>= 4 blue pixels on screen confirms the image cache + decoder +
LAY_PAINT_IMAGE blit pipeline all worked end-to-end.

Why blue and not red?  Red is the default body{color} on test.html
and shows up in glyph anti-aliased fragments all over the page.
Blue is reserved for hyperlinks (which our test page has none),
so a positive blue match is unambiguous.

We also sample (0, 0, 255) -> (R, G, B) per PPM convention: PPM
is RGB-major, our renderer's BGRA gets pixel-format-converted by
QEMU when it produces the screendump, so the bytes we read from
disk are RGB.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-img.sock"
SERIAL_SOCK = "/tmp/osdev-serial-img.sock"
DUMP_PATH   = "/tmp/osdev-fb-img.ppm"

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


def count_color(ppm, target_rgb, tol=8):
    """Count pixels whose channels are all within `tol` of target."""
    w, h, data = ppm
    tr, tg, tb = target_rgb
    c = 0
    # Iterate as a flat byte stream for speed.
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

        # Wait for shell prompt (boots into the desktop with a launcher;
        # we use the serial console for command entry instead).
        if b"$ " not in wait_for(ser, b"$ ", 90.0):
            print("FAIL: shell prompt not reached")
            return 1

        # Open the GUI browser on our test page.  The browser
        # registers a window with the WM and starts blitting; the
        # serial shell will hand control off as soon as the
        # process is exec'd.  We background it so the prompt
        # returns and we can keep typing if needed.
        ser.sendall(b"browser --gui /mnt/img_test.html 800 &\n")
        # Give the browser ~6 s to fetch + decode + render.
        # The page is tiny (~250 bytes HTML, 155-byte PNG) so
        # even on a cold cache this should be well under a second
        # of real work; the bulk of the wall-clock is the
        # process spawn + wm window registration.
        time.sleep(6.0)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        if (ppm[0], ppm[1]) != (FB_W, FB_H):
            print(f"FAIL: bad framebuffer size {ppm[0]}x{ppm[1]}")
            return 1

        # Count pure-blue pixels (the icon's 4x4 BR landmark).
        blue = count_color(ppm, (0, 0, 255), tol=4)
        # Sanity: red pixels (icon's main background, ~140 of them).
        red  = count_color(ppm, (255, 0, 0), tol=4)
        # Sanity: green diagonal pixels (~14 of them after corners).
        green = count_color(ppm, (0, 255, 0), tol=4)

        print(f"on-screen pixel counts: red={red} green={green} blue={blue}")

        if blue < 4:
            print(f"FAIL: only {blue} blue pixels found on screen "
                  f"(expected >= 4 from the icon's 4x4 BR landmark).  "
                  f"Either the image cache didn't decode, "
                  f"LAY_PAINT_IMAGE wasn't emitted, or the blitter "
                  f"isn't compositing the pixels.")
            return 1
        if red < 16:
            print(f"FAIL: only {red} red pixels found on screen "
                  f"(expected dozens from the icon's red fill).")
            return 1

        print(f"PASS: image rendered "
              f"(red={red} green={green} blue={blue})")
        print("PASS: chapter 98 browser image smoke test")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

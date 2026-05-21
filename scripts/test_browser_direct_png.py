#!/usr/bin/env python3
"""scripts/test_browser_direct_png.py — chapter 98 end-to-end test.

User-visible bug being regressed against: navigating the browser
directly to a PNG URL used to feed the raw image bytes through
the HTML tokenizer, rendering garbled text ("IHDR PLTE IDAT ...")
instead of the picture.

Chapter 98 added a content-type sniff in load_page(): if the
fetched body starts with the PNG signature, the decoded BGRA is
pre-installed in the per-page image cache under p->url and the
parser is fed a tiny synthetic wrapper:

    <html><body><p>Image: URL (WxH)</p>
      <img src="URL" width="W" height="H" alt="image" />
    </body></html>

The rest of the pipeline (parser → layout → br_attach_images →
LAY_PAINT_IMAGE blit) runs unchanged.  br_resolve_img_src
collapses the synthetic <img>'s src against p->url so the cache
lookup hits without a second fetch.

This test:
  1. Boots the kernel into the desktop.
  2. Launches `browser --gui /mnt/icon_palette.png 800 &`.
  3. Screendumps the framebuffer.
  4. Counts the pure-colour pixels for each quadrant of the
     baked test PNG — 8x8 each of red / green / blue / white.
     Each quadrant is 64 pixels; we require >= 32 of each
     (giving a generous slack for anti-aliased edges, the
     wrapper text rendered above the image, and any window-
     chrome anti-aliasing).
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-direct-png.sock"
SERIAL_SOCK = "/tmp/osdev-serial-direct-png.sock"
DUMP_PATH   = "/tmp/osdev-fb-direct-png.ppm"

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

        # Navigate the browser DIRECTLY to the paletted PNG.  No
        # wrapper HTML on disk: the sniffer + synthesizer have to
        # do their job for anything to appear.
        ser.sendall(b"browser --gui /mnt/icon_palette.png 800 &\n")
        time.sleep(6.0)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        if (ppm[0], ppm[1]) != (FB_W, FB_H):
            print(f"FAIL: bad framebuffer size {ppm[0]}x{ppm[1]}")
            return 1

        red   = count_color(ppm, (255,   0,   0))
        green = count_color(ppm, (  0, 255,   0))
        blue  = count_color(ppm, (  0,   0, 255))
        # White matches a lot of background chrome (title bars,
        # button highlights, default text color in the wrapper
        # <p> heading).  We don't gate on it.
        print(f"on-screen pixel counts: red={red} green={green} blue={blue}")

        # Each quadrant is 8x8 = 64 pixels of one solid colour.
        # Require >= 32 of each (allow generous slack — most of
        # the time we see exactly 64).
        FAIL = False
        for label, count in (("red", red), ("green", green), ("blue", blue)):
            if count < 32:
                print(f"FAIL: only {count} pure-{label} pixels on screen — "
                      f"expected ~64 from the icon's {label} 8x8 quadrant.  "
                      f"Either the sniff/synth path didn't fire (we'd see "
                      f"raw 'PNG IHDR PLTE' text instead) or the palette "
                      f"decoder produced wrong pixels.")
                FAIL = True
        if FAIL:
            return 1

        print(f"PASS: direct PNG navigation rendered the image "
              f"(red={red} green={green} blue={blue})")
        print("PASS: chapter 98 direct PNG navigation smoke test")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

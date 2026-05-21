#!/usr/bin/env python3
"""scripts/test_fontd.py -- chapter-108b regression test.

Asserts that the userspace font server replaces the in-kernel
TTF rasteriser end-to-end.  Three positive checks:

  1. The expected boot banner appears in serial:
       [fontd] ready on /srv/font
     Proves init's supervisor spawned the daemon and the
     daemon successfully bound /srv/font (which in turn
     means the chapter-107 named-IPC bus is up).

  2. The shell prompt is reached.  Proves init didn't get
     stuck behind fontd startup -- the supervisor is
     non-blocking.

  3. A framebuffer screendump shows grayscale-AA pixels in
     a launcher button label.  This is the same property
     test_truetype.py checks, but interpreted differently:
     pre-chapter-108b it proved the in-kernel TTF
     rasteriser was active; post-chapter-108b it proves
     that the kernel WM successfully reached fontd over IPC
     and got back AA bitmaps.  If fontd were unreachable
     wm_draw_text would fall back to the bitmap font and
     every label pixel would be 0/255 in the foreground/
     background colour with NO intermediate alphas.

The fontd-respawn-on-crash path is exercised by chapter-108's
supervisor, which is tested generically by test_clipboard.py
(same code path).  No need to re-test it here.
"""
import os
import select
import socket
import subprocess
import sys
import time
import json

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERIAL_SOCK = "/tmp/osdev-serial-fontd.sock"
QMP_SOCK    = "/tmp/osdev-qmp-fontd.sock"
DUMP_PATH   = "/tmp/osdev-fb-fontd.ppm"

FB_W = 1280
FB_H = 800

# Launcher window geometry (matches test_truetype.py).
WIN_X, WIN_Y = 80, 60
TITLE_H      = 24
BTN_AREA_X0  = WIN_X + 16                       # 96
BTN_AREA_X1  = BTN_AREA_X0 + 208                # 304
BTN0_Y0      = WIN_Y + TITLE_H + 16             # 100

BTN_BGRA  = (0xC0, 0xD0, 0xE8)   # button fill (label background)
TEXT_BGRA = (0x10, 0x18, 0x28)   # label foreground


def cleanup():
    for p in (SERIAL_SOCK, QMP_SOCK):
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


def wait_for(s, needle, timeout, accum=None):
    """Wait up to `timeout` seconds for `needle` in the serial
    stream.  Accumulates bytes into the optional `accum` bytearray
    so subsequent waits don't miss output that arrived in the same
    drain window as the previous needle.  Without `accum` we'd
    have a race where two needles produced close together (a fast
    boot, say) get consumed by the first wait and the second
    wait never sees its needle.  Returns the accumulated buffer
    so legacy callers still get a snapshot."""
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    if accum is None: accum = bytearray()
    if needle in accum: return bytes(accum)
    while time.time() < deadline:
        chunk = drain(s, time.time() + 0.4)
        if chunk: accum.extend(chunk)
        if needle in accum: return bytes(accum)
    return bytes(accum)


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

        # Shared accumulator so check 2 doesn't miss `$ ` that
        # arrived in the same drain window as the fontd banner.
        # Boot is fast (~1.5s on HVF) and both lines now land
        # together; an earlier per-call buffer would consume
        # both into check 1 and time check 2 out.
        accum = bytearray()

        # -------- Check 1: fontd boot banner -------------------
        wait_for(ser, b"[fontd] ready on /srv/font", 20.0, accum)
        if b"[fontd] ready on /srv/font" not in accum:
            print("FAIL: fontd boot banner not seen")
            print("  expected: '[fontd] ready on /srv/font'")
            print("  init may not be spawning /bin/fontd, or fontd "
                  "is crashing before srv_bind")
            return 1
        print("PASS: fontd announced 'ready on /srv/font'")

        # -------- Check 2: shell prompt still reachable --------
        wait_for(ser, b"$ ", 15.0, accum)
        if b"$ " not in accum:
            print("FAIL: shell prompt not reached "
                  "(supervisor may be blocking init)")
            return 1
        print("PASS: shell prompt reached (init didn't block on fontd)")

        # Let the launcher render its first frame.
        time.sleep(0.5)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        print(f"  saved screendump: {DUMP_PATH}")

        # -------- Check 3: grayscale AA in label band ----------
        # If fontd is broken / unreachable, the WM falls back to
        # the bitmap font which produces only 0/255 alphas.  The
        # presence of intermediate-alpha pixels proves the WM
        # successfully fetched AA glyphs from fontd over IPC.
        intermediate = 0
        for y in range(BTN0_Y0 + 12, BTN0_Y0 + 27):
            for x in range(BTN_AREA_X0 + 2, BTN_AREA_X1 - 2):
                p = pixel_at(ppm, x, y)
                if near(p, BTN_BGRA, tol=12):  continue
                if near(p, TEXT_BGRA, tol=24): continue
                intermediate += 1
        if intermediate < 4:
            print(f"FAIL: only {intermediate} intermediate-alpha pixels "
                  f"in the label band; expected >= 4")
            print(f"      WM may have fallen back to the bitmap font, "
                  f"which means it couldn't reach fontd over IPC")
            return 1
        print(f"PASS: {intermediate} intermediate-alpha pixels found "
              f"(WM -> fontd IPC working, AA glyphs flowing)")

        print()
        print("CHAPTER 108B: USERSPACE FONT SERVER TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=2.0)
        except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

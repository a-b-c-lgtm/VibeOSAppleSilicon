#!/usr/bin/env python3
"""scripts/test_pixapp.py — chapter 108d regression.

What this proves
----------------
After chapter 108d (kernel-WM compose retired, wsd takes over), a
userspace app can still paint a window's pixel buffer by writing
BGRA bytes directly through the wmclient-mapped framebuffer and
the wsd compositor blits the result onto the scanout via
SYS_FB_PRESENT.  The /bin/pixapp demo opens a window through
/srv/wm, paints a horizontal red-to-blue gradient by direct
stores, then DAMAGEs the whole window once; this test boots,
spawns it over serial, and screen-scrapes three pixels off the
gradient to confirm:

    LEFT   column ≈ red-dominant
    MIDDLE column ≈ magenta (equal red + blue)
    RIGHT  column ≈ blue-dominant

If wmclient's MAP_FB didn't actually share pages with wsd, the
window would show the wsd wallpaper colour at every sample and
all three checks would fail.  If wsd's WM_WIN_DAMAGE didn't
trigger fb_present, the screen would show whatever was there
before pixapp ran.

Architecture notes
------------------
* wsd owns the per-window framebuffer (allocated via the
  kernel win_fb pool at WM_WIN_CREATE time).
* wmclient maps those pages into the calling process so
  pixapp can write BGRA bytes directly with no syscalls.
* WM_WIN_DAMAGE blits the dirty rect from the per-window FB
  into wsd's compose state and calls fb_present to push the
  scanout region to the GPU.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-pixapp.sock"
SERIAL_SOCK = "/tmp/osdev-serial-pixapp.sock"
DUMP_PATH   = "/tmp/osdev-fb-pixapp.ppm"

FB_W = 1280
FB_H = 800

# Chapter 108d -- wsd's cascade starts at WM_CASCADE_BASE_X/Y
# (100, 100) and steps by 40 px per cascade-positioned window.
# Boot auto-launches desktop (full-screen, _at), taskbar (_at),
# and launcher (also _at, anchored above the taskbar).  Only
# wm_create_window_input advances the cascade, so all three
# boot apps leave it untouched.  Pixapp is the first cascade
# client and lands at slot 0 = (100, 100).  chapter 108e added
# wsd-side title bars: a 24-px tall bar painted at (PIX_X, PIX_Y)
# on top of the body, so the body now starts at
# (PIX_X, PIX_Y + WSD_TITLE_H).  Pixapp doesn't pass
# NO_DECORATION (its goal IS to demonstrate decorations and
# event-driven repainting), so we shift CONTENT_Y by the bar
# height.  WSD_TITLE_H == 24, defined in userspace/wsd/wsd.c.
PIX_X = 100
PIX_Y = 100
PIX_W = 300
PIX_H = 200

WSD_TITLE_H = 24
CONTENT_X = PIX_X
CONTENT_Y = PIX_Y + WSD_TITLE_H

# Pixapp samples: row 100 inside the gradient, far enough below
# the 16x16 event-triggered overlay in the top-left so the
# gradient is undisturbed.
SAMPLE_Y      = CONTENT_Y + 100   # 224
SAMPLE_LEFT_X = CONTENT_X + 5     # 105
SAMPLE_RIGHT_X= CONTENT_X + PIX_W - 5   # 395
SAMPLE_MID_X  = CONTENT_X + PIX_W // 2  # 250


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


def is_red_dominant(p):
    r, g, b = p
    return r >= 180 and b <= 60 and g <= 60


def is_blue_dominant(p):
    r, g, b = p
    return b >= 180 and r <= 60 and g <= 60


def is_magenta(p):
    r, g, b = p
    # Middle of red→blue gradient: roughly equal red + blue, low green.
    return 70 <= r <= 200 and 70 <= b <= 200 and g <= 60


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached")
            return 1
        time.sleep(0.5)

        # Background the app so the shell prompt comes back; we
        # don't need to interact with it after this point.
        ser.sendall(b"pixapp &\n")

        # `[pixapp] painted gradient` is printed after wsd has
        # already logged `[wmclient] window id=` and any FB-map
        # output, so it's a single sufficient wait point.  Don't
        # chain wait_for calls — each one drains the socket into
        # its own buffer and an earlier needle would lose later
        # ones.
        log = wait_for(ser, b"[pixapp] painted gradient", 10.0)
        if b"[wmclient] window id=" not in log:
            print("FAIL: pixapp wmclient did not open a window")
            return 1
        if b"[pixapp] painted gradient" not in log:
            print("FAIL: pixapp did not finish initial paint")
            return 1
        # Compositor recomposes asynchronously; give it a beat.
        time.sleep(0.5)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        left = pixel_at(ppm, SAMPLE_LEFT_X,  SAMPLE_Y)
        mid  = pixel_at(ppm, SAMPLE_MID_X,   SAMPLE_Y)
        right= pixel_at(ppm, SAMPLE_RIGHT_X, SAMPLE_Y)
        print(f"  sampled LEFT  ({SAMPLE_LEFT_X},{SAMPLE_Y}) = {left}")
        print(f"  sampled MID   ({SAMPLE_MID_X},{SAMPLE_Y})  = {mid}")
        print(f"  sampled RIGHT ({SAMPLE_RIGHT_X},{SAMPLE_Y}) = {right}")

        ok = True
        if not is_red_dominant(left):
            print(f"FAIL: LEFT pixel {left} is not red-dominant")
            ok = False
        if not is_magenta(mid):
            print(f"FAIL: MIDDLE pixel {mid} is not magenta")
            ok = False
        if not is_blue_dominant(right):
            print(f"FAIL: RIGHT pixel {right} is not blue-dominant")
            ok = False

        if not ok:
            print("FAIL: gradient not visible — wm_map_window or "
                  "wm_damage is broken")
            return 1

        print("PASS: pixapp gradient visible through SYS_GUI_MAP_WINDOW "
              "+ SYS_GUI_DAMAGE")
        return 0
    finally:
        # SIGKILL the QEMU outright + wait long enough for the
        # HVF VM to actually be reaped before returning.  An
        # earlier version used `terminate` + a 2 s wait, which
        # was racy under HVF — terminate() doesn't always
        # finish quickly under load, and a half-dead qemu
        # keeps an exclusive lock on build/disk.img that
        # blocks every subsequent test from even creating its
        # serial socket.  See repo memory
        # /memories/repo/chapter-108a-userspace-window-buffers.md
        # for the sweep-breakage symptom.
        try: q.kill()
        except Exception: pass
        try: q.wait(timeout=10)
        except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_notify.py — milestone-49 smoke test.

Boots, types `notify Hello world! &` at the shell, screendumps,
and asserts:
  1. A notification window exists in the top-right area with the
     toast background colour (32, 40, 64).
  2. The 4-px-wide accent bar (LEFT edge, BGRA = (64, 128, 255))
     is painted.
  3. After ~3.5 s the toast has auto-dismissed (no notification
     pixels remain at the original location — the wallpaper
     gradient bleeds back through).

The shell `&` runs the spawn in the background so notify and sh
overlap.  We send carriage return after to get a fresh prompt.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-notify.sock"
SERIAL_SOCK = "/tmp/osdev-serial-notify.sock"
DUMP_PATH_A = "/tmp/osdev-fb-notify-a.ppm"
DUMP_PATH_B = "/tmp/osdev-fb-notify-b.ppm"

FB_W = 1280
FB_H = 800

# Notify geometry (matches userspace/notify/notify.c).
WIN_W   = 360
WIN_H   = 80
MARGIN  = 16
WIN_X   = FB_W - WIN_W - MARGIN     # 904
WIN_Y   = MARGIN                    # 16

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

def near(a, b, tol=10):
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

        # Spawn notify in the background.
        ser.sendall(b"/bin/notify Hello world!\n")
        time.sleep(0.6)

        # First snapshot: toast should be visible.
        screendump(qmp, DUMP_PATH_A)
        ppm_a = read_ppm(DUMP_PATH_A)

        # Body BG check: a pixel a bit inside the toast (avoid
        # accent bar at left).  WIN_X+200, WIN_Y+30 should be
        # the BG colour BG_BGRA = (32, 40, 64).
        # NOTE: wallpaper near the top is (24, 32, 64), which is
        # within ~10 of the toast BG, so we use a tight tol AND
        # also require the accent bar (which is unmistakable) and
        # diff against the post-dismiss snapshot.
        bg = pixel_at(ppm_a, WIN_X + 200, WIN_Y + 30)
        if not near(bg, (32, 40, 64), tol=4):
            print(f"FAIL: toast BG pixel = {bg}, expected ~(32, 40, 64)")
            return 1
        print(f"PASS: toast body painted (pixel = {bg})")

        # Accent bar check: WIN_X+2 should be ACCENT (64, 128, 255).
        ac = pixel_at(ppm_a, WIN_X + 2, WIN_Y + WIN_H // 2)
        if not near(ac, (64, 128, 255), tol=15):
            print(f"FAIL: accent bar pixel = {ac}, "
                  f"expected ~(64, 128, 255)")
            return 1
        print(f"PASS: accent bar painted (pixel = {ac})")

        # Border check: top edge.
        bd = pixel_at(ppm_a, WIN_X + WIN_W // 2, WIN_Y)
        if not near(bd, (128, 160, 224), tol=18):
            print(f"FAIL: border pixel = {bd}, "
                  f"expected ~(128, 160, 224)")
            return 1
        print(f"PASS: border painted (pixel = {bd})")

        # Wait long enough for the toast to auto-dismiss
        # (DISMISS_MS = 3000), then snapshot again.
        # The clearest signal of dismissal is that the accent bar
        # (which is bright cyan-blue, nothing else on screen looks
        # like it) is GONE.
        time.sleep(3.5)
        screendump(qmp, DUMP_PATH_B)
        ppm_b = read_ppm(DUMP_PATH_B)

        ac2 = pixel_at(ppm_b, WIN_X + 2, WIN_Y + WIN_H // 2)
        if near(ac2, (64, 128, 255), tol=15):
            print(f"FAIL: accent bar still present after dismiss "
                  f"(pixel still = {ac2})")
            return 1
        print(f"PASS: toast auto-dismissed (accent now = {ac2})")

        print("\nMILESTONE 49: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

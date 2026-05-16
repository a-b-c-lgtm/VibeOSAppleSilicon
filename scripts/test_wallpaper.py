#!/usr/bin/env python3
"""scripts/test_wallpaper.py — milestone-50 smoke test.

Boots, waits for shell, screendumps the framebuffer, and asserts
that the wallpaper painted by the userspace /bin/desktop process
is visible (i.e. not the kernel's gradient fallback).

Strategy: pick a handful of pixels at known positions and assert
each one is materially different from the corresponding pixel of
the gradient fallback at that position.  We don't need to know
the exact image contents; we just need to verify that something
OTHER than the gradient is on screen.

The gradient (kernel/core/wm.c paint_wallpaper) is a smooth
top-to-bottom interpolation between two dark blue-grey shades,
so any sufficiently colourful image (jpegs of flowers, etc) will
yield pixels far away from the gradient at most positions.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-wp.sock"
SERIAL_SOCK = "/tmp/osdev-serial-wp.sock"
DUMP_PATH   = "/tmp/osdev-fb-wallpaper.ppm"

FB_W = 1920
FB_H = 1080

# Gradient endpoints from kernel/core/wm.c paint_wallpaper.
# top:    (24, 32, 64)   (BGRA: 0x40, 0x20, 0x18)
# bottom: (40, 56, 96)   (BGRA: 0x60, 0x38, 0x28)

def gradient_at(y):
    t = y / float(FB_H - 1)
    r = int(24 + (40 - 24) * t)
    g = int(32 + (56 - 32) * t)
    b = int(64 + (96 - 64) * t)
    return (r, g, b)

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

def pixel_at(ppm, x, y):
    w, h, data = ppm
    o = (y * w + x) * 3
    return data[o], data[o+1], data[o+2]

def colour_distance(a, b):
    return sum(abs(int(x) - int(y)) for x, y in zip(a, b))


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell prompt reached")

        # Give /bin/desktop a moment to finish blitting the
        # 4 MB wallpaper.  4 MB / (32 rows * 5 KB/row) = ~25
        # gui_present syscalls; should complete in well under 1 s.
        time.sleep(1.5)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)
        if (ppm[0], ppm[1]) != (FB_W, FB_H):
            print(f"FAIL: bad framebuffer size {ppm[0]}x{ppm[1]}")
            return 1

        # Sample 6 points spread across the screen, avoiding
        # known-occupied regions: the launcher (small window,
        # default position upper-left) and the taskbar (bottom
        # 28 px).  We sample across the wider 1920x1080 area.
        samples = [
            (480,  100),
            (960,  150),
            (1440, 200),
            (480,  600),
            (1440, 600),
            (960,  900),
        ]
        far_count = 0
        for (x, y) in samples:
            actual = pixel_at(ppm, x, y)
            grad   = gradient_at(y)
            d      = colour_distance(actual, grad)
            tag = "DIFFERS" if d > 30 else "matches gradient"
            print(f"  ({x:4d},{y:3d}): actual={actual} grad={grad} d={d}  [{tag}]")
            if d > 30:
                far_count += 1

        if far_count < 4:
            print(f"FAIL: only {far_count}/6 sample pixels diverge "
                  f"from gradient — desktop wallpaper may not be "
                  f"painted (kernel fallback still visible?)")
            return 1
        print(f"PASS: {far_count}/6 sample pixels diverge from "
              f"gradient — userspace wallpaper visible")

        # Sanity: prompt still alive (no panic during blit).
        ser.sendall(b"echo wp-ok\n")
        out = wait_for(ser, b"wp-ok", 5.0)
        if b"wp-ok" not in out:
            print("FAIL: shell unresponsive after wallpaper blit")
            return 1
        print("PASS: shell still responsive after wallpaper blit")

        print("\nMILESTONE 50: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

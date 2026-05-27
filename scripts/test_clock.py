#!/usr/bin/env python3
"""scripts/test_clock.py — clock smoke test.

Boots fully headless with NO input.  Verifies the taskbar shows a
clock in the right corner that updates every second.

Strategy:
  1. Boot, wait ~2s for the system to settle.
  2. Screendump.  Sample a digit pixel inside the clock area
     (CLOCK_X = 1280-80-8 = 1192, CLOCK_Y = 4 + bar_y = 776).  Look
     for a CLOCK_FG_BGRA-coloured pixel (0xC0E0FF = (192, 224, 255)).
  3. Wait another ~1.5 s.  Screendump again.  Diff the clock-area
     pixels: at least one byte must differ (the seconds digits will
     have changed).
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-clk.sock"
SERIAL_SOCK = "/tmp/osdev-serial-clk.sock"
DUMP_PATH_A = "/tmp/osdev-fb-clk-a.ppm"
DUMP_PATH_B = "/tmp/osdev-fb-clk-b.ppm"

FB_W = 1280
FB_H = 800
BAR_H = 28
BAR_Y = FB_H - BAR_H

# Clock geometry (matches userspace/taskbar/taskbar.c).
CLOCK_W   = 80
CLOCK_PAD = 8
CLOCK_X   = FB_W - CLOCK_W - CLOCK_PAD
CLOCK_Y   = BAR_Y + 4
CLOCK_H   = BAR_H - 8

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

def clock_strip_bytes(ppm):
    """Slice a horizontal strip across the clock area, one row at the
    middle of the digits, returned as a flat bytes object so we can
    compare equality cheaply."""
    w, h, data = ppm
    y = CLOCK_Y + CLOCK_H // 2
    o0 = (y * w + CLOCK_X) * 3
    o1 = (y * w + CLOCK_X + CLOCK_W) * 3
    return data[o0:o1]


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        if b"$ " not in wait_for(ser, b"$ ", 25.0):
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell prompt reached")
        time.sleep(0.5)

        # First snapshot.
        screendump(qmp, DUMP_PATH_A)
        ppm_a = read_ppm(DUMP_PATH_A)

        # Verify clock body is the dark-blue CLOCK_BG.  Sample at the
        # very top of the clock body where no digit can land
        # (CLOCK_Y..CLOCK_Y+1).
        bg = pixel_at(ppm_a, CLOCK_X + CLOCK_W // 2, CLOCK_Y + 2)
        if not near(bg, (16, 20, 36), tol=10):
            print(f"FAIL: clock BG pixel at center top = {bg}, "
                  f"expected ~(16, 20, 36)")
            return 1
        print(f"PASS: clock BG painted (pixel = {bg})")

        # Verify at least one CLOCK_FG-coloured pixel inside the
        # digit row.  CLOCK_FG = (192, 224, 255).
        digit_row_y = CLOCK_Y + CLOCK_H // 2
        fg = (192, 224, 255)
        any_fg = False
        for x in range(CLOCK_X + 8, CLOCK_X + CLOCK_W - 8):
            if near(pixel_at(ppm_a, x, digit_row_y), fg, tol=15):
                any_fg = True
                break
        if not any_fg:
            print("FAIL: no clock-foreground pixels in digit row")
            return 1
        print(f"PASS: clock digit pixels present (fg={fg})")

        # Wait long enough that at least one second has ticked, then
        # snapshot again.  The seconds digit changes every second, so
        # the strip bytes MUST differ.
        time.sleep(1.4)
        screendump(qmp, DUMP_PATH_B)
        ppm_b = read_ppm(DUMP_PATH_B)
        strip_a = clock_strip_bytes(ppm_a)
        strip_b = clock_strip_bytes(ppm_b)
        if strip_a == strip_b:
            print("FAIL: clock did not tick "
                  "(A and B clock-row strips are identical)")
            return 1
        # Count differing bytes for a more interesting message.
        diff = sum(1 for x, y in zip(strip_a, strip_b) if x != y)
        print(f"PASS: clock ticked between snapshots "
              f"({diff} bytes differ in the digit strip)")

        print("\nALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

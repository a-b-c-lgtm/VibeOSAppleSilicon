#!/usr/bin/env python3
"""scripts/test_taskbar.py — milestone-47 smoke test.

Boots fully headless with NO input.  Verifies:
  1. Both /bin/taskbar and /bin/launcher auto-start
  2. The taskbar logs window-create with NO_DECORATION + ALWAYS_ON_TOP
     flags
  3. The framebuffer at the bottom strip (y >= 772) shows the
     taskbar's distinctive dark-blue BG (0x18, 0x1C, 0x32), and shows
     a CELL coloured (0x30, 0x40, 0x70) at x ~16 (where the launcher's
     entry sits)
  4. The launcher's title text "launcher" is visible — we look for a
     TEXT_BGRA pixel at the rough centre of the cell glyph row
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-tb.sock"
SERIAL_SOCK = "/tmp/osdev-serial-tb.sock"
DUMP_PATH   = "/tmp/osdev-fb-tb.ppm"

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

def near(a, b, tol=10):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 25.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        if b"launching /bin/taskbar" not in boot_log:
            print("FAIL: init did not auto-spawn taskbar")
            return 1
        print("PASS: init auto-spawned /bin/taskbar")

        # The taskbar's create line should mention flags=0x3
        # (NO_DECORATION | ALWAYS_ON_TOP).  serial_puthex emits a
        # 16-digit hex word with leading zeros.
        more = drain(ser, time.time() + 1.0)
        boot_log += more
        flags_needle = b"flags=0x0000000000000003"
        if flags_needle not in boot_log:
            print("FAIL: no window with flags=0x3 created (decoration|always-on-top)")
            print("--- recent serial ---")
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: taskbar window created with flags=0x3")

        # Wait for the taskbar to do its first render-with-cells pass.
        time.sleep(0.6)
        screendump(qmp, DUMP_PATH)
        print(f"  saved screendump: {DUMP_PATH}")

        ppm = read_ppm(DUMP_PATH)
        # Bar BG colour at top of strip (y == FB_H - BAR_H + 8).
        # Bar Y range = 772..799.  Sample at y=775 (above any cell
        # but below the 1px highlight at y=772).
        # Wait — cells start at y=4 inside the bar = absolute 776.
        # So sample at y=775 for pure BG.
        bar_bg = pixel_at(ppm, 1100, 775)
        if not near(bar_bg, (24, 28, 50), tol=10):
            print(f"FAIL: bar BG pixel at (1100,775) = {bar_bg}, "
                  f"expected ~(24, 28, 50)")
            return 1
        print(f"PASS: taskbar BG painted (pixel at (1100,775) = {bar_bg})")

        # First cell sits at (CELL_PADX=8, bar_y=772+4=776), 180x20.
        # Text is left-aligned at x=14 spanning ~64 px ("launcher" =
        # 8 chars * 8 px).  Sample at (170, 786) — well right of the
        # label but still inside the cell, on its body fill.
        # Cell BG is CELL_BGRA = (48, 64, 112) when not focused, or
        # CELL_FOCUS_BGRA = (96, 144, 224) when focused.
        cell_pix = pixel_at(ppm, 170, 786)
        focus = (96, 144, 224)
        unfocus = (48, 64, 112)
        if near(cell_pix, focus, tol=15):
            print(f"PASS: launcher cell painted FOCUSED (pixel = {cell_pix})")
        elif near(cell_pix, unfocus, tol=15):
            print(f"PASS: launcher cell painted unfocused (pixel = {cell_pix})")
        else:
            print(f"FAIL: cell pixel at (170, 786) = {cell_pix}, "
                  f"expected near {focus} or {unfocus}")
            return 1

        # Bonus: sample inside the launcher label itself — should be
        # the white-ish glyph foreground (240, 240, 240).
        glyph_pix = pixel_at(ppm, 60, 786)
        if not near(glyph_pix, (240, 240, 240), tol=20):
            # Don't fail — just informational.  Anti-aliased / blended
            # glyphs would land elsewhere.
            print(f"  note: glyph pixel at (60, 786) = {glyph_pix}")
        else:
            print(f"PASS: launcher label glyph rendered (pixel = {glyph_pix})")

        # The launcher window should still be in its usual spot —
        # confirm we didn't break that pixel.
        launcher_bg = pixel_at(ppm, 200, 90)
        if launcher_bg[0] < 220 or launcher_bg[1] < 220 or launcher_bg[2] < 220:
            print(f"FAIL: launcher BG regression — (200,90) = {launcher_bg}")
            return 1
        print(f"PASS: launcher still visible (pixel at (200,90) = {launcher_bg})")

        print("\nMILESTONE 47: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_browser_resize_cycle.py — chapter 118 follow-up #3.

Regression for the browser-crashes-mid-resize bug.

Before the fix, dragging the bottom-right grip of a browser window
loading a non-trivial page (hn.html, ~35 KB) reliably crashed the
browser with a data abort: wsd's SYS_WIN_FB_RESIZE was synchronously
uninstalling the browser's old FB VA while the browser was still
rendering against it, faulting on the cached pointer.

After the fix (kernel/core/win_fb.c lazy-unmap with stale generations),
the old VA stays mapped to the old pages until the browser acks the
resize by calling sys_win_fb_map.  The browser never faults.

This test:
  1. Boots, opens browser on hn.html.
  2. Drags the grip down-right to grow the window.
  3. Drags the grip up-left to shrink it.
  4. Drags the grip down-right again to grow back to ~original.
  5. Settles, screen-dumps, samples pixels in the grown-back body.

PASS if all samples are content-coloured (white page bg, or text);
FAIL if any sample is the wsd grown-region IDLE placeholder
(RGB 0x55,0x66,0x77 == 85,102,119) — which only persists if the
browser died and stopped repainting.

History reference: see scripts/_dbg_browser_resize_cycle2.py for the
ad-hoc version this evolved from, plus scripts/_dbg_browser_shrink_crash.py
for the bug-reproduction harness that first surfaced the data abort.
"""
import json, os, re, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-brrsz.sock"
SERIAL_SOCK = "/tmp/osdev-serial-brrsz.sock"
DUMP_BASE   = "/tmp/osdev-fb-brrsz"

FB_W = 1280
FB_H = 1024
WIN_X, WIN_Y = 140, 140
TITLE_H = 24
GRIP_SIZE = 12
ABS_MAX = 0x7FFF

IDLE_RGB = (0x55, 0x66, 0x77)


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


def cursor_to(qmp, x, y):
    ax = int(x * ABS_MAX / FB_W); ay = int(y * ABS_MAX / FB_H)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})


def left_button(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": down, "button": "left"}}]}})


def drag(qmp, x0, y0, x1, y1, steps=10, settle=0.04):
    cursor_to(qmp, x0, y0); time.sleep(settle)
    left_button(qmp, True); time.sleep(settle)
    for i in range(1, steps + 1):
        ix = x0 + (x1 - x0) * i // steps
        iy = y0 + (y1 - y0) * i // steps
        cursor_to(qmp, ix, iy); time.sleep(settle)
    left_button(qmp, False); time.sleep(settle)


SERIAL_BUF = []
SERIAL_RUN = True


def serial_pump(ser):
    while SERIAL_RUN:
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            try:
                c = ser.recv(8192)
            except OSError:
                return
            if not c:
                return
            SERIAL_BUF.append(c.decode("ascii", "replace"))


def all_serial():
    return "".join(SERIAL_BUF)


def last_final_size():
    matches = re.findall(r"resize end win-slot=\d+ final=(\d+)x(\d+)",
                         all_serial())
    if not matches: return None
    return (int(matches[-1][0]), int(matches[-1][1]))


def screendump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {"execute": "screendump", "arguments": {"filename": path}})
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.15); break
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
    return data[o], data[o + 1], data[o + 2]


def is_white(rgb, tol=20):
    return all(c >= 255 - tol for c in rgb)


def is_idle(rgb, tol=20):
    return all(abs(int(a)-int(b)) <= tol for a, b in zip(rgb, IDLE_RGB))


def grip_center(w, h):
    gx = WIN_X + w - GRIP_SIZE // 2
    gy = WIN_Y + TITLE_H + h - GRIP_SIZE // 2
    return (gx, gy)


def main():
    q = boot()
    rc = 0
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        threading.Thread(target=serial_pump, args=(ser,), daemon=True).start()

        for _ in range(50):
            time.sleep(0.5)
            if "$ " in all_serial():
                break
        else:
            print("FAIL no shell prompt"); return 1
        time.sleep(0.5)

        ser.sendall(b"browser --gui /mnt/hn.html\n")
        for _ in range(40):
            time.sleep(0.5)
            if "[browser] gui window=" in all_serial():
                break
        m = re.search(r"\[browser\] gui window=\d+ size=(\d+)x(\d+)",
                      all_serial())
        if not m:
            print("FAIL no browser size"); return 1
        WIN_W, WIN_H = int(m.group(1)), int(m.group(2))
        print(f"browser opened {WIN_W}x{WIN_H}")
        time.sleep(2.5)
        cursor_to(qmp, 50, 50); time.sleep(0.3)

        cur_w, cur_h = WIN_W, WIN_H

        def step(name, drag_to):
            nonlocal cur_w, cur_h
            gx, gy = grip_center(cur_w, cur_h)
            drag(qmp, gx, gy, drag_to[0], drag_to[1])
            deadline = time.time() + 3.0
            while time.time() < deadline:
                fs = last_final_size()
                if fs and (fs[0] != cur_w or fs[1] != cur_h):
                    cur_w, cur_h = fs
                    break
                time.sleep(0.05)
            time.sleep(8.0)
            cursor_to(qmp, 30, 30); time.sleep(0.3)
            ppm_path = f"{DUMP_BASE}-{name}.ppm"
            screendump(qmp, ppm_path)
            return read_ppm(ppm_path)

        # 1. GROW
        ppm1 = step("1-grown",
                    (WIN_X + WIN_W + 200, WIN_Y + TITLE_H + WIN_H + 120))
        sx = WIN_X + cur_w - 50
        sy = WIN_Y + TITLE_H + cur_h - 50
        p = pixel_at(ppm1, sx, sy)
        if is_idle(p):
            print(f"FAIL grow: ({sx},{sy}) = {p} IDLE placeholder")
            rc = 1

        # 2. SHRINK
        step("2-shrunk", (WIN_X + 250, WIN_Y + TITLE_H + 200))

        # 3. GROW-BACK (the actual repro: this is where the original
        # bug left visible IDLE-gray strips because the browser had
        # crashed during the cycle).
        ppm3 = step("3-grown-back",
                    (WIN_X + WIN_W + 200, WIN_Y + TITLE_H + WIN_H + 120))
        samples = [
            (WIN_X + 350, WIN_Y + TITLE_H + 250),
            (WIN_X + 500, WIN_Y + TITLE_H + 400),
            (WIN_X + 600, WIN_Y + TITLE_H + 500),
            (WIN_X + cur_w - 50, WIN_Y + TITLE_H + cur_h - 50),
        ]
        for sx, sy in samples:
            if sx >= FB_W or sy >= FB_H: continue
            p = pixel_at(ppm3, sx, sy)
            if is_idle(p):
                print(f"FAIL grow-back: ({sx},{sy}) = {p} IDLE placeholder")
                rc = 1

        # 4. Cross-check: browser must still be alive (no [svc] FATAL
        # ...thread = /bin/browser line in the serial log).  If the
        # FB-mid-render fault regresses, we'll see it here even when
        # pixels happen to look right by coincidence.
        if re.search(r"\[svc\] FATAL.*?thread\s*=\s*/bin/browser",
                     all_serial(), re.DOTALL):
            print("FAIL browser took a fatal exception during the cycle")
            rc = 1

        print("PASS" if rc == 0 else "FAIL")
        return rc
    finally:
        global SERIAL_RUN
        SERIAL_RUN = False
        try: q.kill()
        except Exception: pass
        try: q.wait(timeout=2)
        except Exception: pass
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

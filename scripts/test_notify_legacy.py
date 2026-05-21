#!/usr/bin/env python3
"""scripts/test_notify_legacy.py — chapter 108c kernel-draw exhibit.

The chapter 108c migration moved most apps off the legacy
gui_fill_rect / gui_draw_text / gui_present syscalls and onto a
userspace mapping + libgui/draw.h.  Once the kernel started
returning -EBUSY for any mapped window that also tried to use
the syscall path, it would have been easy to delete the syscall
path entirely.  We deliberately did not: notify is still drawn
via wm_fill_rect and wm_draw_text in kernel/core/wm.c.

The reasoning, recorded in chapter 108c:

  - Notify is a one-shot rectangle with three colours and one
    line of text.  Mapping a pixel buffer just to paint 360x80
    pixels for 3 seconds is overhead, not insight.
  - The kernel-draw path has to keep working for the WM's own
    title bars (drawn from inside the WM, not by the owning
    app) and for any future kernel-emitted UI (panic toasts,
    boot splash, etc).  Keeping notify on that path gives us a
    real userspace caller for it, not just an internal one.
  - The chapter teaches the contrast: notify is what kernel
    draw looks like, hellogui/launcher/taskbar/paint/desktop
    is what userspace draw looks like.  Without the contrast
    the chapter is a one-sided pitch.

This test boots, spawns /bin/notify, and asserts that the toast
appears with the expected accent-bar and body-background pixels.
It is intentionally a near-twin of scripts/test_notify.py: the
purpose is to verify that the chapter 108c -EBUSY enforcement
did NOT regress the legacy path that notify still depends on.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-notifyleg.sock"
SERIAL_SOCK = "/tmp/osdev-serial-notifyleg.sock"
DUMP_PATH   = "/tmp/osdev-fb-notifyleg.ppm"

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

        # Spawn notify.  The toast uses kernel-side draw under
        # the covers — gui_fill_rect for the BG, accent bar, and
        # border; gui_draw_text for the label.  After chapter
        # 108c those calls are still legal because notify never
        # calls gui_window_fb.
        ser.sendall(b"/bin/notify Chapter108c\n")
        time.sleep(0.6)

        screendump(qmp, DUMP_PATH)
        ppm = read_ppm(DUMP_PATH)

        # Body BG pixel: well inside the toast, clear of the
        # accent bar.  BG_BGRA = (32, 40, 64).  The chapter
        # 108c -EBUSY enforcement in wm_fill_rect should NOT
        # have refused this fill, because notify hasn't mapped
        # the window.
        bg = pixel_at(ppm, WIN_X + 200, WIN_Y + 30)
        if not near(bg, (32, 40, 64), tol=4):
            print(f"FAIL: legacy kernel-draw path BROKEN — body pixel = "
                  f"{bg}, expected ~(32, 40, 64)")
            return 1
        print(f"PASS: kernel wm_fill_rect still paints body (pixel = {bg})")

        # Accent bar pixel: hard signal because nothing else on
        # screen is bright cyan-blue.
        ac = pixel_at(ppm, WIN_X + 2, WIN_Y + WIN_H // 2)
        if not near(ac, (64, 128, 255), tol=15):
            print(f"FAIL: legacy kernel-draw path BROKEN — accent bar = "
                  f"{ac}, expected ~(64, 128, 255)")
            return 1
        print(f"PASS: kernel wm_fill_rect still paints accent (pixel = {ac})")

        print("\nCHAPTER 108c LEGACY EXHIBIT: notify still draws via kernel")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

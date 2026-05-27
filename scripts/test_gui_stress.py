#!/usr/bin/env python3
"""scripts/test_gui_stress.py — open a bunch of windows in
sequence and verify the kernel doesn't panic.

Reproduces the interactive sequence that crashed the user during
desktop-shell testing: boot, then click each launcher button to spawn
gui_term, paint, and notepad in turn.  Asserts no KERNEL PANIC
appears in the serial log throughout.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-stress.sock"
SERIAL_SOCK = "/tmp/osdev-serial-stress.sock"

FB_W = 1920
FB_H = 1080

# Launcher is the FIRST regular (non-pinned) window the WM creates;
# it lands at the cascade origin (80, 60), 240 wide x 180 tall, 24-px
# title bar.  Buttons are 36 px high, 8 px gap, starting 16 px below
# the title bar.
WIN_X, WIN_Y = 80, 60
WIN_W, WIN_H = 240, 180
TITLE_H      = 24

ABS_MAX = 0x7FFF

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

def screen_to_abs(x, y):
    return (int(x * ABS_MAX / FB_W), int(y * ABS_MAX / FB_H))

def move_to(qmp, x, y):
    ax, ay = screen_to_abs(x, y)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})

def left_click(qmp, x, y):
    move_to(qmp, x, y)
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": True,  "button": "left"}},
    ]}})
    time.sleep(0.05)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}},
    ]}})

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

def assert_no_panic(buf):
    if b"KERNEL PANIC" in buf:
        idx = buf.find(b"KERNEL PANIC")
        ctx = buf[max(0, idx - 200): idx + 1500]
        print("FAIL: kernel panic")
        print("--- serial context around panic ---")
        print(ctx.decode("ascii","replace"))
        return False
    return True


def main():
    q = boot()
    log = b""
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        log += wait_for(ser, b"$ ", 25.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-1500:].decode("ascii","replace"))
            return 1
        if not assert_no_panic(log): return 1
        print("PASS: shell ready, no panic during boot")

        # The launcher is auto-spawned by init.  Three buttons:
        #   button 0 (gui_term):  centre y = TITLE_H + 16 + 18 = 58
        #   button 1 (paint):                + (36+8)         = 102
        #   button 2 (notepad):              + (36+8)*2       = 146
        cx = WIN_X + WIN_W // 2
        for label, dy in (("gui_term", 58),
                          ("paint",    102),
                          ("notepad",  146)):
            print(f"  clicking launcher button: {label}")
            left_click(qmp, cx, WIN_Y + dy)
            log += wait_for(ser, b"[wm] window created", 5.0)
            log += drain(ser, time.time() + 1.0)
            if not assert_no_panic(log): return 1

        # Move the cursor around the screen, click random spots.
        # If anything in the focus / paint / event-delivery path
        # gets confused with 6+ windows, it will surface here.
        print("  exercising mouse: random clicks")
        for (mx, my) in [(400, 500), (1000, 600), (1500, 400),
                         (200, 200), (1700, 300)]:
            move_to(qmp, mx, my)
            time.sleep(0.05)
        log += drain(ser, time.time() + 0.5)
        if not assert_no_panic(log): return 1

        # Final settle.
        log += drain(ser, time.time() + 1.5)
        if not assert_no_panic(log): return 1

        print("PASS: all 3 launcher children spawned without panic")
        print("\nGUI STRESS: PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

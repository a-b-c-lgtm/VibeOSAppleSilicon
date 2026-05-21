#!/usr/bin/env python3
"""scripts/_dbg_browser_shrink_crash.py — capture the kernel
panic / wsd assertion that happens during browser shrink.

Boots, opens browser, grows, then shrinks; teeing all serial
output to stdout so we can see the kernel oops / log line that
killed QEMU.
"""
import json, os, select, socket, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-shr.sock"
SERIAL_SOCK = "/tmp/osdev-serial-shr.sock"

FB_W = 1280
FB_H = 1024
WIN_X, WIN_Y = 140, 140
TITLE_H = 24
GRIP_SIZE = 12
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
        try:
            m = qrl(qmp)
            if "return" in m or "error" in m: return m
        except RuntimeError:
            return None


def cursor_to(qmp, x, y):
    ax = int(x * ABS_MAX / FB_W); ay = int(y * ABS_MAX / FB_H)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})


def left_button(qmp, down):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": down, "button": "left"}}]}})


def drag(qmp, x0, y0, x1, y1, steps=14, settle=0.04):
    cursor_to(qmp, x0, y0); time.sleep(settle)
    left_button(qmp, True); time.sleep(settle)
    for i in range(1, steps + 1):
        ix = x0 + (x1 - x0) * i // steps
        iy = y0 + (y1 - y0) * i // steps
        cursor_to(qmp, ix, iy); time.sleep(settle)
    left_button(qmp, False); time.sleep(settle)


SERIAL_BUF = []
SERIAL_RUN = True
SERIAL_FH = open('/tmp/sh_serial_raw.log', 'wb')
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
            SERIAL_FH.write(c); SERIAL_FH.flush()
            text = c.decode('ascii', 'replace')
            SERIAL_BUF.append(text)


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        pump = threading.Thread(target=serial_pump, args=(ser,), daemon=True)
        pump.start()

        # Wait for shell prompt
        for _ in range(50):
            time.sleep(0.5)
            if any("$ " in b for b in SERIAL_BUF[-3:]):
                break
        print("=== shell up ===")
        time.sleep(0.5)

        ser.sendall(b"browser --gui /mnt/test_layout.html\n")
        for _ in range(40):
            time.sleep(0.5)
            if any("[browser] gui window=" in b for b in SERIAL_BUF):
                break
        print("=== browser opened ===")

        import re
        all_text = "".join(SERIAL_BUF)
        m = re.search(r"\[browser\] gui window=\d+ size=(\d+)x(\d+)", all_text)
        if not m:
            print("FAIL no browser size")
            return 1
        WIN_W = int(m.group(1)); WIN_H = int(m.group(2))
        print(f"=== browser opened {WIN_W}x{WIN_H} ===")
        time.sleep(2.0)
        cursor_to(qmp, 50, 50); time.sleep(0.3)

        # GROW
        grip_cx = WIN_X + WIN_W - GRIP_SIZE // 2
        grip_cy = WIN_Y + TITLE_H + WIN_H - GRIP_SIZE // 2
        new_grip_cx = grip_cx + 200
        new_grip_cy = grip_cy + 120
        print(f"=== GROW drag ({grip_cx},{grip_cy}) -> "
              f"({new_grip_cx},{new_grip_cy}) ===")
        drag(qmp, grip_cx, grip_cy, new_grip_cx, new_grip_cy)
        time.sleep(2.0)

        # SHRINK -- this is the crash repro.
        GROWN_W = WIN_W + 200; GROWN_H = WIN_H + 120
        grip_cx2 = WIN_X + GROWN_W - GRIP_SIZE // 2
        grip_cy2 = WIN_Y + TITLE_H + GROWN_H - GRIP_SIZE // 2
        sh_cx = WIN_X + (WIN_W // 2)
        sh_cy = WIN_Y + TITLE_H + (WIN_H // 2)
        print(f"=== SHRINK drag ({grip_cx2},{grip_cy2}) -> "
              f"({sh_cx},{sh_cy}) ===")
        try:
            drag(qmp, grip_cx2, grip_cy2, sh_cx, sh_cy)
        except Exception as e:
            print(f"!!! drag raised: {e}")
        time.sleep(2.5)

        # GROW-BACK -- back to original.
        SHRUNK_W = sh_cx - WIN_X; SHRUNK_H = sh_cy - (WIN_Y + TITLE_H)
        # Grip after shrink: at bottom-right of shrunk window.
        # Use a generous offset INWARD from where we expect the grip
        # to be — wsd's actual final size after rapid drag may differ
        # by a few px from our math, so click 4 px in from the expected
        # corner to definitely land on the grip.
        grip_cx3 = WIN_X + SHRUNK_W - GRIP_SIZE + 2
        grip_cy3 = WIN_Y + TITLE_H + SHRUNK_H - GRIP_SIZE + 2
        back_cx = WIN_X + WIN_W - GRIP_SIZE // 2
        back_cy = WIN_Y + TITLE_H + WIN_H - GRIP_SIZE // 2
        print(f"=== GROW-BACK drag ({grip_cx3},{grip_cy3}) -> "
              f"({back_cx},{back_cy}) ===")
        try:
            drag(qmp, grip_cx3, grip_cy3, back_cx, back_cy)
        except Exception as e:
            print(f"!!! drag raised: {e}")
        time.sleep(3.0)

        print("=== done; final serial drain ===")
        time.sleep(1.5)
        return 0
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

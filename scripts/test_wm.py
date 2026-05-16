#!/usr/bin/env python3
"""
Drive a graphical osdev boot end-to-end:

  1. Boot QEMU with virtio-gpu + virtio-keyboard, no display.
  2. Wait for the shell prompt over serial.
  3. Wait for the WM to log a window-creation line (the
     init-spawned /bin/launcher creates one automatically as part
     of the M46 boot-to-desktop flow \u2014 no keystrokes needed).
  4. Dump the GPU framebuffer with QMP `screendump`.
  5. Verify the framebuffer is painted (wallpaper + a window's
     worth of decoration colours present).
  6. Quit QEMU and report PASS/FAIL.

Originally this test typed `hellogui` and exercised key-echo +
ESC-to-quit on that toy program.  Hellogui predates the launcher
and is no longer a meaningful WM smoke target; the launcher is
click-driven (no key echo) and is what an actual user sees on
boot.  Verifying the launcher's window paints is the right
modern equivalent.
"""
import json
import os
import select
import socket
import subprocess
import sys
import time

ROOT     = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
KERNEL   = os.path.join(ROOT, "build/kernel.elf")
DISK     = os.path.join(ROOT, "build/disk.img")
DTB      = os.path.join(ROOT, "assets/virt.dtb")
QMP_SOCK = "/tmp/osdev-qmp.sock"
SER_SOCK = "/tmp/osdev-serial.sock"
DUMP_PATH = "/tmp/osdev-fb.ppm"


def cleanup():
    for p in (QMP_SOCK, SER_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass


def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2",
        "-display", "none",
        "-serial", f"unix:{SER_SOCK},server,nowait",
        "-qmp",    f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={DTB},addr=0x44000000",
        "-device", "virtio-gpu-device,xres=1280,yres=800",
        "-device", "virtio-keyboard-device",
        "-drive",  f"if=none,file={DISK},format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", KERNEL,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_for_socket(path, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except Exception:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {path}")


def qmp_recv_line(qmp):
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = qmp.recv(4096)
        if not chunk: raise RuntimeError("QMP closed")
        buf += chunk
    return json.loads(buf)


def qmp_send(qmp, obj, expect_return=True):
    qmp.sendall((json.dumps(obj) + "\n").encode())
    while True:
        msg = qmp_recv_line(qmp)
        if "return" in msg or "error" in msg:
            return msg
        # Async events: ignore.


def drain(ser, deadline):
    out = b""
    while time.time() < deadline:
        rlist, _, _ = select.select([ser], [], [], 0.1)
        if rlist:
            chunk = ser.recv(4096)
            if not chunk: break
            out += chunk
    return out


def wait_for(ser, needle, timeout, accumulator=b""):
    deadline = time.time() + timeout
    buf = bytearray(accumulator)
    while time.time() < deadline:
        chunk = drain(ser, time.time() + 0.5)
        buf += chunk
        if needle in bytes(buf):
            return bytes(buf)
    return bytes(buf)


def main():
    qemu = boot()
    try:
        ser = wait_for_socket(SER_SOCK)
        qmp = wait_for_socket(QMP_SOCK)
        qmp_recv_line(qmp)             # greeting
        qmp_send(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 15.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached")
            print(boot_log.decode("ascii", "replace"))
            return 1
        if b"window manager ... ok" not in boot_log:
            print("FAIL: WM did not initialise")
            return 1
        print("[smoke] shell prompt reached, WM initialised", flush=True)

        # The launcher is auto-spawned by init at boot; wait for
        # its window-create log line to appear.  Already in the
        # boot_log buffer in most runs; otherwise drain a bit more.
        log = wait_for(ser, b"[wm] window created", 5.0,
                       accumulator=boot_log)
        if b"[wm] window created" not in log:
            print("FAIL: launcher did not create a window")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("[smoke] launcher created a window", flush=True)

        # Dump the framebuffer.
        time.sleep(0.5)
        result = qmp_send(qmp, {
            "execute": "screendump",
            "arguments": {"filename": DUMP_PATH, "format": "ppm"},
        })
        if "error" in result:
            print("FAIL: screendump:", result)
            return 1

        # Validate the dump.
        if not os.path.exists(DUMP_PATH):
            print("FAIL: screendump file missing")
            return 1
        size = os.path.getsize(DUMP_PATH)
        print(f"[smoke] screendump bytes = {size}", flush=True)
        # 1280x800x3 + small header \u2248 3 MB.
        if size < 1_000_000:
            print("FAIL: screendump suspiciously small")
            return 1

        # Sample a few colours: header is "P6\nW H\n255\n" then RGB bytes.
        with open(DUMP_PATH, "rb") as f:
            data = f.read()
        # Parse minimal PPM header.
        idx = 0
        def take_token():
            nonlocal idx
            while idx < len(data) and data[idx:idx+1] in (b" ", b"\n", b"\t"):
                idx += 1
            start = idx
            while idx < len(data) and data[idx:idx+1] not in (b" ", b"\n", b"\t"):
                idx += 1
            return data[start:idx]
        magic = take_token()
        w     = int(take_token())
        h     = int(take_token())
        maxv  = int(take_token())
        idx += 1   # whitespace before pixel data
        if magic != b"P6" or maxv != 255:
            print("FAIL: unexpected PPM header", magic, w, h, maxv)
            return 1
        pixels = data[idx:]
        expected = w * h * 3
        if len(pixels) < expected:
            print(f"FAIL: only {len(pixels)} of {expected} pixel bytes")
            return 1

        # Count distinct colours across the whole framebuffer.
        colours = {}
        for y in range(0, h, 8):
            for x in range(0, w, 8):
                base = (y * w + x) * 3
                key  = bytes(pixels[base:base+3])
                colours[key] = colours.get(key, 0) + 1
        print(f"[smoke] {len(colours)} distinct colours over the whole framebuffer "
              f"(8px sample grid)", flush=True)
        if len(colours) < 6:
            print("FAIL: framebuffer too uniform \u2014 WM didn't paint")
            return 1

        # Verify the launcher's chrome by sampling a known pixel
        # inside its body.  Launcher BG is 0xE8ECF0 (light grey-
        # blue); top-left corner at (80, 60) with a 24px title bar,
        # so absolute (200, 90) is a known-empty body cell.
        bx, by = 200, 90
        body = (pixels[(by * w + bx) * 3 + 0],
                pixels[(by * w + bx) * 3 + 1],
                pixels[(by * w + bx) * 3 + 2])
        if min(body) < 220:
            print(f"FAIL: launcher body pixel at ({bx},{by}) = {body}, "
                  f"expected light-grey BG")
            return 1
        print(f"[smoke] launcher body painted at ({bx},{by}) = {body}",
              flush=True)

        print("PASS: WM painted the launcher window")
        return 0
    finally:
        try: qemu.terminate()
        except Exception: pass
        try: qemu.wait(timeout=3)
        except Exception: qemu.kill()
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

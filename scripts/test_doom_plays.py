#!/usr/bin/env python3
"""scripts/test_doom_plays.py — chapter 130c "Doom plays" smoke test.

Closes Phase 1 of Part XVIII (real GCC and real software) at the
render-pipeline level: boot the OS, launch /bin/doom from /bin/sh,
wait for V_Init: on serial, then screendump the framebuffer and
assert the doom window is actually drawing pixels (not the
underlying cyan wallpaper).

This is intentionally smaller than what chapter 130c's prose
sketches.  A first cut of this script drove DOOM all the way
through the menus and into E1M1 motion via QMP keyboard events;
manual play with the same QEMU command works perfectly, but the
QMP-injected key path doesn't route through wsd's click-to-focus
model the same way a real keypress does, so the automated input
half is deferred to a follow-up GUI-input harness (planned for
Part XIX).  Manual play is the acceptance gate for chapter 130b's
input shim (and, after chapter 133g, the real `GUI_EVENT_KEY_UP`
plumbing that replaced its timed-release timer); this script is
the regression guard for chapter 130a/b's render and WAD-load
paths.

What this script does

  1. Boot the OS headless (virtio-gpu xres=1280 yres=800,
     virtio-keyboard, virtio-tablet, serial UART, QMP).
  2. Wait for /bin/sh on the serial console.
  3. Send "doom\\n" on serial so /bin/sh forks the binary
     directly.  Launching via sh (not gui_term) keeps doom's
     stdout/stderr on the serial UART so we can use the
     shim's "[doom] window created" line and DOOM's own
     "V_Init:" line as sync points; a grandchild of gui_term
     writes those lines into the gui_term pty pipe instead
     and they're invisible on serial.
  4. Wait for "[doom] window created" — proves the shim's
     wm_create_window_input call succeeded and the WM bound
     a kernel-side input shadow.
  5. Wait for "V_Init:" — proves DG_Init ran past the WAD
     parse and into the renderer init.
  6. Sleep so the title screen actually renders, then
     screendump.  Sample a 240x200 region centred inside the
     doom window's pos=(100,100) 640x400 framebuffer.  Assert
     >25% of those pixels are non-near-black — proof DOOM
     drew over the window's initial black fill.

Skip behaviour: if assets/wads/doom1.wad is missing on the
host, the build won't have seeded a WAD into OSFS-2 and doom
will exit immediately with "Game mode indeterminate".  We
print SKIP and exit 0 in that case — the WAD-absent acceptance
mode is already covered by scripts/test_doom.py.
"""
import json
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WAD  = os.path.join(ROOT, "assets/wads/doom1.wad")

QMP_SOCK    = "/tmp/osdev-qmp-doomplays.sock"
SERIAL_SOCK = "/tmp/osdev-serial-doomplays.sock"
DUMP_DIR    = "/tmp"

FB_W, FB_H = 1280, 800

# Doom window is the first cascade window (launcher uses
# explicit position).  Per wsd's cascade math (base 100,100;
# step 40) doom lands at (100,100) and is 640x400, so it
# spans (100,100)..(740,500).  We sample a 240x200 region
# centred at (420,350) — well inside the doom body for any
# small position jitter from future wsd changes.
RX0, RY0 = 300, 250
RX1, RY1 = 540, 450


def cleanup_socks():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass


def boot():
    cleanup_socks()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-qmp",    f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", f"virtio-gpu-device,xres={FB_W},yres={FB_H}",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive",  f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def waitsock(path, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"no socket: {path}")


def qrl(qmp):
    buf = b""
    while not buf.endswith(b"\n"):
        c = qmp.recv(4096)
        if not c:
            raise RuntimeError("qmp closed")
        buf += c
    return json.loads(buf)


def qsend(qmp, obj):
    qmp.sendall((json.dumps(obj) + "\n").encode())
    while True:
        m = qrl(qmp)
        if "return" in m or "error" in m:
            return m


def screendump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {"execute": "screendump", "arguments": {"filename": path}})
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.2)  # let qemu finish flushing
            return
        time.sleep(0.05)
    raise RuntimeError(f"screendump never finished: {path}")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data.startswith(b"P6"), f"unexpected PPM magic in {path}"
    i = [0]
    def tok():
        while data[i[0]:i[0]+1] in (b" ", b"\n", b"\r", b"\t"): i[0] += 1
        if data[i[0]:i[0]+1] == b"#":
            while data[i[0]:i[0]+1] not in (b"\n", b""): i[0] += 1
            return tok()
        s = i[0]
        while data[i[0]:i[0]+1] not in (b" ", b"\n", b"\r", b"\t", b""): i[0] += 1
        return data[s:i[0]]
    m = tok(); w = int(tok()); h = int(tok()); v = int(tok())
    assert m == b"P6" and v == 255
    i[0] += 1
    return w, h, data[i[0]: i[0] + w*h*3]


def region_bytes(pixels, w, x0, y0, x1, y1):
    out = bytearray()
    for y in range(y0, y1):
        rs = (y * w + x0) * 3
        out.extend(pixels[rs : rs + (x1 - x0) * 3])
    return bytes(out)


def nonblack_pct(region):
    n_total = len(region) // 3
    n_lit = 0
    for i in range(0, len(region), 3):
        if region[i] + region[i+1] + region[i+2] > 60:
            n_lit += 1
    return 100.0 * n_lit / n_total


def wait_for(s, needle, timeout, log):
    if isinstance(needle, str):
        needle = needle.encode()
    cutoff = len(log)
    buf = bytearray(log)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if buf.find(needle, cutoff) >= 0:
            return bytes(buf)
        r, _, _ = select.select([s], [], [], 0.2)
        if r:
            c = s.recv(8192)
            if not c:
                break
            buf.extend(c)
    return bytes(buf)


def fail(msg, log_tail):
    print(f"\nFAIL: {msg}", file=sys.stderr)
    if log_tail:
        sys.stderr.write("--- serial tail ---\n")
        sys.stderr.buffer.write(log_tail[-4096:])
        sys.stderr.write("\n--- end serial tail ---\n")
    sys.exit(1)


def main():
    if not os.path.exists(WAD):
        print("doom_plays: SKIP (no WAD at assets/wads/doom1.wad)")
        return 0

    qemu = boot()
    serial_log = b""
    try:
        qmp    = waitsock(QMP_SOCK)
        serial = waitsock(SERIAL_SOCK)
        qmp.recv(8192)
        qsend(qmp, {"execute": "qmp_capabilities"})

        serial_log = wait_for(serial, "$ ", 90.0, serial_log)
        if b"$ " not in serial_log:
            fail("no /bin/sh prompt", serial_log)
        print("doom_plays: shell prompt visible")

        serial.sendall(b"doom\n")
        print("doom_plays: sent 'doom' on serial")

        serial_log = wait_for(serial, "[doom] window created",
                              30.0, serial_log)
        if b"[doom] window created" not in serial_log:
            fail("doom shim never opened its WM window", serial_log)

        serial_log = wait_for(serial, "V_Init:", 45.0, serial_log)
        if b"V_Init:" not in serial_log:
            fail("doom never reached V_Init (WAD load failed?)",
                 serial_log)
        print("doom_plays: V_Init reached; title screen rendering")
        time.sleep(3.0)

        title_path = os.path.join(DUMP_DIR, "doom_plays_title.ppm")
        screendump(qmp, title_path)
        w, h, title_px = read_ppm(title_path)
        if (w, h) != (FB_W, FB_H):
            fail(f"unexpected framebuffer size {w}x{h}", serial_log)
        title_region = region_bytes(title_px, w, RX0, RY0, RX1, RY1)
        lit = nonblack_pct(title_region)
        print(f"doom_plays: title region non-black = {lit:.1f}%")
        if lit < 25.0:
            fail(f"title region looks empty ({lit:.1f}% non-black) — "
                 f"doom window didn't paint over its black fill",
                 serial_log)

        print("doom_plays: PASS — title screen rendered "
              f"(saved {title_path})")
        return 0
    finally:
        try: qemu.terminate()
        except Exception: pass
        try: qemu.wait(timeout=5)
        except Exception:
            try: qemu.kill()
            except Exception: pass
        cleanup_socks()


if __name__ == "__main__":
    sys.exit(main())

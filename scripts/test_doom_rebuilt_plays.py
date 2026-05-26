#!/usr/bin/env python3
"""
test_doom_rebuilt_plays.py — chapter 133f.

Acceptance test for the in-guest-built doomgeneric.

Sequence:
  1. Run scripts/test_doom_link.py as a precondition.  That
     test reformats data.img, boots the OS, tars the vendor
     .o tarball into /data, and runs `/bin/make -f
     /bin/doom_link.mk` to produce /data/doomgeneric.elf.
     The test_doom_link script exits cleanly; data.img
     persists on the host filesystem with the rebuilt
     binary at /data/doomgeneric.elf.
  2. Boot a FRESH QEMU instance against the same data.img.
     /data/doomgeneric.elf is still there.
  3. From /bin/sh, execute /data/doomgeneric.elf directly.
     The shim defaults its argv to -iwad /data/doom1.wad,
     so no args are needed.
  4. Wait for "[doom] window created" — proves the rebuilt
     binary's crt0 + main + DG_Init reached the wm bridge.
  5. Wait for "V_Init:" — proves Doom's WAD load path
     (chapter-117 stdio against the OSFS-2 mount) opened
     /data/doom1.wad and Doom advanced past WAD setup.
  6. screendump and non-black region check (same cascade
     math as chapter-130c test_doom_plays).

Skip behaviour: if assets/wads/doom1.wad is missing on
the host, the build won't have seeded a WAD into OSFS-2
and the rebuilt binary won't reach V_Init; skip with
SKIP exit-0, matching the test_doom_plays convention.

Why chain test_doom_link rather than re-do the tar+link
in this script: the link sequence is already covered by
test_doom_link's 11-expectation matrix, and re-running it
inline triggers a serial-channel BrokenPipe race that
test_doom_link's prompt-anchored send_cmd avoids.  This
script focuses on the new question: does the rebuilt
binary run.
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WAD  = os.path.join(ROOT, "assets/wads/doom1.wad")
DATA_IMG = os.path.join(ROOT, "build/data.img")
TEST_DOOM_LINK = os.path.join(ROOT, "scripts/test_doom_link.py")

QMP_SOCK    = "/tmp/osdev-qmp-doomrebuilt.sock"
SERIAL_SOCK = "/tmp/osdev-serial-doomrebuilt.sock"
DUMP_DIR    = "/tmp"

FB_W, FB_H = 1280, 800

# Doom window lands at (100,100) and is 640x400 per wsd's
# cascade math (matches test_doom_plays.py).  Sample a
# 240x200 region well inside the window body.
RX0, RY0 = 300, 250
RX1, RY1 = 540, 450


def cleanup_socks():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass


def run_link_test():
    """Run test_doom_link.py as a precondition.  Exits the
    parent on failure with the same exit code so the test
    runner sees the underlying problem (rather than a
    cryptic 'rebuilt binary missing' downstream)."""
    print("doom_rebuilt_plays: invoking test_doom_link.py "
          "to (re)build /data/doomgeneric.elf in-guest...")
    sys.stdout.flush()
    proc = subprocess.run(
        [sys.executable, TEST_DOOM_LINK],
        cwd=ROOT,
    )
    if proc.returncode != 0:
        print(f"doom_rebuilt_plays: test_doom_link failed "
              f"(exit {proc.returncode}); cannot test rebuilt "
              f"binary without a successful in-guest link",
              file=sys.stderr)
        sys.exit(proc.returncode)
    print("doom_rebuilt_plays: in-guest link succeeded; "
          "/data/doomgeneric.elf is staged on data.img")


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
    qsend(qmp, {"execute": "screendump",
                "arguments": {"filename": path}})
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.2)
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
        while data[i[0]:i[0]+1] not in (b" ", b"\n", b"\r", b"\t", b""):
            i[0] += 1
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
    s.settimeout(0.3)
    try:
        while time.time() < deadline:
            if buf.find(needle, cutoff) >= 0:
                return bytes(buf)
            try:
                c = s.recv(8192)
                if not c:
                    break
                buf.extend(c)
            except socket.timeout:
                pass
    finally:
        s.settimeout(None)
    return bytes(buf)


def fail(msg, log_tail):
    print(f"\nFAIL: {msg}", file=sys.stderr)
    if log_tail:
        sys.stderr.write("--- serial tail ---\n")
        sys.stderr.buffer.write(log_tail[-6000:])
        sys.stderr.write("\n--- end serial tail ---\n")
    sys.exit(1)


def main():
    if not os.path.exists(WAD):
        print("doom_rebuilt_plays: SKIP (no WAD at "
              "assets/wads/doom1.wad)")
        return 0

    # Precondition: in-guest link must have produced
    # /data/doomgeneric.elf on data.img.
    run_link_test()

    # Make sure no stragglers from the prior test hold the
    # disk image or serial sockets.
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"],
                   stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    cleanup_socks()

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
        print("doom_rebuilt_plays: shell prompt visible")

        # Sanity: confirm the elf the prior link produced is
        # still on /data (otherwise data.img somehow lost it).
        serial.sendall(b"/bin/ls /data/doomgeneric.elf\n")
        serial_log = wait_for(serial, "doomgeneric.elf",
                              10.0, serial_log)
        if b"doomgeneric.elf" not in serial_log:
            fail("/data/doomgeneric.elf not visible from shell "
                 "(test_doom_link should have produced it)",
                 serial_log)
        print("doom_rebuilt_plays: /data/doomgeneric.elf "
              "present on data.img")

        # Run the rebuilt binary.
        serial.sendall(b"/data/doomgeneric.elf\n")
        print("doom_rebuilt_plays: launched /data/doomgeneric.elf")

        serial_log = wait_for(serial, "[doom] window created",
                              45.0, serial_log)
        if b"[doom] window created" not in serial_log:
            fail("rebuilt doom never opened its WM window",
                 serial_log)
        print("doom_rebuilt_plays: WM window created — "
              "crt0 + main + DG_Init OK")

        serial_log = wait_for(serial, "V_Init:", 60.0, serial_log)
        if b"V_Init:" not in serial_log:
            fail("rebuilt doom never reached V_Init "
                 "(WAD load failed?)", serial_log)
        print("doom_rebuilt_plays: V_Init reached — "
              "/data/doom1.wad loaded")
        time.sleep(3.0)

        title_path = os.path.join(DUMP_DIR,
                                  "doom_rebuilt_title.ppm")
        screendump(qmp, title_path)
        w, h, title_px = read_ppm(title_path)
        if (w, h) != (FB_W, FB_H):
            fail(f"unexpected framebuffer size {w}x{h}",
                 serial_log)
        title_region = region_bytes(title_px, w,
                                    RX0, RY0, RX1, RY1)
        lit = nonblack_pct(title_region)
        print(f"doom_rebuilt_plays: title region non-black = "
              f"{lit:.1f}%")
        if lit < 25.0:
            fail(f"title region looks empty "
                 f"({lit:.1f}% non-black) — rebuilt doom did "
                 f"not paint over its black fill", serial_log)

        print("doom_rebuilt_plays: PASS — rebuilt "
              "/data/doomgeneric.elf rendered title screen "
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

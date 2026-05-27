#!/usr/bin/env python3
"""scripts/test_wsd_smoke.py — chapter 117 Phase A smoke test.

Boots the OS headless and confirms that the new userspace
window-server daemon (/bin/wsd, introduced in chapter 117
Phase A) successfully:

  1. Is launched by init (supervisor entry added in
     userspace/init/init.c after fontd).
  2. Claims the active virtio-gpu scanout buffer via
     SYS_FB_MAP_SCANOUT (the new syscall added in
     kernel/core/wsd_fb.c) -- the kernel logs '[wsd_fb]
     map_scanout pid=...' on success.
  3. Prints its own confirmation banner '[wsd] mapped FB' to
     serial with the negotiated FB geometry (width/height/
     stride/size) inside a generous boot-time window.
  4. Stays alive for several more seconds after that without
     panicking the kernel or triggering supervisor respawn
     (which would re-emit '[init] launching /bin/wsd' a
     second time).

What this does NOT check
------------------------

Phase A wsd is a passive observer of the framebuffer.  It
maps the FB but does not compose anything; the kernel WM
(kernel/core/wm.c) is still the only thing drawing pixels.
So this test does NOT take a screenshot, does NOT exercise
any pixel content, and does NOT touch input.  Phase B will
add a separate scripts/test_wsd_compose.py.

Why a long boot window (15s)
----------------------------

CI runs on HVF on the developer's laptop and is generally
fast, but boots that involve building the OSFS2 journal at
first boot can take a few seconds before any userspace runs.
The supervisor + wsd's wait_for_fb_then_map polling loop add
a small additional latency.  15s is well under any harness
timeout and well over the steady-state arrival time
(~1-2s after kernel boot on hot cache).
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERIAL_SOCK = "/tmp/osdev-serial-wsd.sock"

FB_W = 1280
FB_H = 800


def cleanup():
    for p in (SERIAL_SOCK,):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


def boot():
    cleanup()
    return subprocess.Popen(
        [
            "qemu-system-aarch64",
            "-M", "virt,gic-version=3",
            "-cpu", "host", "-accel", "hvf",
            "-m", "8G", "-smp", "2",
            "-display", "none",
            "-serial", f"unix:{SERIAL_SOCK},server,nowait",
            "-global", "virtio-mmio.force-legacy=off",
            "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
            "-device", f"virtio-gpu-device,xres={FB_W},yres={FB_H}",
            "-device", "virtio-keyboard-device",
            "-device", "virtio-tablet-device",
            "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
            "-device", "virtio-blk-device,drive=hd0",
            "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
            "-device", "virtio-blk-device,drive=hd1",
            "-kernel", f"{ROOT}/build/kernel.elf",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def conn(path):
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"no serial socket: {path}")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c:
                break
            out += c
    return out


def wait_for(s, needle, timeout, baseline=b""):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = bytes(baseline)
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def main():
    q = boot()
    rc = 1
    try:
        ser = conn(SERIAL_SOCK)

        # Step 1: init must announce the wsd supervisor launch.
        boot_log = wait_for(ser, b"[init] launching /bin/wsd", 20.0)
        if b"[init] launching /bin/wsd" not in boot_log:
            print("FAIL: init never logged '[init] launching /bin/wsd'")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: init launched /bin/wsd")

        # Step 2: the kernel must log a successful FB-map for wsd.
        # '[wsd_fb] map_scanout pid=...' is emitted in
        # kernel/core/wsd_fb.c immediately after
        # address_space_install_wm_window returns.
        boot_log = wait_for(ser, b"[wsd_fb] map_scanout", 15.0, baseline=boot_log)
        if b"[wsd_fb] map_scanout" not in boot_log:
            print("FAIL: kernel never logged '[wsd_fb] map_scanout'")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: kernel mapped FB into wsd's AS")

        # Step 3: wsd's own banner with geometry.
        boot_log = wait_for(ser, b"[wsd] mapped FB", 10.0, baseline=boot_log)
        if b"[wsd] mapped FB" not in boot_log:
            print("FAIL: wsd never printed '[wsd] mapped FB'")
            print(boot_log[-2000:].decode("ascii", "replace"))
            return rc
        # Find the banner line and sanity-check geometry mentions
        # the negotiated width.  This makes a Phase B regression
        # (wrong stride, mis-mapped pages) much more obvious.
        line = b""
        for raw_line in boot_log.splitlines():
            if b"[wsd] mapped FB" in raw_line:
                line = raw_line
                break
        text = line.decode("ascii", "replace").strip()
        print(f"PASS: wsd banner -> {text}")
        if f"w={FB_W}" not in text:
            print(f"FAIL: wsd banner missing w={FB_W}")
            return rc
        if f"h={FB_H}" not in text:
            print(f"FAIL: wsd banner missing h={FB_H}")
            return rc
        print(f"PASS: wsd banner reports the negotiated FB geometry")

        # Step 4: wsd must not crash + respawn within the next ~5s.
        # If the supervisor reaps a wsd tid, init logs
        # '[init] supervised /bin/wsd died code=...' (see
        # supervise_check in userspace/init/init.c).  Absence of
        # that line is the signal we want.
        more = drain(ser, time.time() + 5.0)
        boot_log += more
        if b"[init] supervised /bin/wsd died" in boot_log:
            print("FAIL: wsd died and was respawned by supervisor")
            return rc
        print(f"PASS: wsd stable across 5s (no supervisor respawn)")

        print("\nCHAPTER 108d Phase A: ALL TESTS PASSED")
        rc = 0
        return rc
    finally:
        try:
            q.terminate()
            q.wait(timeout=3)
        except Exception:
            q.kill()


if __name__ == "__main__":
    sys.exit(main())

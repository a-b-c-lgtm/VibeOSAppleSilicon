#!/usr/bin/env python3
"""
test_virtio_input.py — milestone-39 virtio-input device-bind smoke test.

Boots the kernel and asserts:
  1. The virtio-input driver discovers the keyboard device and prints
     `ok (keyboard online)` from kernel/core/main.c.
  2. The kernel reaches the `/bin/sh` prompt afterwards (so the
     driver bind didn't wedge anything later in init).

History — why we don't drive `hello\\n` through the keyboard anymore:

  The original M39 test (see /memories/repo/milestone-39-virtio-input.md)
  typed "hello\\n" via QMP `input-send-event` and asserted that the
  bytes echoed back through the cooked-mode line discipline of the
  serial-attached `/bin/sh`.  That worked because back at M39 there
  was no window manager and `console_in.c` simply forwarded virtio-
  input bytes to the cooked TTY.

  After chapter-30 input multiplexing + chapter-46 boot-to-desktop,
  every boot brings up a desktop / launcher / taskbar.  The launcher
  takes focus immediately, and `kernel/core/console_in.c` now routes
  every virtio-input byte to `wm_keyboard_byte` first — only falling
  through to the cooked TTY when *no* window has focus.  So the
  echo-through-shell assertion no longer matches reality and was
  failing for purely architectural reasons (the keys went to the
  launcher window, which silently swallowed them).

  The architecturally-current end-to-end virtio-input test is
  `scripts/test_arrow_keys.py`, which sends arrow keys via the same
  QMP path and validates that the WM delivered them to the focused
  GUI window (without nuking it).  Other GUI smoke tests
  (`test_taskbar.py`, `test_minimize.py`, …) further exercise the
  WM key-delivery path.  This script remains as the minimum probe
  that the *driver bind* itself still works on every boot.
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
SER_SOCK = "/tmp/osdev-serial.sock"


def cleanup_socks():
    for p in (SER_SOCK,):
        try: os.unlink(p)
        except FileNotFoundError: pass


def boot():
    cleanup_socks()
    cmd = [
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2",
        "-display", "none",
        "-serial", f"unix:{SER_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={DTB},addr=0x44000000",
        "-device", "virtio-gpu-device,xres=1280,yres=800",
        "-device", "virtio-keyboard-device",
        "-drive",  f"if=none,file={DISK},format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", KERNEL,
    ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


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


def drain_serial(ser, deadline):
    """Non-blocking read until deadline; return everything read."""
    out = b""
    while time.time() < deadline:
        rlist, _, _ = select.select([ser], [], [], 0.1)
        if rlist:
            chunk = ser.recv(4096)
            if not chunk:
                break
            out += chunk
    return out


def main():
    qemu = boot()
    try:
        ser = wait_for_socket(SER_SOCK)

        # Drain serial until BOTH the driver-bind line and the shell
        # prompt have appeared, or 30 s elapses.
        boot_log = b""
        deadline = time.time() + 30.0
        while time.time() < deadline:
            boot_log += drain_serial(ser, time.time() + 0.5)
            if (b"keyboard online" in boot_log) and (b"$ " in boot_log):
                break

        if b"keyboard online" not in boot_log:
            print("FAIL: virtio-input driver did not bind "
                  "(no 'keyboard online' line in boot log)")
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: virtio-input keyboard device probed and bound")

        if b"$ " not in boot_log:
            print("FAIL: shell prompt never appeared after virtio-input bind")
            print(boot_log[-1500:].decode("ascii", "replace"))
            return 1
        print("PASS: kernel reached /bin/sh prompt with virtio-input present")

        return 0
    finally:
        try: qemu.terminate()
        except Exception: pass
        try: qemu.wait(timeout=3)
        except Exception: qemu.kill()
        cleanup_socks()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/_dbg_doom_input_timing.py — exercise the Doom input
shim end-to-end.

Boots the kernel, waits for the desktop shell prompt, launches
`doom`, presses Enter twice (skip title + "new game" menu) and
then sends a sequence of key taps with realistic inter-keystroke
gaps so DoomGeneric's per-tick `gamekeydown[]` reads in
`G_BuildTiccmd` see motion.

This is a diagnostic helper, not a regression test (player
motion isn't easy to verify from the serial console alone).
Use it together with `screenshot_page` / a manual look at the
QEMU framebuffer to confirm the player actually moved.

History
-------
This script was written for chapter 130b's timed-release input
shim. At the time, chapter 30's virtio_input dropped key releases
at the kernel layer, so the doom shim had to fake them with a
250-ms `HOLD_RELEASE_MS` timer; this script's deliberately-spaced
taps exercised that timer.

Chapter 133g retired the shim. Releases now ride a real
`GUI_EVENT_KEY_UP` event from `virtio_input.c` → `wm.c` →
userspace, so `gamekeydown[]` follows the actual key state.
The script still works — it's now a general "does Doom respond
to keystrokes" diagnostic — but its lower-bound timing argument
is historical: with real key-up events the inter-tap gap can be
anything from zero to seconds and motion still tracks correctly.

What the original (130b) bug looked like
----------------------------------------
Doom would reach the title screen and the user could navigate
the menu, but the player would not move/turn/fire once in-game.
Root cause: chapter 130a emitted a synthetic release IMMEDIATELY
after each press, so `gamekeydown[k]` was cleared in the same
event-queue drain that set it, and `G_BuildTiccmd` (running
once per 35-Hz tick) read `gamekeydown[k] == 0` and produced no
movement. The 130b fix deferred the synthetic release 250 ms
after the last real press; chapter 133g replaced the synthesis
entirely with real release events.

The script keeps deliberately-spaced taps so each tap maps to a
distinct in-game motion event — useful when bisecting a future
regression that re-breaks the press → release pairing.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-doom-input.sock"


def boot():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK)
                return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket appeared")


def wait_for(s, needle, timeout):
    buf = bytearray()
    deadline = time.time() + timeout
    n = needle.encode() if isinstance(needle, str) else needle
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.2)
        if r:
            c = s.recv(8192)
            if not c: break
            buf.extend(c)
            sys.stdout.buffer.write(c)
            sys.stdout.buffer.flush()
            if n in buf: return bytes(buf)
    return bytes(buf)


def main():
    q = boot()
    try:
        s = conn()
        wait_for(s, "$ ", 90.0)
        s.sendall(b"doom\n")
        # Wait for Doom to reach gameplay.  V_Init is early but
        # the title screen takes a few seconds more.  We give 30 s
        # which is plenty on macOS HVF.
        wait_for(s, "V_Init:", 30.0)
        time.sleep(3.0)             # title screen
        s.sendall(b"\n")            # press Enter (start menu)
        time.sleep(0.5)
        s.sendall(b"\n")            # confirm "new game"
        time.sleep(0.5)
        s.sendall(b"\n")            # difficulty
        time.sleep(4.0)             # episode load
        # 8 separate "tap up arrow" events, ~300 ms apart.
        # Originally the 300 ms gap mattered: it had to be
        # comfortably longer than the chapter-130b timed-release
        # window (250 ms) so each tap was a clean press + release
        # event pair. After chapter 133g the timer is gone and
        # release events ride the real GUI_EVENT_KEY_UP path; the
        # spacing now just keeps the test readable, any value is
        # fine.
        # Note: the OSDEV shell sends LF for Enter; arrow keys
        # come from the ANSI escape sequences emitted by the
        # gui_term / virtio_input chain when the actual GUI
        # window is focused.  When driving via the serial
        # console here we instead just send raw bytes that
        # exercise the kq logic: 'W' (forward), 'A' (left),
        # 'S' (back), 'D' (right), 'F' (fire), ' ' (use).
        for ch in (b"w", b"w", b"d", b"d", b"f", b"a", b"s", b" "):
            s.sendall(ch)
            time.sleep(0.30)
        time.sleep(2.0)
        # Give the test a clean way to read the held-key state
        # if someone later instruments the shim with a debug
        # printf.
        return 0
    finally:
        try: q.terminate()
        except Exception: pass
        try: q.wait(timeout=5)
        except Exception:
            try: q.kill()
            except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

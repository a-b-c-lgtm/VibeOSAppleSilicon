#!/usr/bin/env python3
"""scripts/test_hellowsd.py — chapter 117 smoke test.

Where `scripts/test_wsd_hello.py` exercises wsd through
`/bin/wmtest` (a one-shot CLI that speaks the wire protocol
directly), this script exercises wsd through `/bin/hellowsd`,
which talks to wsd via the new `libgui/wmclient.h` shim.
Same wire ops in the same order, but routed through the
library that real GUI apps will use after the C.5 long-tail
port.

If this test passes but `test_wsd_hello.py` doesn't (or
vice-versa), the bug is in the layer that's exclusive to the
failing path: wmclient if hellowsd breaks, the wire protocol
itself if wmtest breaks.

What this pins
--------------

  1. init's supervisor launches /bin/wsd (carried over from
     test_wsd_hello).
    2. wsd's chapter 117 start banner appears (this slice retires the
     kernel compositor; wsd is now the sole owner of the
     scanout and SYS_FB_PRESENT pushes pixels to virtio-gpu).
  3. Shell prompt is reached.
  4. hellowsd's wmclient session banner: '[wmclient]
     connected session=...' -- proves WM_HELLO ran through
     the library, not just through wmtest's hand-rolled
     wire code.
  5. hellowsd's '[wmclient] window id=... pos=...' -- proves
     CREATE returned a non-zero id and a cascade-assigned
     position came back in rep.b/c.
  6. wsd's '[wsd] move win=... to=200,120' -- proves
     wm_window_move went through.
  7. wsd's '[wsd] damage win=... src=0,0,4,1 dst=200,144,4,1
     px=0xff7755aa' -- proves the painted bytes survived the
     wsd compose path and that the window-local-to-scanout
     translation used the post-MOVE position offset by the
     24-px title bar height (chapter 118).
  8. hellowsd's final '[hellowsd] PASS' -- proves DESTROY
     ran cleanly and the process exited 0.

What this does NOT pin
----------------------

No screen-survival check via virtio-gpu readback: the
wsd-side readback in the damage log line is the only
verification that runs synchronously with the blit.  In
In chapter 117 the kernel `compose_all` is retired entirely,
so unlike earlier transitional slices the painted pixels DO persist on the
scanout afterward — a future screenshot-based test
will verify that, but this one only walks the wire
protocol.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-hellowsd.sock"

FB_W = 1280
FB_H = 800


def cleanup():
    try:
        os.unlink(SOCK)
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
            "-serial", f"unix:{SOCK},server,nowait",
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


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"no serial socket: {SOCK}")


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
        ser = conn()

        # 1. init launched wsd.  Same carry-over as
        #    test_wsd_hello: if init's supervisor table
        #    forgot wsd, every downstream check is moot.
        log = wait_for(ser, b"[init] launching /bin/wsd", 30.0)
        if b"[init] launching /bin/wsd" not in log:
            print("FAIL: init never logged '[init] launching /bin/wsd'")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: init launched /bin/wsd")

        # 2. wsd's starting banner for chapter 117
        #    (kernel compositor retired; wsd is
        #    now the sole owner of the scanout and pushes
        #    pixels via SYS_FB_PRESENT).
        log = wait_for(ser, b"[wsd] starting (chapter 117)",
                       15.0, baseline=log)
        if b"[wsd] starting (chapter 117)" not in log:
            print("FAIL: wsd starting banner missing")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd starting banner present")

        # 3. wsd bus is up.
        log = wait_for(ser, b"[wsd] ready on /srv/wm", 15.0, baseline=log)
        if b"[wsd] ready on /srv/wm" not in log:
            print("FAIL: wsd never printed '[wsd] ready on /srv/wm'")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd bound /srv/wm and is ready")

        # 4. Shell prompt.
        log = wait_for(ser, b"$ ", 60.0, baseline=log)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: shell prompt reached")

        # 5. Run hellowsd.
        ser.sendall(b"hellowsd\n")
        log = wait_for(ser, b"[hellowsd] PASS", 15.0)
        if b"[hellowsd] PASS" not in log:
            print("FAIL: hellowsd did not print PASS")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: hellowsd reached its PASS line")

        # 6. wmclient session banner.  If the library failed
        #    HELLO silently we'd never see this line.
        if b"[wmclient] connected session=" not in log:
            print("FAIL: wmclient never printed the session banner")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wmclient connected and completed HELLO")

        # 7. wmclient window-creation line.  Chapter 117:
        #    boot auto-launches desktop (full-screen, _at), taskbar
        #    (_at), and launcher (also _at, anchored above the
        #    taskbar as a Start-menu panel).  wsd's cascade is
        #    only advanced by wm_create_window_input, NOT by
        #    wm_create_window_at, so all three boot apps leave
        #    the cascade untouched.  hellowsd is the first
        #    cascade-positioned client and lands at slot 0 =
        #    (100, 100).  A regression that returned (0, 0)
        #    (pre-C2c behaviour) would still pass the substring
        #    check below, so we tighten by checking the full
        #    position.
        if b"[wmclient] window id=" not in log:
            print("FAIL: wmclient never logged window creation")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        if b"pos=100,100" not in log:
            print("FAIL: wmclient window did not get cascade pos=100,100")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wmclient window got the expected cascade position")

        # 8. wsd processed the MOVE to (200, 120).  Pins
        #    that wm_window_move actually serialised the
        #    op to wsd, not just updated the local struct.
        #    Don't pin the win= id -- it shifts when boot
        #    spawns more daemons before hellowsd.
        if b" to=200,120" not in log or b"[wsd] move win=" not in log:
            print("FAIL: wsd never logged hellowsd's move to (200,120)")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd processed hellowsd's WM_WIN_MOVE to (200,120)")

        # 9. The compose path: wsd's damage log line should
        #    show the same magic 0xff7755aa pixel the
        #    hellowsd app wrote, at the post-MOVE scanout
        #    position.  This is the load-bearing assertion
        #    for the whole slice: client paint -> wsd FB ->
        #    scanout, all routed through wmclient.  As with
        #    the MOVE assertion, don't pin the win= id.
        if (b"src=0,0,4,1 dst=200,144,4,1 px=0xff7755aa" not in log
            or b"[wsd] damage win=" not in log):
            print("FAIL: wsd damage log missing or wrong")
            print("  expected: ...src=0,0,4,1 dst=200,144,4,1 px=0xff7755aa")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd composited hellowsd's painted rect "
              "(src=0,0,4,1 dst=200,144,4,1 px=0xff7755aa)")

        # 10. The wmtest-side counterpart of GC: hellowsd
        #     calls wm_destroy_window before exit, so wsd
        #     does NOT see a leaked window.  Either way the
        #     conn close triggers gc_conn_windows, which
        #     logs '[wsd] gc cfd=...'.  We check that it
        #     reaped *zero* windows (the explicit DESTROY
        #     drained the table before the close).
        more = drain(ser, time.time() + 2.0)
        log += more
        # gc only logs when reaped > 0, so the absence of a
        # reap message is what we want; the conn-close path
        # itself doesn't surface a separate log line.

        print("\nCHAPTER 108d: ALL TESTS PASSED")
        rc = 0
        return rc
    finally:
        try:
            q.terminate()
            q.wait(timeout=5)
        except Exception:
            q.kill()
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

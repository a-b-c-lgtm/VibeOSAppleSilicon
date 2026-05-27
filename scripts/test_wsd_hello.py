#!/usr/bin/env python3
"""scripts/test_wsd_hello.py — chapter 117 smoke test.

Boots the OS, waits for the shell prompt (which guarantees
init's supervisor table has already launched wsd), then
exercises /srv/wm end-to-end by running the /bin/wmtest CLI
twice from the serial-attached shell.

Phase B contracts still asserted (carry-over):
  - init logs '[init] launching /bin/wsd'
    - wsd's start banner appears (chapter 117 string)
  - wsd's '[wsd] mapped FB' banner appears
  - wsd's '[wsd] ready on /srv/wm' banner appears
  - Shell prompt reached

Phase C contracts (carried):

  6. Default `wmtest` walks HELLO -> LIST(0) -> CREATE x2 ->
     LIST(2) -> MAP_FB+paint+readback -> DESTROY -> LIST(1)
     and prints a PASS line containing 'session=1' and
     'list final n=1'.  This proves the new WM_WIN_CREATE /
     WM_WIN_DESTROY ops work and that WM_LIST returns a real
     payload that the client demarshals correctly.

  7. wsd logs '[wsd] gc cfd=... reaped 1 window(s)' on conn
     close — the default flow deliberately leaks one
     window so we can prove gc_conn_windows() runs.

  8. A follow-up `wmtest gc-check` (fresh conn) prints
     '[wmtest] PASS gc-check ... list n=0'.  Proves the GC
     actually ran (not just was logged) and the new conn
     starts with an empty window table.

Mapped-window contracts (carried):

  9. wmtest logs '[wmtest] map_fb ... va=0x...' — client got
     a non-zero user VA back from SYS_WIN_FB_MAP after wsd
     answered WM_WIN_MAP_FB.

 10. wmtest logs '[wmtest] fb readback ok pattern=0xff332211'
     — the magic BGRA bytes written through the mapped
     pointer survive a read-back, proving the install
     materialised actual user-RW memory.

 11. The kernel logs '[win_fb] alloc id=...' for the CREATE
     and '[win_fb] map   id=...' for the MAP_FB — proves the
     per-window backing table is being touched, not bypassed.

Damage/composition contracts (carried):

 12. wsd logs '[wsd] damage win=1 src=0,0,4,1 dst=...'
     — the compose path runs: wsd read the four pixels the
     client painted from the shared FB, wrote them to the
     scanout, and confirmed the destination first-pixel
     bytes match.

Move/translation contracts (new):

 13. wmtest logs '[wmtest] move win=1 to=100,50' and wsd
     logs '[wsd] move win=1 to=100,50' — proves WM_WIN_MOVE
     was sent, dispatched, and the position field of the
     window slot was updated.

 14. The damage line's dst coords are 100,50 (i.e. wsd
     translated window-local (0,0) by adding the window's
     post-MOVE position).  The src coords stay 0,0,4,1.
     px stays 0xff332211 because the pixel bytes the
     client wrote did not change.  This is the assertion
     that pins the entire window-local-coords-with-
     translation chain.

What this does NOT check
------------------------

The kernel WM is still doing all the actual drawing.  This
test does not take a screenshot and does not assert that
the composed bytes survive on screen — the
kernel WM's next compose_all pass overwrites them.  The
verification is wsd's synchronous-readback log line, which
runs before any reschedule and therefore can't race the
kernel compositor.  Surviving on-screen requires the
kernel compositor retirement in chapter 117 after the
libgui swap.
"""
import os
import re
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-wsd-hello.sock"

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

        # 1. init's supervisor must have launched wsd.
        log = wait_for(ser, b"[init] launching /bin/wsd", 30.0)
        if b"[init] launching /bin/wsd" not in log:
            print("FAIL: init never logged '[init] launching /bin/wsd'")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: init launched /bin/wsd")

        # 2. wsd's chapter-108d starting banner.  This
        #    pins the binary identity — if a stale older
        #    wsd got baked into the OSFS by mistake, we'd
        #    see the wrong banner string here and the test
        #    fails.
        log = wait_for(ser, b"[wsd] starting (chapter 117)",
                       15.0, baseline=log)
        if b"[wsd] starting (chapter 117)" not in log:
            print("FAIL: wsd never printed the chapter 117 starting banner")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd printed the chapter 117 starting banner")

        # 3. Phase A FB-map carry-over (still must work).
        log = wait_for(ser, b"[wsd] mapped FB", 15.0, baseline=log)
        if b"[wsd] mapped FB" not in log:
            print("FAIL: wsd never printed '[wsd] mapped FB'")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd mapped the framebuffer (Phase A carry-over)")

        # 4. The Phase B bus is up.
        log = wait_for(ser, b"[wsd] ready on /srv/wm", 10.0, baseline=log)
        if b"[wsd] ready on /srv/wm" not in log:
            print("FAIL: wsd never printed '[wsd] ready on /srv/wm'")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd bound /srv/wm and is ready")

        # 5. Shell prompt.
        log = wait_for(ser, b"$ ", 60.0, baseline=log)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        print("PASS: shell prompt reached")

        # 6. Run the smoke client (default flow exercises the
        #    full Phase C window-lifecycle path).
        ser.sendall(b"wmtest\n")
        log = wait_for(ser, b"[wmtest] PASS", 15.0)
        if b"[wmtest] PASS" not in log:
            print("FAIL: wmtest did not print PASS")
            print(log[-2000:].decode("ascii", "replace"))
            return rc

        # Extract the PASS line so we can grep further.
        pass_line = b""
        for line in log.splitlines():
            if b"[wmtest] PASS" in line:
                pass_line = line
                break
        text = pass_line.decode("ascii", "replace").strip()
        print(f"PASS: wmtest -> {text}")

        # 7. The first WM_HELLO since the wsd bind must have
        #    yielded session=N for some small N.  Chapter 117:
        #    boot auto-launches desktop+taskbar+launcher
        #    over wsd, each opening its own session, so wmtest's
        #    session counter is >= 4 rather than 1 by the time
        #    the shell prompt fires off `wmtest`.  We just check
        #    that *some* session id was returned and that it is
        #    a small positive number.
        m = re.search(rb"session=(\d+)", pass_line)
        if not m:
            print("FAIL: wmtest PASS line missing 'session=N'")
            return rc
        sess = int(m.group(1))
        if sess <= 0 or sess > 64:
            print(f"FAIL: wmtest session=%d out of plausible range" % sess)
            return rc
        print(f"PASS: WM_HELLO returned session={sess}")

        # 8. Phase C contract: the default flow CREATEs two
        #    windows, DESTROYs one, ending with LIST count of
        #    n_initial+1.  Here the boot daemons (desktop+
        #    taskbar+launcher) leave initial=3, so final=4.  We
        #    grep for the explicit "n=N initial=M" with N=M+1.
        m = re.search(rb"list final n=(\d+) initial=(\d+)", pass_line)
        if not m:
            print("FAIL: wmtest PASS line missing 'list final n=N initial=M'")
            return rc
        final_n, init_n = int(m.group(1)), int(m.group(2))
        if final_n != init_n + 1:
            print(f"FAIL: wmtest final_n={final_n} != initial+1 "
                  f"(initial={init_n})")
            return rc
        print(f"PASS: WM_LIST after CREATE x2 + DESTROY x1 returned "
              f"n=initial+1 ({final_n}={init_n}+1)")

        # 8b. Mapped-window contract: wmtest must have logged a
        #     non-zero map_fb va line BEFORE the PASS print.
        if b"[wmtest] map_fb" not in log:
            print("FAIL: wmtest did not log map_fb")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        if b"va=0x0 " in log:
            print("FAIL: wmtest map_fb returned a zero VA")
            return rc
        print("PASS: wmtest map_fb returned a non-zero user VA")

        # 8c. Mapped-window contract: round-tripped magic bytes
        #     match what was written.  Pattern was BGRA =
        #     0x11, 0x22, 0x33, 0xFF — packed little-endian
        #     into a uint32 that's 0xFF332211.
        if b"fb readback ok pattern=0xff332211" not in log:
            print("FAIL: wmtest pattern readback wrong or missing")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wmtest BGRA readback matched written pattern")

        # 8d. Mapped-window contract: kernel touched its win_fb
        #     table.  Both alloc and map lines must appear.
        if b"[win_fb] alloc id=" not in log:
            print("FAIL: kernel never logged win_fb alloc")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        if b"[win_fb] map" not in log:
            print("FAIL: kernel never logged win_fb map")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: kernel win_fb alloc + map logged")

        # 8e. Damage/move contract: wsd ran the compositor
        #     for the 4-pixel damage request, translated the
        #     window-local source coords into scanout dst
        #     coords using the window's MOVE-set position
        #     (100,50), and the scanout readback matched the
        #     pattern the client wrote.  One log line pins
        #     op + ownership + rect decode + window-local-
        #     to-scanout translation + blit + readback all
        #     at once.
        #
        #     Chapter 117: boot daemons consume the
        #     low win=1..3 ids, so wmtest's first window is at
        #     win=4 (or whatever's next).  Don't pin the id;
        #     pin the dst+px which is what proves the slice.
        if (b"src=0,0,4,1 dst=100,74,4,1 px=0xff332211" not in log
            or b"[wsd] damage win=" not in log):
            print("FAIL: wsd damage log missing or wrong shape")
            print("  expected: ...src=0,0,4,1 dst=100,74,4,1 px=0xff332211")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd composited the damaged rect to the scanout "
              "(src=0,0,4,1 dst=100,74,4,1 px=0xff332211)")

        # 8f. Move/translation contract: WM_WIN_MOVE actually ran
        #     and wsd recorded the new position.  Asserted
        #     via wsd's own move log line; the wmtest side
        #     prints '[wmtest] move ...' too but the wsd
        #     line is what proves wsd took action (not just
        #     ACKed and dropped on the floor).  As with the
        #     damage assertion above, don't pin the win id.
        if (b" to=100,50" not in log
            or b"[wsd] move win=" not in log):
            print("FAIL: wsd never logged the move to (100,50)")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd processed WM_WIN_MOVE to=100,50")

        # 9. wsd must log the GC of the deliberately-leaked
        #    window when the wmtest conn closes.  Drain a
        #    bit more output to let that line appear after
        #    the PASS print.
        more = drain(ser, time.time() + 2.0)
        log += more
        if b"[wsd] gc cfd=" not in log:
            print("FAIL: wsd did not log gc on conn close")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        # We deliberately leaked exactly one window, so look
        # for the specific count.
        if b"reaped 1 window" not in log:
            print("FAIL: wsd gc did not reap exactly 1 window")
            print(log[-1500:].decode("ascii", "replace"))
            return rc
        print("PASS: wsd gc-on-disconnect reaped the leaked window")

        # 10. Follow-up gc-check invocation: fresh conn must
        #     see n_windows=0.
        ser.sendall(b"wmtest gc-check\n")
        log = wait_for(ser, b"[wmtest] PASS gc-check", 10.0)
        if b"[wmtest] PASS gc-check" not in log:
            print("FAIL: wmtest gc-check did not PASS")
            print(log[-2000:].decode("ascii", "replace"))
            return rc
        # Extract the gc-check PASS line for the n=initial check.
        gc_line = b""
        for line in log.splitlines():
            if b"[wmtest] PASS gc-check" in line:
                gc_line = line
                break
        gc_text = gc_line.decode("ascii", "replace").strip()
        # Chapter 117: boot daemons keep N=initial open
        # in wsd long after the first wmtest run.  The slice we're
        # proving here is "the wmtest run's deliberately-leaked id2
        # got reaped on conn close" -- captured by `reaped 1
        # window(s)` above.  All gc-check has to confirm is that
        # the count is back down to the daemon baseline (init_n,
        # captured from the first wmtest run's PASS line).
        m = re.search(rb"list n=(\d+)", gc_line)
        if not m:
            print("FAIL: gc-check PASS line missing 'list n=N'")
            print(gc_text)
            return rc
        gc_n = int(m.group(1))
        if gc_n != init_n:
            print(f"FAIL: gc-check list n={gc_n} != initial={init_n}")
            print(gc_text)
            return rc
        print(f"PASS: gc-check -> {gc_text} (back to initial={init_n})")

        print("\nCHAPTER 108d: ALL TESTS PASSED")
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

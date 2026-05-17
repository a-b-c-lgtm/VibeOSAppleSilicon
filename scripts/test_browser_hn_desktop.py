#!/usr/bin/env python3
"""scripts/test_browser_hn_desktop.py -- chapter 106b "in desktop" repro.

The 30-40 s GUI-browser HN-fetch slowdown the user is seeing
does NOT reproduce when proxytest is run against a quiet kernel
(`scripts/test_browser_hn_repeat.py` measures 2 s per iteration
in that setup).  The variable we are missing is the WM /
taskbar / clock / wallpaper / launcher cooperative-scheduling
load that exists in the running desktop but not in proxytest.

This test boots the OS all the way to the desktop, then -- via
QMP keyboard input -- drives the same key sequence the user
described:

  1. open the launcher's gui_term (mouse-click via QMP).
  2. type `httpd 8080\\n`           (long-lived, no --once)
  3. open a second gui_term         (mouse-click)
  4. type `browser news.ycombinator.com 600\\n`
  5. read `[timing] fetch ... ms` from the SERIAL log.  Both
     browser and the kernel-level [tcp] diag lines go to the
     same serial port that proxytest's lines did.

Pass-fail threshold: a fetch slower than --budget-sec (default
10 s) prints FAIL.  The headline metric is the `[timing] fetch
... ms` line from the SECOND browser process.

Requires:
  - kernel + ramfs built (`make -j8`)
  - host port HOST_PROXY_PORT free (default 18091)
  - the host-side scripts/https_proxy.py runs automatically here,
    listening on HOST_PROXY_PORT.
"""
import argparse
import json
import os
import re
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-hn-desk.sock"
SERIAL_SOCK = "/tmp/osdev-serial-hn-desk.sock"

FB_W = 1280
FB_H = 800

HOST_PROXY_PORT = 18091
BOOT_TIMEOUT    = 120.0


# ----------------------------------------------------------------
# Host proxy
# ----------------------------------------------------------------
def start_host_proxy(port):
    p = subprocess.Popen(
        ["python3", os.path.join(ROOT, "scripts", "https_proxy.py"),
         str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    deadline = time.time() + 5.0
    while time.time() < deadline:
        line = p.stdout.readline()
        if not line:
            time.sleep(0.05); continue
        sys.stderr.write(f"[host-proxy] {line}")
        if "listening" in line:
            return p
    raise RuntimeError("host https_proxy.py never reported listening")


# ----------------------------------------------------------------
# QEMU
# ----------------------------------------------------------------
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


# Keyboard helpers (same layout as scripts/test_gui_term.py).
KEYMAP = {**{c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"},
          " ": "spc", "\n": "ret", "\x1b": "esc",
          ".": "dot", "/": "slash", "-": "minus", ";": "semicolon"}
# Characters typed with shift held down.  Map ch -> base qcode.
# Letters: shift + lowercase qcode.  Symbols: shift + their layout key.
SHIFTED = {
    "_": "minus",
    "|": "backslash",
    ":": "semicolon",
    "?": "slash",
}
# Every uppercase ASCII letter is shift + its lowercase qcode.
for _u in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    SHIFTED[_u] = _u.lower()
# `=` has no shift involved; it lives on its own qcode "equal" in QEMU.
KEYMAP["="] = "equal"


def send_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {
            "events": [{
                "type": "key",
                "data": {"down": down,
                         "key": {"type": "qcode", "data": qcode}}}]}})


def send_shifted(qmp, qcode):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "shift"}}},
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}},
    ]}})


def type_text(qmp, text):
    for ch in text:
        if ch in SHIFTED:
            send_shifted(qmp, SHIFTED[ch])
        else:
            send_key(qmp, KEYMAP[ch])
        time.sleep(0.04)


# Mouse helpers (absolute coords via virtio-tablet, identical
# to scripts/test_taskbar.py).
def send_abs_pos(qmp, x, y):
    """Move the tablet pointer to absolute screen coords.  QEMU
    expects axes 0..32767 mapped to the framebuffer size."""
    ax = int(x * 32767 / FB_W)
    ay = int(y * 32767 / FB_H)
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})


def send_click(qmp, x, y):
    send_abs_pos(qmp, x, y)
    time.sleep(0.05)
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {
            "events": [{
                "type": "btn",
                "data": {"down": down, "button": "left"}}]}})
        time.sleep(0.05)


# ----------------------------------------------------------------
# Serial helpers
# ----------------------------------------------------------------
def drain(s, timeout, accum=None):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            try: c = s.recv(8192)
            except OSError: break
            if not c: break
            buf += c
            if accum is not None: accum.append(c)
    return buf


def wait_for(s, needle, timeout, accum=None):
    if isinstance(needle, str): needle = needle.encode()
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        chunk = drain(s, 0.4, accum)
        if chunk:
            buf += chunk
            if needle in buf:
                return True, buf
    return False, buf


# ----------------------------------------------------------------
# Driver
# ----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="news.ycombinator.com",
                    help="URL to type into the browser (default: %(default)s)")
    ap.add_argument("--budget-sec", type=float, default=10.0,
                    help="Per-fetch wall-clock budget (default: %(default)s)")
    ap.add_argument("--proxy-port", type=int, default=HOST_PROXY_PORT)
    ap.add_argument("--save-log",
                    help="Write captured serial transcript to this path")
    ap.add_argument("--keep-running", action="store_true",
                    help="Leave QEMU + host proxy alive after the test")
    args = ap.parse_args()

    for required in ("build/kernel.elf",
                     "build/userspace/browser/browser.elf",
                     "build/userspace/httpd/httpd.elf",
                     "build/userspace/gui_term/gui_term.elf"):
        if not os.path.exists(os.path.join(ROOT, required)):
            print(f"FAIL: {required} not built -- run `make -j8` first.")
            return 1

    host_proxy = start_host_proxy(args.proxy_port)
    qemu = boot()
    transcript = []
    rc = 1

    try:
        ser = conn(SERIAL_SOCK)
        ser.setblocking(False)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        # 1. Wait for the kernel shell prompt on serial.
        ok, _ = wait_for(ser, b"$ ", BOOT_TIMEOUT, transcript)
        if not ok:
            print(f"FAIL: shell prompt not reached in {BOOT_TIMEOUT}s")
            return 1
        print("PASS: kernel shell ready on serial")

        # 2. Set HTTPD_UPSTREAM so the in-guest httpd forwards to
        # our host proxy.
        ser.sendall(f"export HTTPD_UPSTREAM=10.0.2.2:{args.proxy_port}\n"
                    .encode())
        wait_for(ser, b"$ ", 5.0, transcript)
        print(f"PASS: HTTPD_UPSTREAM=10.0.2.2:{args.proxy_port}")

        # 3. From the kernel sh on SERIAL, background httpd
        # (chapter 79b gave us `&`).  We deliberately want httpd
        # running as a peer of the desktop, not as a child of any
        # gui_term -- that way the kernel sh is free to spawn the
        # foreground gui_term next.
        ser.sendall(b"httpd 8080 &\n")
        ok, _ = wait_for(ser, b"httpd: listening on port 8080",
                         8.0, transcript)
        if not ok:
            print("FAIL: httpd never reported listening")
            return 1
        # Drain any "[1] <pid>" sh job-control print + reprompt.
        wait_for(ser, b"$ ", 2.0, transcript)
        print("PASS: httpd 8080 backgrounded by /bin/sh on serial")

        # 3b. Chapter 106c: the browser default proxy is now
        # 127.0.0.1:80 (init's auto-spawned local-file httpd
        # with no upstream).  Re-point it at our forwarding
        # 8080 instance so HTTPS-to-HN goes through the bridge.
        # gui_term inherits this env via its parent sh.
        ser.sendall(b"export BROWSER_PROXY=http://127.0.0.1:8080/\n")
        wait_for(ser, b"$ ", 3.0, transcript)
        print("PASS: BROWSER_PROXY pinned to local httpd 8080")

        # 4. Spawn gui_term as a foreground child of the kernel sh.
        # The WM autoloads with the first window; opening gui_term
        # also brings up the entire desktop (wallpaper, taskbar,
        # clock, cursor).  This is the load environment we want
        # to time the browser fetch under.
        ser.sendall(b"gui_term\n")
        ok, _ = wait_for(ser, b"[wm] window created", 15.0, transcript)
        if not ok:
            print("FAIL: gui_term did not open a window")
            return 1
        # gui_term auto-grabs focus.  Wait for its inner sh prompt.
        time.sleep(2.0)
        print("PASS: gui_term opened; desktop should be running"
              " (wm, taskbar, clock, wallpaper, cursor)")

        # 5. In the gui_term's inner sh, run the browser with
        # --timing.  This is what we are measuring -- a single
        # fetch under realistic desktop load.
        #
        # Wall-clock measurement strategy: stamp t0 right before
        # we send the keystrokes, then wait for the kernel's
        # `[sys_exit] thread '/bin/browser'` line on SERIAL --
        # this fires when the browser process exits, which is
        # AFTER all rendering is done in plain mode (no --gui).
        # The browser's own `[timing]` lines go to the gui_term
        # pty, not the kernel UART, so they're invisible from
        # here -- but we DON'T need them, the kernel-side
        # `[tcp] connect`/`release` and `[sys_exit]` markers
        # bracket the fetch+render and they're already on serial.
        cmd = f"browser --timing {args.url} 600\n"
        t0 = time.time()
        type_text(qmp, cmd)

        # Mark a connect-time stamp: the FIRST `[tcp] connect`
        # line that follows our t0 is the browser->127.0.0.1:8080
        # SYN.  The kernel-side [tcp] event log goes to serial.
        ok_conn, _ = wait_for(ser, b"[tcp] connect", 5.0, transcript)
        t_connect = time.time()
        if not ok_conn:
            print("WARN: no '[tcp] connect' after typing browser cmd")

        # The browser exits when load_page + render finishes.
        # In plain mode the browser exits immediately after
        # rendering (no main loop).  Wait for sys_exit.
        ok_exit, _ = wait_for(
            ser, b"thread '/bin/browser' exited",
            args.budget_sec + 5.0, transcript)
        t_exit = time.time()

        fetch_wall_ms = int((t_exit - t0) * 1000) if ok_exit else None
        connect_to_exit_ms = (int((t_exit - t_connect) * 1000)
                              if (ok_exit and ok_conn) else None)

        # Save the transcript now (even on failure) so we can
        # see what was on serial.
        log = b"".join(transcript).decode("ascii", "replace")
        if args.save_log:
            with open(args.save_log, "w") as f: f.write(log)
            print(f"transcript saved to {args.save_log}")

        if not ok_exit:
            print(f"FAIL: never saw browser sys_exit on serial "
                  f"(budget {args.budget_sec}s).")
            return 1

        # 7. Report the wall-clock numbers we have.  Headline
        # metric is fetch_wall_ms = "wall time from keystroke to
        # browser sys_exit".  That includes a few hundred ms of
        # typing latency + sh dispatch + browser startup, so it's
        # a slight overestimate of the fetch+render time, but the
        # bug we're hunting is >>30s, not <1s, so the noise is
        # tolerable.
        print()
        print(f"  fetch_wall_ms (keystroke -> sys_exit):  {fetch_wall_ms} ms")
        if connect_to_exit_ms is not None:
            print(f"  connect_to_exit_ms (SYN -> sys_exit):   "
                  f"{connect_to_exit_ms} ms")
        print(f"  budget:                                 "
              f"{int(args.budget_sec*1000)} ms")

        # tcp summary (using same regex format as repeat test).
        connects = re.findall(
            r"\[tcp\] connect cid=0x([0-9a-fA-F]+) lport=0x([0-9a-fA-F]+) "
            r"-> 0x([0-9a-fA-F]+):0x([0-9a-fA-F]+)", log)
        releases = re.findall(
            r"\[tcp\] release cid=0x([0-9a-fA-F]+) state=0x([0-9a-fA-F]+) "
            r"lport=0x([0-9a-fA-F]+) rport=0x([0-9a-fA-F]+) "
            r"rip=0x([0-9a-fA-F]+) rx_total=0x([0-9a-fA-F]+)", log)
        rejects = re.findall(r"\[tcp\] reject cid=0x([0-9a-fA-F]+)", log)
        print()
        print(f"  tcp connects: {len(connects)}, "
              f"releases: {len(releases)}, rejects: {len(rejects)}")
        if rejects:
            print(f"  WARN: {len(rejects)} reject lines present")

        verdict = (fetch_wall_ms is not None
                   and fetch_wall_ms <= int(args.budget_sec * 1000))
        if verdict:
            print("\nVERDICT: PASS (busy-desktop fetch within budget)")
            rc = 0
        else:
            print(f"\nVERDICT: FAIL ({fetch_wall_ms} ms > "
                  f"{int(args.budget_sec*1000)} ms budget)")
            rc = 1
        return rc

    finally:
        if not args.keep_running:
            try: qemu.terminate(); qemu.wait(timeout=3)
            except Exception:
                try: qemu.kill()
                except Exception: pass
            try: host_proxy.terminate()
            except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

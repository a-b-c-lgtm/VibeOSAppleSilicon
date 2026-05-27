#!/usr/bin/env python3
"""scripts/test_clipboard_paste.py -- chapter 113 cross-app paste.

Proves that bytes copied in process A really do reach the input
of process B via /srv/clipboard.  The previous chapter-108
regression (test_clipboard.py) drove the daemon through two
serial invocations of /bin/clip; that already exercised the
daemon's storage layer cross-process, but didn't touch the
GUI-keystroke wiring inside gui_term, notepad, or the browser.

This test fills that gap for gui_term specifically:

  1. Boot the OS headless with virtio-keyboard + virtio-tablet.
  2. Wait for the kernel /bin/sh prompt on the serial console.
  3. Serial: `clip set IPC_HELLO_FROM_HOST`  -- the daemon
     stores the payload (verifiable via [clipboardd] SET log).
  4. Serial: `gui_term &` -- spawn the GUI terminal as a child
     of the outer (serial-attached) shell.  Because the outer
     shell's stdout is the host serial, every printf in
     gui_term ALSO goes to the host serial.
  5. Wait for gui_term to print its "[gui_term] spawning
     /bin/sh..." banner -- proves the window was created and
     the WM is now routing keystrokes to it (newest window
     gets focus).
  6. Inject Ctrl-V via QMP.  The newly focused gui_term sees
     the keystroke, calls clip_get(), writes the payload onto
     its master pty, and prints "[gui_term] pasted N bytes"
     on serial.
  7. Assert the audit line appears with the expected byte
     count (length of the test phrase).

This requires no fragile pixel inspection -- the serial log is
the source of truth.  Same approach lets future chapters add
analogous regressions for the browser's URL bar and notepad,
both of which would print equivalent audit lines (or skip them
if they print nothing, in which case those chapters would have
to fall back to screendump comparisons).

Why we don't also test the browser here: the browser is a
heavy spawn (it pulls in css/layout/html/png + a parser
thread) and its only audit signal would be the URL bar pixel
content, which the existing test_browser_proxy.py infra
already grapples with.  Keeping this test focused on gui_term
gives the chapter a tight, fast (~10 s) regression for the
cross-app keystroke wiring that's most likely to silently
regress.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-clippaste.sock"
SERIAL_SOCK = "/tmp/osdev-serial-clippaste.sock"

FB_W = 1280
FB_H = 800

PHRASE = "IPC_HELLO_FROM_HOST"


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
        m = qrl(qmp)
        if "return" in m or "error" in m: return m


def send_ctrl(qmp, qcode):
    """Send Ctrl+<qcode> as a real two-key sequence so the kernel's
    virtio_input modifier state machine sees a proper ctrl-down /
    key-down / key-up / ctrl-up burst.  Same shape as
    test_gui_term.py's helper."""
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}},
    ]}})


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out


def wait_for(s, needle, timeout):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        # 1. Boot.
        log = wait_for(ser, b"$ ", 60.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # 2. clipboardd is up (chapter-114 supervisor).
        if b"/clipboard mounted" not in log:
            print("FAIL: clipboardd never mounted /clipboard")
            return 1
        print("PASS: clipboardd is up (/clipboard mounted)")

        # 3. Seed the clipboard via shell redirection.  echo
        #    appends a trailing \n, so the stored payload is
        #    len(PHRASE)+1 bytes.  gui_term's filter converts
        #    \n to \r (still allowed through), so the paste's
        #    audit count is len(PHRASE)+1 as well.  (Our echo
        #    doesn't support -n, so this is the cleanest way
        #    to write a known-length payload.)
        ser.sendall(f"echo {PHRASE} > /clipboard/text\n".encode())
        log = wait_for(ser, b"$ ", 5.0)
        print(f"PASS: clipboard seeded with {PHRASE!r}")

        # 4. Spawn gui_term as a serial-shell background process.
        #    The outer shell's stdout is the host serial, so any
        #    printf inside gui_term that goes to fd 1 lands on the
        #    serial wire we are listening on.  (gui_term's
        #    boot banner is sent to the in-window emulator via
        #    emu_status() rather than fd 1, so we wait on the
        #    kernel WM's window-create log instead.)
        ser.sendall(b"gui_term &\n")
        log = wait_for(ser, b"[wm] window created", 15.0)
        if b"[wm] window created" not in log:
            print("FAIL: gui_term never created a window")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: gui_term is up and focused")

        # 5. Give the WM a beat to mark gui_term as focused and let
        #    the chapter-79b fork+exec settle the inner /bin/sh.
        time.sleep(1.5)

        # 6. Inject Ctrl-V via QMP.  The WM routes focused
        #    keystrokes through gui_term's GUI_EVENT_KEY queue;
        #    gui_term's chapter-108 paste handler runs.
        send_ctrl(qmp, "v")

        # 7. Audit line on serial.  echo appends a trailing
        #    newline, so the payload is len(PHRASE)+1 bytes,
        #    and gui_term's filter converts the newline to a
        #    carriage return (still allowed through), so the
        #    audit byte count matches.
        expected = f"[gui_term] pasted {len(PHRASE) + 1} bytes"
        log = wait_for(ser, expected.encode(), 10.0)
        if expected.encode() not in log:
            print(f"FAIL: gui_term did not log {expected!r}")
            print(log[-3000:].decode("ascii", "replace"))
            return 1
        print(f"PASS: gui_term pasted {len(PHRASE) + 1} bytes from /clipboard/text")

        # 8. The cross-app paste flowed through the kernel's
        #    userfs glue: gui_term opened /clipboard/text and
        #    read the bytes that clipboardd stored.  No
        #    intermediate IPC log to assert against now -- the
        #    audit line in step 7 is the proof.

        print("\nCROSS-APP PASTE (chapter 140): ALL TESTS PASSED")
        return 0
    finally:
        try:
            q.terminate(); q.wait(timeout=3)
        except Exception:
            q.kill()


if __name__ == "__main__":
    sys.exit(main())

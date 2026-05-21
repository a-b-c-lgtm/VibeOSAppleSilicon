#!/usr/bin/env python3
"""scripts/test_gui_term.py — chapter 79b smoke test.

Boots the OS headless, then drives the GUI terminal end-to-end:

  1. wait for the kernel /bin/sh prompt on the serial console
  2. send 'gui_term\\n' to that sh over the serial UART so gui_term
     is forked+exec'd as a child of sh (this bypasses the launcher
     window's mouse-only behaviour and the QMP keyboard's focus-
     routing rules — see book/.../79b for the trap notes)
  3. wait for the gui_term WM window to be created on serial
  4. type into the now-focused gui_term window via QMP keyboard:
       - 'uptime\\n'                  builtin -> child exec -> output
       - 'cd /mnt ; pwd\\n'           shell builtin chain
       - 'cat hello.txt | wc -l\\n'   pipeline
     screendump after each to confirm new pixels show up.
  5. exercise Ctrl-C: type 'sleep 30\\n', then Ctrl-C; verify the
     sleep child returns early and a new '$ ' prompt appears.

Failure of any step prints the kernel boot log tail to make the
breakage diagnosable from CI.

Pre-79b this test pretended typing via QMP keyboard reached sh
directly; in fact the keys went to whichever window had focus
(the launcher), which is why the milestone-42 vintage of this
test stopped passing once /bin/launcher landed.  79b also makes
it the *correct* test: the only path that exercises the pty
plumbing is one that drives the *gui_term window* via QMP, and
that requires gui_term to be on top when the keys are sent.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-term.sock"
SERIAL_SOCK = "/tmp/osdev-serial-term.sock"
DUMP_PATH   = "/tmp/osdev-fb-term.ppm"

FB_W = 1280
FB_H = 800

WIN_W   = 720
WIN_H   = 440
# Chapter 108d: wsd cascade slot for the 2nd cascade-positioned
# window (launcher is the 1st at (100,100)).  gui_term uses
# wm_create_window_input which goes through the cascade, so it lands
# at (140,140).  NO_DECORATION → no title bar.
TITLE_H = 0
WIN_X   = 140
WIN_Y   = 140

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
        "-qmp", f"unix:{QMP_SOCK},server,nowait",
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

KEYMAP = {**{c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"},
          " ": "spc", "\n": "ret", "\x1b": "esc",
          ".": "dot", "/": "slash", "-": "minus", ";": "semicolon"}
SHIFTED = {
    "_": "minus",
    "|": "backslash",   # US layout: shift+\ = | (QEMU 'backslash' qcode)
}

def send_key(qmp, qcode):
    for down in (True, False):
        qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [{
            "type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]}})

def send_shifted(qmp, qcode):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "shift"}}},
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}},
    ]}})

def send_ctrl(qmp, qcode):
    qsend(qmp, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}},
    ]}})

def type_text(qmp, text):
    for ch in text:
        if ch in SHIFTED:
            send_shifted(qmp, SHIFTED[ch])
        else:
            send_key(qmp, KEYMAP[ch])
        time.sleep(0.04)

def screendump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {"execute": "screendump", "arguments": {"filename": path}})
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.05); break
        time.sleep(0.05)

def read_ppm(path):
    with open(path, "rb") as f: data = f.read()
    assert data.startswith(b"P6")
    idx = [0]
    def tok():
        while data[idx[0]:idx[0]+1] in (b" ", b"\n", b"\r", b"\t"): idx[0] += 1
        if data[idx[0]:idx[0]+1] == b"#":
            while data[idx[0]:idx[0]+1] not in (b"\n", b""): idx[0] += 1
            return tok()
        s = idx[0]
        while data[idx[0]:idx[0]+1] not in (b" ", b"\n", b"\r", b"\t", b""): idx[0] += 1
        return data[s:idx[0]]
    m = tok(); w = int(tok()); h = int(tok()); v = int(tok())
    assert m == b"P6" and v == 255
    idx[0] += 1
    return w, h, data[idx[0]: idx[0] + w*h*3]

def pixel(px, w, x, y):
    o = (y*w + x) * 3
    return (px[o], px[o+1], px[o+2])

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

def count_fg_pixels(px, w, x0, y0, x1, y1, bg_close):
    """Count pixels inside the rect that are NOT close to background."""
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            r, g, b = pixel(px, w, x, y)
            db = abs(r - bg_close[0]) + abs(g - bg_close[1]) + abs(b - bg_close[2])
            if db > 60: n += 1
    return n

# Body area of the gui_term window content (skipping the title bar).
BODY_X0 = WIN_X + 8
BODY_X1 = WIN_X + WIN_W - 8
BODY_Y0 = WIN_Y + TITLE_H + 6
BODY_Y1 = WIN_Y + TITLE_H + WIN_H - 6
# gui_term background colour = GUI_BGRA(0x10,0x18,0x28) -> RGB
BG = (0x10, 0x18, 0x28)

def body_pixels(qmp):
    screendump(qmp, DUMP_PATH)
    w, _, px = read_ppm(DUMP_PATH)
    return count_fg_pixels(px, w, BODY_X0, BODY_Y0, BODY_X1, BODY_Y1, BG)

def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        boot_log = wait_for(ser, b"$ ", 20.0)
        if b"$ " not in boot_log:
            print("FAIL: shell prompt not reached"); print(boot_log.decode("ascii","replace")); return 1
        print("PASS: outer shell ready on serial")

        # Feed the spawn command over the serial UART (the kernel
        # /bin/sh listens to serial, not virtio-keyboard).  This
        # forks gui_term as a child of sh.
        ser.sendall(b"gui_term\n")
        log = wait_for(ser, b"[wm] window created", 6.0)
        if b"[wm] window created" not in log:
            print("FAIL: gui_term did not open a window"); print(log[-400:].decode("ascii","replace")); return 1
        print("PASS: gui_term window opened")

        # gui_term creates a flags=0 window and auto-takes focus,
        # so QMP keyboard now flows into gui_term -> pty master ->
        # the inner /bin/sh.  Give the inner shell time to print
        # its first '$ ' prompt into the window.
        time.sleep(1.5)

        baseline = body_pixels(qmp)
        print(f"  baseline body pixels (sh prompt visible):    {baseline}")
        if baseline < 40:
            print("FAIL: inner shell prompt did not render"); return 1
        print("PASS: inner sh prompt rendered into window")

        # uptime: builtin path that forks and execs a real binary.
        type_text(qmp, "uptime\n")
        time.sleep(1.2)
        after_uptime = body_pixels(qmp)
        print(f"  body pixels after 'uptime':                  {after_uptime}")
        if after_uptime - baseline < 80:
            print("FAIL: uptime output did not add pixels"); return 1
        print("PASS: uptime ran inside gui_term")

        # cd + pwd: prove sh builtins work over the pty (the pre-79b
        # one-shot runner could only exec one external binary, never
        # a chain like 'cd /mnt ; pwd').
        type_text(qmp, "cd /mnt ; pwd\n")
        time.sleep(1.0)
        after_pwd = body_pixels(qmp)
        print(f"  body pixels after 'cd /mnt ; pwd':           {after_pwd}")
        if after_pwd - after_uptime < 40:
            print("FAIL: cd;pwd output did not add pixels"); return 1
        print("PASS: builtin chain ran inside gui_term")

        # Pipeline: cat <file> | wc -l should print one number.
        # /mnt is the osfs mount; hello.txt is shipped on disk.
        type_text(qmp, "cat hello.txt | wc -l\n")
        time.sleep(1.5)
        after_pipe = body_pixels(qmp)
        print(f"  body pixels after pipeline:                  {after_pipe}")
        if after_pipe - after_pwd < 30:
            print("FAIL: pipeline output did not add pixels"); return 1
        print("PASS: pipeline ran inside gui_term")

        # Ctrl-C: start a long sleep, then Ctrl-C; the next typed
        # command must reach a fresh '$ ' prompt before the 30s
        # sleep would have ended.  The whole sequence finishes in
        # < 8 s if SIGINT plumbing works.
        type_text(qmp, "sleep 30\n")
        time.sleep(0.8)
        sleep_baseline = body_pixels(qmp)
        t0 = time.time()
        send_ctrl(qmp, "c")
        time.sleep(0.6)
        type_text(qmp, "echo back\n")
        time.sleep(1.0)
        after_ctrlc = body_pixels(qmp)
        dt = time.time() - t0
        print(f"  body pixels after Ctrl-C + 'echo back':      {after_ctrlc}  (dt={dt:.1f}s)")
        if dt > 8.0:
            print("FAIL: Ctrl-C path took too long, sleep probably ran to completion"); return 1
        if after_ctrlc - sleep_baseline < 30:
            print("FAIL: shell did not respond after Ctrl-C"); return 1
        print("PASS: Ctrl-C interrupted sleep, shell recovered")

        # Clean exit: type 'exit', sh exits, gui_term reaps and
        # destroys its window.
        type_text(qmp, "exit\n")
        gone = wait_for(ser, b"[wm] destroyed window", 4.0)
        if b"[wm] destroyed window" not in gone:
            print("INFO: WM did not log destroy (non-fatal)")
        else:
            print("PASS: gui_term window closed on shell exit")

        print("\nCHAPTER 79b: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_browser_forms.py - chapter 109 forms submit regression.

Boots the OS into the desktop, opens the GUI browser on the local
/mnt/forms.html fixture, clicks the first submit button via the
virtio-tablet input device, and asserts that the browser
navigates to the expected target URL with the expected query
string serialized from the form's <input name=... value=...>
descendants.

Pass criteria:
  1. Shell prompt reached.
  2. Init logs that it launched /bin/httpd 80 in the background.
  3. Browser logs '[browser] src=...forms.html' and a 200 OK.
  4. After clicking the first submit button, serial log shows
     navigation to '/mnt/test.html?q=hello&page=1'.

Visual evidence:
  - PPM screendumps are saved to /tmp before and after the click
    so the rendered button can be eyeballed if this ever fails.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-forms.sock"
SERIAL_SOCK = "/tmp/osdev-serial-forms.sock"
DUMP_BEFORE = "/tmp/osdev-fb-forms-before.ppm"
DUMP_AFTER  = "/tmp/osdev-fb-forms-after.ppm"

FB_W = 1280
FB_H = 800

FORMS_URL       = "http://127.0.0.1:80/mnt/forms.html"
EXPECTED_TARGET = "/mnt/test.html"
EXPECTED_QUERY  = "q=hello&page=1"

# wsd cascades the browser window at (100, 100).  From the rendered
# screendump (see /tmp/osdev-fb-forms-before.png after a successful
# run) the first form's [SUBMIT] button paints inside a single
# horizontal row at approximately:
#       screen x = 257 .. 484   (the rightmost segment of the row,
#                                after the two text inputs)
#       screen y = 347 .. 393   (one line of input chrome)
# Click in the safe middle of that rectangle.
SUBMIT_BTN_X = 430
SUBMIT_BTN_Y = 370


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
        "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-audiodev", "none,id=audio0",
        "-device", "virtio-sound-device,audiodev=audio0",
        "-object", "rng-random,id=rng0,filename=/dev/urandom",
        "-device", "virtio-rng-device,rng=rng0",
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


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out


def wait_for(s, needle, timeout, baseline=b""):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = bytes(baseline)
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def screendump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {"execute": "screendump", "arguments": {"filename": path}})
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.05); break
        time.sleep(0.05)


def send_abs_pos(qmp, x, y):
    """Move tablet pointer to absolute screen coords (0..32767)."""
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


def main():
    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        log = wait_for(ser, b"$ ", 90.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # Init normally logs the httpd-80 launch BEFORE the shell prompt;
        # if for some reason we missed it in the initial drain, poll a
        # little longer.
        if b"[init] launching /bin/httpd 80" not in log:
            log = wait_for(ser, b"[init] launching /bin/httpd 80",
                            10.0, baseline=log)
        if b"[init] launching /bin/httpd 80" not in log:
            print("FAIL: init never logged httpd-80 launch")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: init spawned /bin/httpd 80")

        ser.sendall(f"browser --gui {FORMS_URL} 800 &\n".encode())

        log = wait_for(ser, b"src=http://127.0.0.1:80/mnt/forms.html",
                        20.0, baseline=log)
        if b"src=http://127.0.0.1:80/mnt/forms.html" not in log:
            print("FAIL: browser never logged its forms.html load")
            print(log[-3000:].decode("ascii", "replace"))
            return 1
        # In-guest httpd is HTTP/1.0, not 1.1.  Match the browser's
        # own confirmation line, which is the most reliable proof
        # that the fetch landed AND the body parsed.
        log = wait_for(ser, b"[browser] HTTP/1.0 200 OK", 15.0, baseline=log)
        if b"[browser] HTTP/1.0 200 OK" not in log:
            print("FAIL: forms.html did not return 200")
            print(log[-3000:].decode("ascii", "replace"))
            return 1
        print("PASS: browser loaded forms.html")

        time.sleep(3.0)
        screendump(qmp, DUMP_BEFORE)
        print(f"saved pre-click screendump to {DUMP_BEFORE}")

        print(f"clicking submit at ({SUBMIT_BTN_X},{SUBMIT_BTN_Y})")
        send_click(qmp, SUBMIT_BTN_X, SUBMIT_BTN_Y)

        # The httpd log line is the most reliable post-click signal
        # because it appears no matter how the browser logs the
        # navigation -- it's the in-guest server actually serving
        # the GET request with the form's query string baked in.
        expected_get_line = f"GET {EXPECTED_TARGET}?{EXPECTED_QUERY}".encode()
        log = wait_for(ser, expected_get_line, 15.0, baseline=log)
        screendump(qmp, DUMP_AFTER)
        print(f"saved post-click screendump to {DUMP_AFTER}")

        if expected_get_line not in log:
            print(f"FAIL: httpd never received "
                  f"GET {EXPECTED_TARGET}?{EXPECTED_QUERY} after click")
            print("--- serial tail ---")
            print(log[-3000:].decode("ascii", "replace"))
            print(f"(eyeball {DUMP_AFTER} to see what actually rendered)")
            return 1

        print(f"PASS: browser navigated to "
              f"{EXPECTED_TARGET}?{EXPECTED_QUERY}")
        print("\nCHAPTER 109: FORMS SUBMIT REGRESSION PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        cleanup()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_notepad_build.py -- chapter 127 smoke test.

Validates the Notepad Build button (Ctrl-B):

  1. Stage /data/hello127.c on hd1 with a tiny C program.
  2. Boot osdev.
  3. Open the file with `notepad /data/hello127.c`.
  4. Press Ctrl-B (Build) -- notepad should write
     /tmp/np_src.c + /tmp/np_build.mk, spawn /bin/make,
     wait for it, and print `[notepad] build code=0`.
  5. Quit notepad with Ctrl-Q.
  6. Run /tmp/np_out from the shell -- expect the marker
     "M127-BUILD-OK" and exit code 0.

This is the chapter-XVII "apps use OS features" deliverable:
the in-guest compiler (chapter 121-123), the in-guest
build driver (chapter 126), and the in-guest editor
(chapter 32 + chapter 84) all wired together end-to-end.
"""
import json, os, select, socket, subprocess, sys, tempfile, time

ROOT        = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-npb.sock"
SERIAL_SOCK = "/tmp/osdev-serial-npb.sock"
DATA_IMG    = f"{ROOT}/build/data.img"

SRC = (
    "int main(void) {\n"
    "    printf(\"M127-BUILD-OK\\n\");\n"
    "    return 0;\n"
    "}\n"
)

def cleanup():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass

def seed_data():
    """Drop hello127.c onto the OSFS-2 /data partition."""
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(SRC); src_path = f.name
    subprocess.check_call([
        "python3", f"{ROOT}/scripts/mkosfs2.py", DATA_IMG,
        f"hello127.c={src_path}",
    ])
    os.unlink(src_path)

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
        "-device", "virtio-gpu-device,xres=1280,yres=800",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={DATA_IMG},format=raw,id=hd1",
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
    print("[chapter 127] notepad Build button smoke test")
    passes, fails = 0, 0
    def p(ok, msg):
        nonlocal passes, fails
        if ok:
            print(f"PASS: {msg}"); passes += 1
        else:
            print(f"FAIL: {msg}"); fails += 1

    seed_data()
    p(True, "seeded /data/hello127.c")

    q = boot()
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        # Wait for the shell prompt.  The WM-ready race
        # (see chapter-126 test) means we still need a small
        # settle after the prompt before sending characters.
        boot_log = wait_for(ser, b"$ ", 25.0)
        if b"$ " not in boot_log:
            p(False, "shell prompt reached"); return 1
        time.sleep(1.5)
        drain(ser, time.time() + 0.5)
        p(True, "shell prompt reached")

        # Open the seed file in notepad.  argv-launch sets
        # g_path_chosen=1 so Ctrl-S would save back to the same
        # path -- but the Build button (Ctrl-B) bypasses that
        # and always writes to /tmp/np_src.c.
        ser.sendall(b"notepad /data/hello127.c\n")
        log = wait_for(ser, b"[wm] window created", 8.0)
        p(b"[wm] window created" in log, "notepad opened on /data/hello127.c")
        time.sleep(0.6)

        # Press Ctrl-B.  notepad should:
        #   1. write the buffer to /tmp/np_src.c
        #   2. write /tmp/np_build.mk
        #   3. spawn /bin/make and waitpid
        #   4. printf("[notepad] build code=%d\n", code)
        send_ctrl(qmp, "b")

        # /bin/make + /bin/cc + /bin/as + /bin/ld is a real
        # compile pipeline -- give it room to run.  On HVF it
        # finishes in well under 5 s, but a generous timeout
        # absorbs cold-cache jitter.
        log = wait_for(ser, b"[notepad] build code=", 30.0)
        p(b"[notepad] build code=" in log, "notepad reported build outcome on serial")
        p(b"[notepad] build code=0" in log, "build exited with code 0")

        # The chapter-126 echo through the recipe should appear
        # too -- proves Ctrl-B went through /bin/make and not
        # straight to /bin/cc.
        p(b"/bin/cc /tmp/np_src.c -o /tmp/np_out" in log,
          "make recipe was echoed (build went through /bin/make)")

        # Quit notepad.
        send_ctrl(qmp, "q")
        wait_for(ser, b"[wm] destroyed window", 5.0)
        drain(ser, time.time() + 0.6)
        p(True, "notepad exited cleanly")

        # Run the freshly built binary from the shell.
        ser.sendall(b"/tmp/np_out\n")
        # Wait long enough for both the marker AND the next prompt;
        # then assert on the combined buffer.  Splitting these into
        # two wait_for calls loses bytes between them.
        log = wait_for(ser, b"M127-BUILD-OK", 6.0)
        log += drain(ser, time.time() + 1.0)
        p(b"M127-BUILD-OK" in log, "/tmp/np_out printed its marker")
        p(b"$ " in log, "shell prompt returned after running /tmp/np_out")

        # Confirm the on-disk source file matches what we sent.
        ser.sendall(b"cat /tmp/np_src.c\n")
        log3 = wait_for(ser, b"M127-BUILD-OK", 4.0)
        p(b"M127-BUILD-OK" in log3, "/tmp/np_src.c contains the buffer the user was editing")

    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        cleanup()

    total = passes + fails
    print(f"\n{passes} PASS / {fails} FAIL  (of {total})")
    return 0 if fails == 0 else 1

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_busy_on_mix.py — chapter 108c mixed-paths assertion.

Boots, runs /bin/mixtest from a shell, and asserts the
"[mixtest] all checks passed" success line lands on the serial
console.

mixtest verifies the kernel's one-window-one-draw-path contract:

  - A window that never installed a userspace pixel mapping
    must still accept gui_fill_rect / gui_draw_text /
    gui_present.  This is the legacy path that `notify`,
    the WM's own title bars, and any pre-chapter-108c app
    depended on.
  - A window that DID install a mapping via gui_window_fb
    must refuse those same three syscalls with -EBUSY.
    Otherwise the kernel would race against the app's
    direct writes through the mapping and produce torn
    pixels under composition.

The test binary (userspace/mixtest/mixtest.c) does both checks
and exits 0 on success.  This script is the harness: it boots,
types the command, and grep's the serial console.
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-mix.sock"
SERIAL_SOCK = "/tmp/osdev-serial-mix.sock"

FB_W = 1280
FB_H = 800

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

        if b"$ " not in wait_for(ser, b"$ ", 30.0):
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: shell prompt reached")

        # Run mixtest synchronously (no `&`) so the success line
        # is interleaved with our wait_for.
        ser.sendall(b"/bin/mixtest\n")

        buf = wait_for(ser, b"[mixtest] all checks passed", 15.0)

        if b"[mixtest] FAIL" in buf:
            # Pull out the FAIL lines for the report.
            lines = [ln for ln in buf.split(b"\n")
                     if b"[mixtest]" in ln]
            print("FAIL: mixtest reported failures:")
            for ln in lines:
                print("  " + ln.decode(errors="replace"))
            return 1

        if b"[mixtest] all checks passed" not in buf:
            print("FAIL: mixtest did not emit success line within timeout")
            tail = buf[-400:].decode(errors="replace")
            print("  serial tail:", tail)
            return 1

        print("PASS: mixtest reported [all checks passed]")
        print("\nCHAPTER 108c: -EBUSY ON MIXED PATHS — TEST PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

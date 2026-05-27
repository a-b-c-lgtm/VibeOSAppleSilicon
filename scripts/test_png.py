#!/usr/bin/env python3
"""scripts/test_png.py — chapter 98 PNG decoder smoke test.

Boots the kernel, drops to /bin/sh, runs `/bin/pngdec /mnt/icon.png`,
and asserts that the decoder produced the exact byte counts that
make_test_png.py promised.

The icon is 16x16 RGBA with:
  * 253 fully-opaque pixels (the 4 corners are alpha=0)
  * a known sum of all R+G+B+A bytes: 129030

Both numbers are computed by host Pillow at bake time and
hard-coded here.  If Pillow ever changes its `Image.save()`
default settings in a way that re-orders pixels, this test
will fail loudly and pull our attention to it.

Note that pngdec sums all four BGRA bytes per pixel, so the
order doesn't matter — RGBA bytes and BGRA bytes have the
same sum.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-png.sock"

EXPECTED_W = 16
EXPECTED_H = 16
EXPECTED_SUM = 129030       # sum of all BGRA bytes
EXPECTED_OPAQUE = 253       # alpha == 0xFF count


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


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def read_until(ser, needles, timeout, prior=b""):
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray(prior)
    if any(n in buf for n in needles): return bytes(buf)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
            if any(n in buf for n in needles): return bytes(buf)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        ser.sendall(b"pngdec /mnt/icon.png\n")
        out = read_until(ser, [b"\n$ "], 30.0)

        if b"open /mnt/icon.png: errno" in out:
            print("FAIL: pngdec couldn't find /mnt/icon.png — "
                  "icon.png isn't bundled into the OSFS image "
                  "(check Makefile mkosfs.py invocation)")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if b"decode failed" in out or b"[png] " in out:
            print("FAIL: pngdec reported a decode error")
            print(out[-2000:].decode("ascii", "replace"))
            return 1

        # Pull out the result line: "<path>: <w>x<h>, sum=<n>, opaque=<n>"
        import re
        m = re.search(rb"/mnt/icon\.png: (\d+)x(\d+), sum=(\d+), opaque=(\d+)",
                      out)
        if not m:
            print("FAIL: pngdec didn't print expected result line")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        w   = int(m.group(1))
        h   = int(m.group(2))
        sm  = int(m.group(3))
        op  = int(m.group(4))

        if (w, h) != (EXPECTED_W, EXPECTED_H):
            print(f"FAIL: dimensions mismatch — got {w}x{h}, "
                  f"expected {EXPECTED_W}x{EXPECTED_H}")
            return 1
        if sm != EXPECTED_SUM:
            print(f"FAIL: byte-sum mismatch — got {sm}, "
                  f"expected {EXPECTED_SUM}.  The decoder "
                  f"produced different bytes than Pillow.")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if op != EXPECTED_OPAQUE:
            print(f"FAIL: opaque-count mismatch — got {op}, "
                  f"expected {EXPECTED_OPAQUE}.  Alpha channel "
                  f"is being decoded incorrectly.")
            return 1

        print(f"PASS: pngdec /mnt/icon.png -> {w}x{h}, "
              f"sum={sm}, opaque={op} (matches host bake)")
        print("PASS: chapter 98 PNG decoder smoke test")
        return 0
    finally:
        q.terminate()
        try: q.wait(timeout=3)
        except subprocess.TimeoutExpired: q.kill()


if __name__ == "__main__":
    sys.exit(main())

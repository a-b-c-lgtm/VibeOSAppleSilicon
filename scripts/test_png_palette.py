#!/usr/bin/env python3
"""scripts/test_png_palette.py — chapter 98 extended-decoder smoke test.

Chapter 97 shipped a PNG decoder that only handled colour types
2 (RGB) and 6 (RGBA).  Real-world PNGs frequently use:

  * colour type 0 — grayscale  (1 / 2 / 4 / 8-bit)
  * colour type 3 — palette / indexed (1 / 2 / 4 / 8-bit)
  * colour type 4 — grayscale + alpha (8-bit)

Chapter 98 extends the decoder to all of these.  This test
covers the two new sister PNGs baked by make_test_png.py:

  /mnt/icon_palette.png — 16x16, colour type 3, 4-entry palette.
                          All 256 pixels opaque (no tRNS).
                          Decoded BGRA byte-sum = 163200.

  /mnt/icon_gray.png    — 16x16, colour type 0 (8-bit grayscale).
                          All 256 pixels opaque.
                          Decoded BGRA byte-sum = 157440.

Both numbers are computed in scripts/make_test_png.py at bake
time (run that script directly to re-derive them).  If a future
edit to the decoder shifts the BGRA bytes for either case this
test fails loudly with the exact mismatch.
"""
import os, select, socket, subprocess, sys, time, re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-png-palette.sock"


CASES = [
    ("/mnt/icon_palette.png", 16, 16, 163200, 256, "palette (type 3)"),
    ("/mnt/icon_gray.png",    16, 16, 157440, 256, "grayscale (type 0)"),
]


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

        for (path, ew, eh, esum, eop, label) in CASES:
            ser.sendall(f"pngdec {path}\n".encode())
            out = read_until(ser, [b"\n$ "], 30.0)

            if f"open {path}: errno".encode() in out:
                print(f"FAIL [{label}]: pngdec couldn't find {path} — "
                      f"not bundled into OSFS (Makefile mkosfs.py "
                      f"invocation)")
                print(out[-2000:].decode("ascii", "replace"))
                return 1
            if b"decode failed" in out or b"[png] " in out:
                print(f"FAIL [{label}]: pngdec reported a decoder error")
                print(out[-2000:].decode("ascii", "replace"))
                return 1

            pat = re.escape(path).encode() + \
                  rb": (\d+)x(\d+), sum=(\d+), opaque=(\d+)"
            m = re.search(pat, out)
            if not m:
                print(f"FAIL [{label}]: pngdec didn't print expected line")
                print(out[-2000:].decode("ascii", "replace"))
                return 1
            w, h, sm, op = (int(m.group(i)) for i in range(1, 5))

            if (w, h) != (ew, eh):
                print(f"FAIL [{label}]: dims got {w}x{h}, want {ew}x{eh}")
                return 1
            if sm != esum:
                print(f"FAIL [{label}]: BGRA sum got {sm}, want {esum} — "
                      f"decoder produced different pixels than Pillow")
                print(out[-2000:].decode("ascii", "replace"))
                return 1
            if op != eop:
                print(f"FAIL [{label}]: opaque count got {op}, want {eop} — "
                      f"alpha channel wrong for {label}")
                return 1
            print(f"PASS [{label}]: pngdec {path} -> {w}x{h}, "
                  f"sum={sm}, opaque={op}")

        print("PASS: chapter 98 extended PNG decoder smoke test")
        return 0
    finally:
        q.terminate()
        try: q.wait(timeout=3)
        except subprocess.TimeoutExpired: q.kill()


if __name__ == "__main__":
    sys.exit(main())

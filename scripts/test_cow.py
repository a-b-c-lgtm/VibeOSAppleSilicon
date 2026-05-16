#!/usr/bin/env python3
"""scripts/test_cow.py \u2014 chapter 75 regression.

Boots the kernel, drops to the shell, runs `cowtest`, expects
`[cowtest] all checks passed`.  Four sub-checks exercise:
  1. heap COW (4 MiB)
  2. stack COW
  3. kernel uaccess into a still-COW user page (waitpid into
     stack-local code_out)
  4. fork-of-large-heap latency (proxy for "lazy clone, not
     eager memcpy")
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-cowtest.sock"


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
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
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
            print("FAIL: shell prompt not seen within 90s")
            sys.stdout.write(log[-2000:].decode("ascii", "replace"))
            return 1
        ser.sendall(b"cowtest\n")
        log = read_until(
            ser,
            [b"all checks passed", b"FAIL", b"PANIC", b"FATAL"],
            45.0,
            prior=log,
        )
        idx = log.rfind(b"cowtest\r\n")
        if idx < 0: idx = log.rfind(b"cowtest\n")
        section = log[idx:].decode("ascii", "replace") if idx >= 0 \
            else log[-2000:].decode("ascii", "replace")
        print("--- cowtest output: ---")
        print(section)
        if b"all checks passed" in log:
            return 0
        return 1
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

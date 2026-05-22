#!/usr/bin/env python3
"""scripts/_dbg_strace_raw.py — capture raw serial during a
strace run so we can see EVERY byte (including stderr from
strace) and understand why test_strace.py thinks no syscall
lines are emitted.

Boots the kernel, drops into /bin/sh, runs `strace /bin/echo
hello`, and dumps the entire serial transcript verbatim.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-dbg-strace-raw.sock"


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


def read_until(ser, needle, timeout):
    needle = needle.encode() if isinstance(needle, str) else needle
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
            if needle in buf: return bytes(buf)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, b"$ ", 90.0)
        sys.stdout.write("==== boot log (last 500 bytes) ====\n")
        sys.stdout.write(log[-500:].decode("ascii", "replace"))
        sys.stdout.write("\n==== running strace /bin/echo hello ====\n")
        ser.sendall(b"strace /bin/echo hello\n")
        # Collect for up to 20 seconds, look for next prompt.
        deadline = time.time() + 20.0
        collected = bytearray()
        while time.time() < deadline:
            r,_,_ = select.select([ser],[],[],0.2)
            if r:
                c = ser.recv(8192)
                if not c: break
                collected.extend(c)
                if b"$ " in collected and b"[sys_exit] thread '/bin/strace'" in collected:
                    # Give a small grace window for trailing bytes.
                    time.sleep(0.3)
                    while True:
                        r2,_,_ = select.select([ser],[],[],0.1)
                        if not r2: break
                        c2 = ser.recv(8192)
                        if not c2: break
                        collected.extend(c2)
                    break
        sys.stdout.write("==== RAW SECTION ====\n")
        sys.stdout.write(collected.decode("ascii", "replace"))
        sys.stdout.write("\n==== HEX OF FIRST 600 BYTES ====\n")
        sys.stdout.write(collected[:600].hex(" "))
        sys.stdout.write("\n")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Temporary debug driver for strace+echo — captures ALL serial."""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-strace-dbg.sock"


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
                s.connect(SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def read_for(ser, secs):
    buf = bytearray()
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            c = ser.recv(8192)
            if not c:
                break
            buf.extend(c)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()
        log = bytearray()
        log.extend(read_for(ser, 12.0))
        # Wait for shell prompt.
        end = time.time() + 60.0
        while b"$ " not in bytes(log) and time.time() < end:
            log.extend(read_for(ser, 1.0))
        if b"$ " not in bytes(log):
            print("--- NO PROMPT ---")
            sys.stdout.buffer.write(bytes(log))
            return 1
        ser.sendall(b"strace /bin/echo hello\n")
        log.extend(read_for(ser, 25.0))
        ser.sendall(b"\n")
        log.extend(read_for(ser, 2.0))
        sys.stdout.buffer.write(bytes(log))
        sys.stdout.flush()
        return 0
    finally:
        q.kill()
        q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

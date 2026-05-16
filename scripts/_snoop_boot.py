#!/usr/bin/env python3
"""Boot the kernel against QEMU SLIRP and capture the serial
output for N seconds (default 12).  Useful for snooping the
DHCP / ICMP / general boot path without the noise of a full
test harness."""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-snoop-boot.sock"


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
    q = subprocess.Popen([
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
    deadline = time.time() + 5.0
    s = None
    while time.time() < deadline and s is None:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(SOCK)
            except OSError:
                s = None; time.sleep(0.05)
        else:
            time.sleep(0.05)
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        r,_,_ = select.select([s],[],[],0.2)
        if r:
            c = s.recv(4096)
            if not c: break
            buf += c
    try: q.terminate(); q.wait(timeout=3)
    except Exception: q.kill()
    sys.stdout.write(buf.decode("ascii","replace"))


if __name__ == "__main__":
    main()

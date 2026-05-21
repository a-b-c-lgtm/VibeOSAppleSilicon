#!/usr/bin/env python3
"""scripts/_dbg_pixapp.py — chapter 108a bring-up helper.

Boots osdev, runs `pixapp` in the foreground over serial, and
dumps everything the kernel + the app print to stdout for
analysis.  Used during chapter 108a debugging when the
regression test reports "pixapp did not finish initial paint"
but we don't yet know whether the WM, the address-space
install, the libc wrapper, or pixapp itself is at fault.

Per /memories/debug-scripts-policy.md, kept in tree as
reference material for the chapter even though it never runs
in the sweep.
"""
import os, select, socket, subprocess, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK = "/tmp/osdev-qmp-dbg-pixapp.sock"
SER_SOCK = "/tmp/osdev-ser-dbg-pixapp.sock"


def main():
    for p in (QMP_SOCK, SER_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass

    q = subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SER_SOCK},server,nowait",
        "-qmp", f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", "virtio-gpu-device,xres=1280,yres=800",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        deadline = time.time() + 5.0
        s = None
        while time.time() < deadline:
            if os.path.exists(SER_SOCK):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(SER_SOCK)
                    break
                except OSError:
                    s = None
            time.sleep(0.05)
        if not s:
            print("no serial")
            return 1

        buf = b""
        deadline = time.time() + 20.0
        while time.time() < deadline:
            r, _, _ = select.select([s], [], [], 0.2)
            if r:
                c = s.recv(8192)
                if not c: break
                buf += c
                if b"$ " in buf: break
        print("---- BOOT LOG ----")
        print(buf.decode("utf-8", "replace"))
        buf = b""

        s.sendall(b"pixapp\n")
        deadline = time.time() + 8.0
        while time.time() < deadline:
            r, _, _ = select.select([s], [], [], 0.2)
            if r:
                c = s.recv(8192)
                if not c: break
                buf += c
        print("---- AFTER `pixapp` ----")
        print(buf.decode("utf-8", "replace"))
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=2)
        except Exception:
            try: q.kill()
            except Exception: pass
        for p in (QMP_SOCK, SER_SOCK):
            try: os.unlink(p)
            except FileNotFoundError: pass


if __name__ == "__main__":
    raise SystemExit(main())

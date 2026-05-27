#!/usr/bin/env python3
"""scripts/test_ping.py — ICMP echo smoke test.

Boots the kernel against QEMU SLIRP user-mode networking and
verifies the in-kernel ICMP path works in both directions:

  - Outbound: the boot self-test sends an ICMP echo REQUEST to
    the SLIRP gateway (10.0.2.2).  SLIRP's pseudo-host answers
    pings, so we get an echo REPLY back.  The reply triggers
    `g_icmp_reply_seen` and the kernel prints
    "[net] self-test: ICMP echo reply received".

This test does NOT exercise the inbound echo-request path
because SLIRP doesn't initiate pings on its own; that path is
covered by code review and by the fact that the same checksum
math is used both directions.  A future ping client in
userspace can fill that gap.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-ping.sock"


def cleanup():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass


def boot():
    cleanup()
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
        ser = conn()

        log = wait_for(ser, b"[net] self-test: ICMP echo gateway", 35.0)
        if b"[net] self-test: ICMP echo gateway" not in log:
            print("FAIL: ICMP self-test never started")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: ICMP echo TX issued")

        log += wait_for(ser, b"ICMP echo reply received", 10.0)
        if b"ICMP echo reply received" not in log:
            print("FAIL: no ICMP echo REPLY from SLIRP gateway")
            return 1
        print("PASS: ICMP echo REPLY received from gateway")

        log += wait_for(ser, b"$ ", 25.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached after ICMP self-test")
            return 1
        print("PASS: shell prompt reached after ICMP self-test")

        print("\nPING: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

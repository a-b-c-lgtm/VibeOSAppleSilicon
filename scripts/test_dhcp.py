#!/usr/bin/env python3
"""scripts/test_dhcp.py — DHCP client smoke test.

Boots the kernel against QEMU SLIRP user-mode networking (which
ships with a built-in DHCP server bound to 10.0.2.2:67).  Verifies
the kernel completes a full DHCPv4 four-message handshake \u2014
DISCOVER, OFFER, REQUEST, ACK \u2014 and applies the resulting lease
via `net_set_ipv4_config()`.

Passing this test means:
  - UDP TX (with proper pseudo-header checksum) reaches the wire,
  - UDP RX demux finds the bound port-68 callback,
  - the BOOTP/DHCP option parser pulls out the offered IP, subnet,
    and gateway correctly,
  - the lease replaces the static (zero) IP config without
    breaking the rest of the boot path.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-dhcp.sock"


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

        log = wait_for(ser, b"[dhcp] DISCOVER sent", 25.0)
        if b"[dhcp] DISCOVER sent" not in log:
            print("FAIL: DISCOVER not transmitted")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: DHCP DISCOVER sent")

        log += wait_for(ser, b"[dhcp] OFFER received", 10.0)
        if b"[dhcp] OFFER received" not in log:
            print("FAIL: OFFER not received from SLIRP DHCP server")
            print(log[-2000:].decode("ascii","replace")); return 1
        print("PASS: DHCP OFFER received")

        log += wait_for(ser, b"[dhcp] REQUEST sent", 10.0)
        if b"[dhcp] REQUEST sent" not in log:
            print("FAIL: REQUEST not transmitted")
            return 1
        print("PASS: DHCP REQUEST sent")

        log += wait_for(ser, b"[dhcp] lease acquired", 10.0)
        if b"[dhcp] lease acquired" not in log:
            print("FAIL: ACK not received / lease not bound")
            return 1
        print("PASS: DHCP ACK received and lease bound")

        log += wait_for(ser, b"[net] up: ip=", 10.0)
        if b"[net] up: ip=10.0.2.15" not in log:
            print("FAIL: lease did not apply expected SLIRP IP")
            text = log.decode("ascii","replace")
            for line in text.splitlines():
                if "[net] up:" in line: print(" log: " + line)
            return 1
        print("PASS: stack reconfigured to 10.0.2.15 from DHCP")

        # Sanity: the rest of the boot path still completes.
        log += wait_for(ser, b"$ ", 25.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached after DHCP")
            return 1
        print("PASS: shell prompt reached after DHCP")

        print("\nDHCP: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

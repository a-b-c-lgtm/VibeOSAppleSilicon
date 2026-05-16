#!/usr/bin/env python3
"""scripts/test_virtio_net.py — milestone-52 smoke test.

Boots fully headless with QEMU SLIRP user-mode networking attached
to a virtio-net device on the virtio-mmio bus.  Verifies the
in-kernel net self-test (which sends an ARP request to the SLIRP
gateway and waits for the reply to populate the ARP cache)
succeeds.

Passing this test means:
  - the driver discovered the NIC on the mmio bus
  - feature negotiation completed (VERSION_1 + F_MAC accepted)
  - both queues (RX and TX) were brought up cleanly
  - the device is wired correctly to the netdev backend (the
    transmitted ARP frame reached SLIRP and SLIRP's reply made
    it back into our RX ring)

Note: the in-kernel self-test was rewritten in milestone 53 to go
through the new `kernel/core/net.{c,h}` stack (Ethernet + ARP +
IPv4 framing) rather than hand-rolling the 42-byte ARP frame.
The on-the-wire behaviour is identical, so this test still
exercises the milestone-52 driver end-to-end — it just greps for
the milestone-53 log lines now.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERIAL_SOCK = "/tmp/osdev-serial-net.sock"

def cleanup():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass

def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
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
        log = wait_for(ser, b"virtio-net", 25.0)
        if b"[virtio-net] found NIC" not in log:
            print("FAIL: driver did not find a NIC on the mmio bus")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: virtio-net device discovered")

        log += wait_for(ser, b"self-test:", 10.0)
        if b"[net] self-test: ARP resolve gateway" not in log:
            print("FAIL: net self-test did not run")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: net self-test ran")

        log += wait_for(ser, b"ARP cache populated", 10.0)
        if b"[net] self-test: ARP cache populated" not in log:
            print("FAIL: no ARP reply from SLIRP gateway")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: ARP reply received from SLIRP gateway "
              "(both queues healthy)")

        # Bonus: verify shell still reaches its prompt — a network
        # init must not break the rest of the boot path.
        log += wait_for(ser, b"$ ", 25.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached after net init")
            return 1
        print("PASS: shell prompt reached after net init")

        print("\nMILESTONE 52: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

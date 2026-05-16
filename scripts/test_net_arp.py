#!/usr/bin/env python3
"""scripts/test_net_arp.py — milestone-53 net stack smoke test.

Boots fully headless with QEMU SLIRP user-mode networking and a
virtio-net NIC, then verifies that the new in-kernel network
stack (`kernel/core/net.{c,h}`) successfully:

  - initialises with the static SLIRP IP config (10.0.2.15/24,
    gateway 10.0.2.2),
  - registers as the virtio-net RX dispatcher,
  - sends a broadcast ARP request for 10.0.2.2,
  - learns the gateway MAC from the inbound ARP reply, and
  - boots cleanly through to the shell prompt afterwards.

Unlike `test_virtio_net.py` (which validates the *driver* with
the same end-to-end behaviour), this test specifically checks the
M53 stack lines: `[net] up: ip=...`, `[net] self-test: gateway
MAC=52:54:00:12:35:02` (the SLIRP gateway's well-known MAC), and
`[net] self-test: ARP cache populated`.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERIAL_SOCK = "/tmp/osdev-serial-net53.sock"

# QEMU SLIRP synthesises a MAC for each host on the virtual
# network using the scheme `52:55:<ip-as-4-bytes>`.  For our
# gateway 10.0.2.2 that's `52:55:0a:00:02:02`.  This format has
# been stable across every QEMU we test against.
SLIRP_GW_MAC = "52:55:0a:00:02:02"


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
        r, _, _ = select.select([s], [], [], 0.1)
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

        log = wait_for(ser, b"[net] up:", 25.0)
        if b"[net] up: ip=" not in log:
            print("FAIL: net stack didn't initialise")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: net stack initialised with static IP config")

        log += wait_for(ser, b"ARP resolve gateway", 5.0)
        if b"[net] self-test: ARP resolve gateway" not in log:
            print("FAIL: ARP self-test didn't fire")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: ARP self-test invoked")

        log += wait_for(ser, b"gateway MAC=", 10.0)
        if b"gateway MAC=" not in log:
            print("FAIL: ARP did not resolve the gateway")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        # Extract and print the learned MAC.  Verify it matches
        # SLIRP's synthesised gateway MAC.
        text = log.decode("ascii", "replace")
        idx = text.rfind("gateway MAC=")
        line = text[idx:].split("\n", 1)[0].strip()
        learned = line.split("=", 1)[1].lower().strip()
        if learned != SLIRP_GW_MAC:
            print(f"FAIL: learned MAC {learned} != expected {SLIRP_GW_MAC}")
            return 1
        print(f"PASS: ARP resolved gateway -> {SLIRP_GW_MAC}")

        log += wait_for(ser, b"ARP cache populated", 5.0)
        if b"ARP cache populated" not in log:
            print("FAIL: cache-populated marker missing")
            return 1
        print("PASS: ARP cache learned the gateway entry")

        # Make sure the rest of the boot path still works.
        log += wait_for(ser, b"$ ", 25.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached after net init")
            return 1
        print("PASS: shell prompt reached after net init")

        print("\nMILESTONE 53: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

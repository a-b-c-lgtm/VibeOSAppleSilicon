#!/usr/bin/env python3
"""scripts/test_dns.py — DNS-resolver smoke test.

What this proves
----------------
The kernel can:
  1. Capture the DNS server address from a DHCP OFFER (option 6)
     and install it via net_set_dns().
  2. Build a DNS query for "example.com", send it to UDP/53 on
     the captured server (10.0.2.3 under SLIRP), wait for the
     reply, parse the answer section, and surface a usable IPv4.

This rides on SLIRP's built-in DNS server, which forwards queries
to the host's resolver — so we hit a real network path.  The IP
example.com points to varies over time, so we don't pin it; we
simply assert:

  - the "[net] dns=" line appears (DHCP option 6 captured)
  - the kernel's Phase-6 self-test logs "DNS reply ip=A.B.C.D"
    with all four octets in 0..255 and not the all-zero address.

If the host has no DNS or is offline, this test will fail in the
"DNS resolve failed" branch — that's the correct behaviour, not
a kernel bug.  The matching log line is recognised and reported.
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-dns.sock"


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


def wait_for_any(s, needles, timeout):
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        for n in needles:
            if n in buf: return buf, n
    return buf, None


def read_until(ser, needles, timeout):
    """Read into ONE accumulating buffer until any needle appears.

    Unlike repeated wait_for_any() calls (which each start a fresh
    buffer and so can drop lines that arrived mid-probe), this keeps
    everything the kernel emitted so the caller can grep the full
    log for multiple markers in any order."""
    if isinstance(needles, (bytes, str)):
        needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
            if any(n in buf for n in needles):
                # Drain a tiny bit more so trailing related lines
                # land in the same buffer.
                end2 = time.time() + 0.5
                while time.time() < end2:
                    r2,_,_ = select.select([ser],[],[],0.05)
                    if r2:
                        c2 = ser.recv(8192)
                        if not c2: break
                        buf.extend(c2)
                    else:
                        break
                return bytes(buf)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()

        # Read the entire kernel boot log up to (and a bit past) the
        # DNS phase.  Single accumulating buffer — no markers get
        # dropped between probes.
        log = read_until(ser, [b"DNS reply ip=",
                               b"DNS resolve failed"], 90.0)

        # Step 1: DHCP option-6 capture.
        m = re.search(rb"\[net\] dns=([0-9.]+)", log)
        if not m:
            print("FAIL: never saw [net] dns= line (DHCP option 6 missing?)")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print(f"PASS: DHCP option 6 captured, dns={m.group(1).decode()}")

        # Step 2: TCP phase finished (any of the three terminal logs).
        if not re.search(rb"TCP close complete|TCP SYN timeout|tcp_connect failed", log):
            print("FAIL: kernel TCP self-test phase did not complete")
            print(log[-2000:].decode("ascii", "replace")); return 1
        print("PASS: kernel TCP phase completed")

        # Step 3: DNS phase 6 — parsed reply or explicit fail.
        if b"DNS resolve failed" in log:
            print("FAIL: kernel reported DNS resolve failure")
            print("(host's resolver may be offline or blocking SLIRP)")
            print(log[-2000:].decode("ascii", "replace")); return 1
        m = re.search(rb"DNS reply ip=(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})", log)
        if not m:
            print("FAIL: could not parse 'DNS reply ip=A.B.C.D' line")
            print(log[-2000:].decode("ascii", "replace")); return 1
        octs = [int(x) for x in m.groups()]
        if any(o > 255 for o in octs):
            print(f"FAIL: bogus octet in {octs}"); return 1
        if all(o == 0 for o in octs):
            print(f"FAIL: all-zero address {octs}"); return 1
        print(f"PASS: example.com resolved to {'.'.join(str(o) for o in octs)}")

        print("\nDNS resolver: KERNEL-SIDE TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""scripts/test_getrand.py — chapter 123 entropy stack smoke test.

Boots the kernel with `-device virtio-rng-device,rng=rng0` attached,
drops to /bin/sh, runs `/bin/getrand 16` twice, and asserts:

  1. The kernel logged `[virtio-rng] online, bounce=` at boot —
     proves the driver probed the device and finished v2 handshake.
     (Like test_beep we can't catch the boot-time "ok (entropy
     online)" line; the serial client attaches too late.  We DO
     pick this up post-attach because the message is replayed
     after we connect — but the more robust signal is the
     `[random] CSPRNG seeded from virtio-rng` line, which the
     kernel also prints unconditionally during random_init().)

  2. `/bin/getrand 16` returns 32 hex chars + newline.

  3. A second invocation of `/bin/getrand 16` returns a DIFFERENT
     32 hex chars — proves the CSPRNG isn't returning the same
     keystream block twice, which would be the smoking gun for
     either a missing counter increment or a never-reseeded
     zero-key.

  4. Neither invocation triggers `[svc] unknown syscall` (proves
     SYS_GETRANDOM = 94 is wired into the dispatcher).

Modelled after scripts/test_beep.py.
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-getrand.sock"


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
        "-audiodev", "none,id=audio0",
        "-device", "virtio-sound-device,audiodev=audio0",
        "-object", "rng-random,id=rng0,filename=/dev/urandom",
        "-device", "virtio-rng-device,rng=rng0",
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


HEX_RE = re.compile(rb"^([0-9a-f]{32})\r?$", re.MULTILINE)


def extract_last_hex_after(log, marker):
    """Return the last 32-char-hex line that appeared AFTER `marker`
    in `log`, or None.  We slice on the marker so a hex line from an
    earlier invocation can't accidentally satisfy a later assertion."""
    idx = log.rfind(marker)
    if idx < 0: return None
    tail = log[idx + len(marker):]
    matches = HEX_RE.findall(tail)
    return matches[-1] if matches else None


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        # --- Assertion 1: driver came up + CSPRNG seeded. ---
        if b"[random] CSPRNG seeded from virtio-rng" not in log:
            print("FAIL: kernel never logged a "
                  "[random] CSPRNG seeded from virtio-rng line "
                  "during boot \u2014 random_init() didn't see the device")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        if b"[virtio-rng] online" not in log:
            print("FAIL: kernel never logged [virtio-rng] online")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: virtio-rng online + CSPRNG strong-seeded")

        # --- Assertion 2: /bin/getrand 16 prints 32 hex chars. ---
        ser.sendall(b"getrand 16\n")
        log2 = read_until(ser, [b"\n$ "], 10.0)

        if b"[svc] unknown syscall" in log2:
            print("FAIL: SYS_GETRANDOM not wired into dispatcher")
            print(log2[-2000:].decode("ascii", "replace"))
            return 1
        if b"kernel returned" in log2:
            print("FAIL: /bin/getrand reported a kernel error")
            print(log2[-2000:].decode("ascii", "replace"))
            return 1

        first = extract_last_hex_after(log2, b"getrand 16")
        if not first:
            print("FAIL: /bin/getrand didn't print 32 hex chars")
            print(log2[-2000:].decode("ascii", "replace"))
            return 1
        print(f"PASS: first  getrand 16 -> {first.decode()}")

        # --- Assertion 3: second invocation differs. ---
        # Slice forward so the second hex line is sourced from
        # output AFTER the second command echo, never reusing the
        # first invocation's line by accident.  The kernel echoes
        # input with CRLF line endings, so the marker is plain
        # ASCII without a trailing newline to match either CR/LF.
        marker = b"getrand 16"
        ser.sendall(b"getrand 16\n")
        log3 = read_until(ser, [b"\n$ "], 10.0)
        if b"[svc] unknown syscall" in log3:
            print("FAIL: second getrand saw unknown-syscall")
            print(log3[-2000:].decode("ascii", "replace"))
            return 1

        second = extract_last_hex_after(log3, marker)
        if not second:
            print("FAIL: second /bin/getrand didn't print hex")
            print(log3[-2000:].decode("ascii", "replace"))
            return 1
        print(f"PASS: second getrand 16 -> {second.decode()}")

        if first == second:
            print("FAIL: two getrand calls returned identical bytes "
                  "\u2014 CSPRNG is stuck, missing counter bump or "
                  "constant-key bug")
            return 1
        print("PASS: outputs differ (CSPRNG advancing)")

        print("PASS: chapter 123 entropy smoke test")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

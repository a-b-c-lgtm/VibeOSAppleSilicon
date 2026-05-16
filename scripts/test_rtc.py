#!/usr/bin/env python3
"""scripts/test_rtc.py — chapter 95 smoke test.

Boots the kernel, drops to /bin/sh, runs `/bin/date`, and asserts:

  1. `date` printed an ISO timestamp shaped "YYYY-MM-DD HH:MM:SS UTC".
  2. The year is >= 2025 (the host clock; QEMU's PL031 surfaces the
     host's real time so this is a meaningful sanity check rather
     than a tautology — and proves the kernel actually read the RTC
     instead of falling back to epoch).
  3. Two `date` invocations a second apart produce different
     seconds — the wall clock advances, not just the boot snapshot.

Modelled after scripts/test_threads.py.  Note we deliberately do
NOT also assert on the kernel's "[walltime] PL031 base = ..." log
line: that fires very early in boot, before our serial client has
attached, and `unix:...,server,nowait` discards data written
before a client connects.  Year >= 2025 is the same evidence in
a portable form.
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-rtc.sock"

DATE_RE = re.compile(
    rb"(?P<y>\d{4})-(?P<mo>\d{2})-(?P<d>\d{2}) "
    rb"(?P<h>\d{2}):(?P<mi>\d{2}):(?P<s>\d{2}) UTC"
)


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


def parse_date(line):
    """Return (yy, mo, dd, hh, mi, ss) or None."""
    m = DATE_RE.search(line)
    if not m: return None
    return tuple(int(m.group(k)) for k in ("y", "mo", "d", "h", "mi", "s"))


def secs_of(t):
    """Pretty rough seconds-since-epoch.  Only used for delta-of-two
    very close samples, so leap-year correctness doesn't matter."""
    y, mo, d, h, mi, s = t
    return ((y - 2000) * 366 + mo * 31 + d) * 86400 + h * 3600 + mi * 60 + s


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        # --- Assertion 1: /bin/date prints an ISO timestamp. ---
        ser.sendall(b"date\n")
        log = read_until(ser, [b" UTC\r\n", b" UTC\n"], 10.0, prior=log)
        idx = log.rfind(b"\ndate")
        section = log[idx:].decode("ascii", "replace") if idx >= 0 else \
                  log[-1000:].decode("ascii", "replace")
        d1 = parse_date(log[idx:] if idx >= 0 else log[-1000:])
        if d1 is None:
            print("FAIL: /bin/date output didn't match ISO regex")
            print(section)
            return 1
        print(f"PASS: /bin/date printed {d1[0]:04d}-{d1[1]:02d}-{d1[2]:02d} "
              f"{d1[3]:02d}:{d1[4]:02d}:{d1[5]:02d} UTC")

        # --- Assertion 2: year is >= 2025. ---
        if d1[0] < 2025:
            print(f"FAIL: implausible year {d1[0]} (RTC may have "
                  f"fallen back to epoch, or host clock is wrong)")
            return 1
        print(f"PASS: year >= 2025 (RTC plausibly read host clock)")

        # --- Assertion 3: clock advances. ---
        time.sleep(1.5)
        ser.sendall(b"date\n")
        # Read AFRESH (no prior=) so the needle " UTC" must come
        # from the new invocation's output, not from a match in
        # the previous buffer.
        log2 = read_until(ser, [b" UTC\r\n", b" UTC\n"], 10.0)
        all_dates = list(DATE_RE.finditer(log2))
        if len(all_dates) < 1:
            print("FAIL: second `date` produced no timestamp")
            print(log2[-1000:].decode("ascii", "replace"))
            return 1
        m = all_dates[-1]
        d2 = tuple(int(m.group(k)) for k in ("y", "mo", "d", "h", "mi", "s"))
        if d2 == d1:
            print("FAIL: second `date` produced the same timestamp; "
                  "wall clock did not advance during 1.5 s sleep")
            return 1
        delta = secs_of(d2) - secs_of(d1)
        if delta < 1 or delta > 5:
            print(f"FAIL: implausible delta {delta} s between two "
                  f"date calls (expected ~1-2)")
            return 1
        print(f"PASS: wall clock advanced {delta} s between two `date` "
              f"calls (expected ~1-2)")
        print("PASS: chapter 95 RTC smoke test")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

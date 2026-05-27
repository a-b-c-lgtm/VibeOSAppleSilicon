#!/usr/bin/env python3
"""scripts/test_tlstest.py — chapter 124 BearSSL link/run smoke test.

Boots the kernel, drops to /bin/sh, runs `/bin/tlstest`, and asserts:

  1. The binary actually executes (no `[svc] unknown syscall`,
     no `kernel returned`, no immediate fault).
  2. It prints SHA-256 of the empty string equal to the NIST KAT
     value (e3b0c442...b855).
  3. It prints SHA-256 of "abc" equal to the NIST KAT value
     (ba7816bf...015ad).
  4. It prints `tlstest: PASS bearssl sha256 matches NIST vectors`.

If any of those assertions fail, the BearSSL build is suspect:
either inner.h autodetected the wrong int width, the mem* shims in
libc/cstring.c are wrong, or the archive index isn't being walked.

The hex digests are 64 chars and the serial console wraps at 80,
so digests print across two lines.  We squash whitespace out of
the captured tail before doing substring matches so the search
ignores the wrap.

Modelled after scripts/test_getrand.py.  Note we still pass
`-object rng-random` even though SHA-256 doesn't consume entropy:
parity with the rest of the regression boot config keeps log diffs
manageable and lets us bisect against other tests cleanly.
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-tlstest.sock"


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


EMPTY_HEX = b"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
ABC_HEX   = b"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
PASS_MSG  = b"tlstest: PASS bearssl sha256 matches NIST vectors"


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        ser.sendall(b"tlstest\n")
        out = read_until(ser, [PASS_MSG, b"FAIL"], 15.0)

        if b"[svc] unknown syscall" in out:
            print("FAIL: tlstest hit unknown-syscall")
            print(out[-2000:].decode("ascii", "replace"))
            return 1
        if b"kernel returned" in out:
            print("FAIL: tlstest reported a kernel error")
            print(out[-2000:].decode("ascii", "replace"))
            return 1

        # Slice forward from the command echo.  We want the FIRST
        # occurrence of b"tlstest" (the shell echoing our command),
        # not the last (which is the trailing "tlstest: PASS ..."
        # line and would slice off the digest output above it).
        idx = out.find(b"tlstest")
        tail = out[idx:] if idx >= 0 else out

        # Hex digests are 64 chars; the serial console wraps near
        # column 80, so each digest line splits across two physical
        # lines.  Strip whitespace before substring search.
        tail_flat = re.sub(rb"\s+", b"", tail)

        if EMPTY_HEX not in tail_flat:
            print(f"FAIL: expected SHA-256(empty) = {EMPTY_HEX.decode()} "
                  "not present in output")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print(f"PASS: sha256(empty) = {EMPTY_HEX.decode()}")

        if ABC_HEX not in tail_flat:
            print(f"FAIL: expected SHA-256(\"abc\") = {ABC_HEX.decode()} "
                  "not present in output")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print(f"PASS: sha256(\"abc\") = {ABC_HEX.decode()}")

        if PASS_MSG not in tail:
            print("FAIL: tlstest never emitted its summary PASS line")
            print(tail[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: chapter 124 bearssl link/run smoke test")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

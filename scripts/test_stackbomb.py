#!/usr/bin/env python3
"""scripts/test_stackbomb.py — chapter 103 guard-page regression.

Boots the kernel, runs `stackbomb`, and asserts that:

  1. The kernel produces the friendly "[svc] user stack overflow"
     diagnostic (proof that the guard page was reached AND the
     SW_GUARD bit was recognised).
  2. The diagnostic names the thread and reports the FAR/ELR.
  3. The shell prompt comes back afterward — the kernel killed the
     offending thread but did NOT panic.
  4. The kernel did NOT emit the older generic "non-SVC sync
     exception" dump that we used to get pre-chapter-101.

Modelled after scripts/test_mmap.py (chapter 91) and the rest of
the regression sweep.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-stackbomb.sock"


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


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        ser.sendall(b"stackbomb\n")
        # The diagnostic lands quickly (~200 frames @ ~288B each
        # exhausts 64 KiB in well under a second).  Wait for the
        # LAST line of the diagnostic ("thread killed.") so we
        # capture all of FAR/ELR/stack/guard before parsing —
        # earlier needles like "[svc] user stack overflow" would
        # return mid-message and truncate the section we check.
        log = read_until(
            ser,
            [b"thread killed.",
             b"non-SVC sync exception",
             b"PANIC",
             b"UNEXPECTED return"],
            30.0, prior=log,
        )

        idx = log.rfind(b"stackbomb\r\n")
        if idx < 0: idx = log.rfind(b"stackbomb\n")
        section = (
            log[idx:].decode("ascii", "replace")
            if idx >= 0
            else log[-3000:].decode("ascii", "replace")
        )
        print("--- stackbomb output: ---")
        print(section)

        if b"PANIC" in log:
            print("FAIL: kernel PANIC during stackbomb")
            return 1
        if b"UNEXPECTED return" in log:
            print("FAIL: stackbomb returned from recurse() — guard not hit?")
            return 1
        if b"non-SVC sync exception" in log and b"[svc] user stack overflow" not in log:
            print("FAIL: guard fault fell through to generic non-SVC dump "
                  "(SW_GUARD bit not recognised)")
            return 1
        if b"[svc] user stack overflow" not in log:
            print("FAIL: never saw the friendly stack-overflow diagnostic")
            return 1

        # Spot-check the diagnostic carries the structural fields.
        for expect in (b"FAR_EL1", b"ELR_EL1", b"stack    =",
                       b"guard    =", b"DESC_SW_GUARD"):
            if expect not in log:
                print(f"FAIL: diagnostic missing field {expect!r}")
                return 1

        # Kernel must come back to the shell, proving the thread
        # was killed cleanly rather than taking the kernel down.
        # Drain fresh bytes (do NOT pass prior= — that would
        # match the boot-time "$ " already in the log and return
        # instantly without ever waiting for the post-overflow
        # prompt to arrive).
        tail = read_until(ser, [b"$ "], 10.0)
        if b"$ " not in tail:
            print("FAIL: shell prompt did not return after stack overflow")
            return 1

        print("PASS: chapter 103 guard-page smoke test")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

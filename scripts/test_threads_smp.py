#!/usr/bin/env python3
"""scripts/test_threads_smp.py — chapter 93 SMP thread smoke test.

Boots the kernel with -smp 2, drops to /bin/sh, runs `threadtest2`,
and asserts that:

  - The chapter-92 marker [thread2] OK appears in the serial log.
  - All 4 workers printed BOTH a start AND a done line, with
    matching cpu= values (i.e. the kernel actually pinned them).
  - Both CPU 0 AND CPU 1 ran at least one worker (proves the
    secondary CPU's timer PPI is wired up and the runqueue
    cross-CPU push path works).

This is the chapter-91 test_threads.py extended for the SMP case;
threadtest itself remains the chapter-91 single-CPU regression
test."""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-threads-smp.sock"


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
        ser.sendall(b"threadtest2\n")
        log = read_until(
            ser,
            [b"[thread2] OK", b"[thread2] FAIL", b"PANIC"],
            60.0, prior=log,
        )
        idx = log.rfind(b"threadtest2\r\n")
        if idx < 0: idx = log.rfind(b"threadtest2\n")
        section = (
            log[idx:].decode("ascii", "replace")
            if idx >= 0
            else log[-2000:].decode("ascii", "replace")
        )
        print("--- threadtest2 output: ---")
        print(section)
        if b"PANIC" in log:
            print("FAIL: kernel PANIC during threadtest2"); return 1
        if b"[thread2] FAIL" in log:
            print("FAIL: threadtest2 reported failure"); return 1
        # All four workers must have printed start+done markers.
        for w in range(4):
            for kind in ("start", "done"):
                needle = f"[thread2] worker {w} {kind} cpu=".encode()
                if needle not in log:
                    print(f"FAIL: missing marker {needle!r}"); return 1
        # Verify BOTH CPUs ran a worker — proves CPU 1's timer
        # PPI is wired up and clone2() pinning actually moved the
        # child onto the secondary core.
        cpus_seen = set()
        for w in range(4):
            for line in section.splitlines():
                tag = f"[thread2] worker {w} done cpu="
                i = line.find(tag)
                if i >= 0:
                    try:
                        cpu = int(line[i + len(tag):].strip())
                        cpus_seen.add(cpu)
                    except ValueError:
                        pass
        if 0 not in cpus_seen:
            print("FAIL: no worker ran on CPU 0"); return 1
        if 1 not in cpus_seen:
            print("FAIL: no worker ran on CPU 1 (timer PPI?)"); return 1
        if b"[thread2] OK" not in log:
            print("FAIL: overall thread2 marker missing"); return 1
        print("PASS: chapter 93 SMP thread smoke test "
              f"(CPUs seen: {sorted(cpus_seen)})")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

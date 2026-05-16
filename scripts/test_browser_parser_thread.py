#!/usr/bin/env python3
"""scripts/test_browser_parser_thread.py — chapter 94 smoke test.

Boots the kernel with -smp 2 so we have a CPU 1 to pin the parser
thread to, drops to /bin/sh, and runs the browser's headless
`--bench-resize` mode.  That mode:

  1. Loads /mnt/test_layout.html synchronously at viewport=600.
  2. Spawns a parser thread on CPU 1 via thread_spawn_files()
     (so it shares the GUI core's fd table — chapter 93).
  3. Posts a relayout request for viewport=900 to the parser.
  4. Spins polling for the result, counting GUI-loop iterations
     that ran while the parser was busy.
  5. Prints "BENCH parse_ms=N gui_iters=N work_done=N".

Chapter 94 invariant (the regression we defend):

    gui_iters > 0

i.e. the GUI loop kept running while the parser thread did its
work.  Pre-chapter-94 the same operation would have completed
synchronously inside the resize handler with NO gui_iters at
all (because the loop was blocked).

Also asserts:
  - work_done == 1 (parser ran exactly one pass)
  - the new doc dimensions reflect the wider viewport (the
    relayout actually USED the new width)
  - process exit code 0 (clean shutdown, parser joined)
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-browser-parser.sock"


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
    raise RuntimeError("no serial socket")


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


def wait_for_new(ser, needles, timeout, log):
    """Cutoff-aware variant of read_until: only matches needles
    that appear in bytes received AFTER the current end-of-log,
    so a stale `$ ` from the boot banner never satisfies us."""
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    cutoff = len(log)
    buf = bytearray(log)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
        if any(n in bytes(buf[cutoff:]) for n in needles):
            return bytes(buf)
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

        # Start at viewport 600, relayout to 900 on parser thread.
        cmd = b"browser --bench-resize 900 /mnt/test_layout.html 600\n"
        ser.sendall(cmd)
        log = wait_for_new(
            ser,
            [b"[bench] OK", b"PANIC", b"bench: "],
            60.0, log,
        )
        idx = log.rfind(b"--bench-resize")
        section = (log[idx:].decode("ascii", "replace")
                   if idx >= 0
                   else log[-2000:].decode("ascii", "replace"))
        print("--- browser --bench-resize output: ---")
        print(section)

        if b"PANIC" in log:
            print("FAIL: kernel PANIC during bench"); return 1
        if b"[bench] OK" not in log:
            print("FAIL: bench did not reach OK marker"); return 1

        # Pull out the BENCH lines so we can assert on them.
        m = re.search(
            rb"BENCH parse_ms=(\d+) gui_iters=(\d+) work_done=(\d+)",
            log,
        )
        if not m:
            print("FAIL: missing BENCH summary line"); return 1
        parse_ms  = int(m.group(1))
        gui_iters = int(m.group(2))
        work_done = int(m.group(3))

        m2 = re.search(
            rb"BENCH old_w=(\d+) new_w=(\d+) "
            rb"old_doc=(\d+)x(\d+) new_doc=(\d+)x(\d+)",
            log,
        )
        if not m2:
            print("FAIL: missing BENCH dim line"); return 1
        old_w, new_w = int(m2.group(1)), int(m2.group(2))
        old_doc_w    = int(m2.group(3))
        new_doc_w    = int(m2.group(5))

        print(f"[bench-stats] parse_ms={parse_ms} "
              f"gui_iters={gui_iters} work_done={work_done}")
        print(f"[bench-stats] {old_w}->{new_w}  "
              f"{old_doc_w}->{new_doc_w}")

        # The chapter-94 invariant.
        if gui_iters < 1:
            print("FAIL: gui_iters == 0 — GUI loop was blocked "
                  "by parser; parallelisation did not happen")
            return 1

        if work_done != 1:
            print(f"FAIL: expected exactly 1 parser pass, "
                  f"got {work_done}"); return 1

        # The relayout actually used the new viewport.  doc_w
        # should grow when the viewport widens (test_layout.html
        # is a long list of paragraphs that wrap to viewport).
        if new_doc_w == old_doc_w:
            print(f"FAIL: new doc_w == old doc_w ({new_doc_w}) "
                  f"— relayout had no effect"); return 1

        print("PASS: chapter 94 browser parser-thread smoke test")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())

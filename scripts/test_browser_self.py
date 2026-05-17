#!/usr/bin/env python3
"""scripts/test_browser_self.py -- chapter 106c capstone test.

The end-to-end loop: in-guest /bin/browser fetches a page from
in-guest /bin/httpd over the lo0 loopback interface, no host
machinery in the request path.  Topology (every arrow stays
inside the guest):

  /bin/browser
     -> socket_connect(127.0.0.1, 80)
     -> kernel tcp_send (chapter 106 loopback short-circuit)
     -> kernel tcp_handle
     -> /bin/httpd's socket_accept (auto-spawned by init)
     -> open("/mnt/test.html")
     -> VFS dispatch into OSFS
     -> read() the html bytes
     -> kernel tcp_send back through lo0
     -> /bin/browser's tcp_recv
     -> tokenize, parse, layout, paint

The test asserts:

  - init logged "[init] launching /bin/httpd 80" and httpd
    printed "httpd: listening on port 80" BEFORE the shell
    prompt appeared (proves the auto-spawn order in init.c).
  - `browser http://127.0.0.1:80/mnt/test.html 600` succeeds
    and prints the marker text from test.html ("Hello, &
    goodbye" and "The quick brown fox") into the painted
    output.
  - tcp_recv saw a non-zero byte count for the connection that
    served the request (proves data actually moved, not just
    the SYN handshake).
  - browser exited cleanly.

The boot is BARE-KERNEL deliberately: no virtio-gpu,
virtio-keyboard, virtio-tablet.  This keeps the test fast
(~6 s wall) and isolates the loopback path from the
yield-driven cursor pump that chapter 106b spent so much
effort fixing.  The desktop variant of this test
(test_browser_hn_serial_with_desktop.py) covers the GUI
version of the same fetch path against the chapter-106a
forwarding proxy.

Per `apps-must-use-features` policy in user memory: this is
the chapter-106c test, replacing the placeholder
test_browser_self.py mentioned in the chapter stub.
"""
import argparse, os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-self.sock"

# Markers from assets/osfs/test.html that we expect the browser
# to render.  Two layers of evidence so we catch regressions
# both in the network path (httpd serves bytes) and the render
# path (browser actually drew them).
#
# Render markers are expressed as REGEX-with-loose-whitespace
# because the chapter-71 plain paint mode spaces every glyph
# (so "Hello" renders as "H e l l o").  The pattern joins
# character classes with `\s+` so a single repaint pass through
# the box tree -- whatever its grid step -- matches.
PAGE_LITERAL = [
    # httpd's request log line proves the byte path:
    # /bin/httpd -> /mnt/test.html dispatch in serve_get.
    b"local GET /mnt/test.html -> 200",
    # browser's response log proves it parsed the headers
    # and the body is a non-empty text/html.
    b"HTTP/1.0 200 OK (text/html",
]
# Regexes against the painted output: each char from the source
# string, joined by `\s+` so the chapter-71 plain mode's per-glyph
# spacing matches.
def _loose(literal: bytes) -> bytes:
    parts = []
    for ch in literal.decode("ascii"):
        if ch == " ":
            parts.append(rb"\s+")
        else:
            parts.append(re.escape(ch.encode("ascii")))
            parts.append(rb"\s*")
    # Strip the trailing \s* so the regex doesn't overrun.
    if parts and parts[-1] == rb"\s*":
        parts.pop()
    return b"".join(parts)

PAGE_RENDERED_RES = [
    re.compile(_loose(b"Hello"), re.DOTALL),
    re.compile(_loose(b"goodbye"), re.DOTALL),
    re.compile(_loose(b"The quick brown fox"), re.DOTALL),
]

# init.c (chapter 106c) prints these BEFORE handing control to
# the shell.  All three must appear in the boot transcript or
# the auto-spawn regressed.
BOOT_MARKERS = [
    b"[init] launching /bin/httpd 80",
    b"httpd: listening on port 80",
]


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
        "-drive",  f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        # virtio-net required for the kernel's TCP path to come
        # up cleanly (init_netd's DHCP probe, the chapter-103
        # boot self-test).  Loopback (chapter 106) is decoupled
        # from the NIC -- 127.0.0.1 traffic short-circuits at
        # ip_send_packet -- so the in-guest fetch doesn't go
        # through this device.  But booting without it makes
        # netd grumble on serial, which we want to keep quiet.
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
    """Read until `needle` appears or timeout elapses.  Returns
    (found_bool, accumulated_bytes)."""
    if isinstance(needle, str): needle = needle.encode()
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return True, buf
    return False, buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot-timeout", type=float, default=120.0,
                    help="Seconds to wait for the shell prompt "
                         "(default: %(default)s; the chapter-103 "
                         "boot self-test camps on accept for ~30s)")
    ap.add_argument("--fetch-timeout", type=float, default=15.0,
                    help="Seconds to wait for the browser to exit "
                         "(default: %(default)s)")
    ap.add_argument("--save-log",
                    help="Write the serial transcript to this path")
    args = ap.parse_args()

    for required in ("build/kernel.elf",
                     "build/userspace/init/init.elf",
                     "build/userspace/httpd/httpd.elf",
                     "build/userspace/browser/browser.elf"):
        if not os.path.exists(os.path.join(ROOT, required)):
            print(f"FAIL: {required} not built -- run `make -j` first.")
            return 1

    qemu = boot()
    transcript = []
    rc = 1

    try:
        ser = conn()

        # (1) Boot.  Drain until the shell prompt.  Whatever
        # init prints in the meantime is captured for the
        # auto-spawn assertions below.
        ok, buf = wait_for(ser, b"$ ", args.boot_timeout)
        transcript.append(buf)
        if not ok:
            print(f"FAIL: shell prompt not reached in {args.boot_timeout}s")
            return 1
        print(f"PASS: shell prompt reached")

        # (2) Auto-spawn assertions.  init.c (chapter 106c) is
        # supposed to spawn httpd on port 80 BEFORE running the
        # shell.  The boot transcript proves that ordering.
        boot_log = b"".join(transcript)
        for m in BOOT_MARKERS:
            if m not in boot_log:
                print(f"FAIL: boot transcript missing {m!r}")
                if args.save_log:
                    with open(args.save_log, "wb") as f: f.write(boot_log)
                return 1
        print("PASS: init auto-spawned httpd on port 80")

        # (3) Hand-typed fetch from the serial sh.  Explicit
        # http://127.0.0.1:80/mnt/test.html so the URL takes
        # browser.c's case-2 passthrough (no proxy rewrite);
        # the fetch goes straight to the in-guest httpd we
        # just confirmed is listening.
        ser.sendall(b"browser --timing "
                    b"http://127.0.0.1:80/mnt/test.html 600\n")

        t0 = time.time()
        ok, buf = wait_for(ser,
                           b"thread '/bin/browser' exited",
                           args.fetch_timeout)
        t_exit = time.time()
        transcript.append(buf)
        if not ok:
            print(f"FAIL: browser did not exit within "
                  f"{args.fetch_timeout}s")
            return 1
        fetch_wall_ms = int((t_exit - t0) * 1000)
        print(f"PASS: browser exited in {fetch_wall_ms} ms")

        # (4) Render assertions.  Two checks: the literal
        # log lines from httpd + browser prove the byte path,
        # and the loose-whitespace regex on the painted output
        # proves the bytes survived parse/layout/paint into
        # visible glyphs (chapter-71 plain mode spaces them).
        log = b"".join(transcript)
        for m in PAGE_LITERAL:
            if m not in log:
                print(f"FAIL: log missing {m!r}")
                if args.save_log:
                    with open(args.save_log, "wb") as f: f.write(log)
                return 1
        print("PASS: httpd served test.html and browser parsed "
              "the response headers")
        for rx in PAGE_RENDERED_RES:
            if not rx.search(log):
                print(f"FAIL: painted output missing {rx.pattern!r}")
                if args.save_log:
                    with open(args.save_log, "wb") as f: f.write(log)
                return 1
        print("PASS: painted output contains expected text glyphs")

        # (5) TCP byte-flow assertion.  Every chapter-106c
        # success leaves a `[tcp] release` line for the
        # browser's connection with rx_total > 0 (the html
        # body must have crossed the wire).  We don't insist
        # on a particular cid because the chapter-103
        # self-test consumes the first few.
        rx_pat = re.compile(
            rb"\[tcp\] release cid=0x[0-9a-f]+ state=0x[0-9a-f]+ "
            rb"lport=0x[0-9a-f]+ rport=0x0+0050 rip=0x0+7f000001 "
            rb"rx_total=0x([0-9a-f]+)")
        max_rx = 0
        for m in rx_pat.finditer(log):
            rx = int(m.group(1), 16)
            if rx > max_rx: max_rx = rx
        if max_rx == 0:
            print("FAIL: no loopback connection with rx_total>0 "
                  "(does port 80 -> rport=0x0050 grep match?)")
            return 1
        print(f"PASS: tcp release saw rx_total=0x{max_rx:x} "
              f"({max_rx} bytes) over loopback")

        if args.save_log:
            with open(args.save_log, "wb") as f: f.write(log)
            print(f"transcript saved to {args.save_log}")

        print("\nVERDICT: PASS -- the loop is closed.")
        rc = 0
        return rc

    finally:
        try: qemu.terminate()
        except Exception: pass
        try: qemu.wait(timeout=5)
        except Exception:
            try: qemu.kill()
            except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

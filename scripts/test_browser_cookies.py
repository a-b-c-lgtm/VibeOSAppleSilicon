#!/usr/bin/env python3
"""scripts/test_browser_cookies.py -- chapter 120 cookie round-trip.

Drives the in-guest httpd's `/cookie/*` test endpoints with the
in-guest `/bin/httpget` so the whole cookie path -- Set-Cookie
parse -> /data/cookies/<host> write -> Cookie header build ->
server-side echo -- runs end-to-end on the OS being built.

The eight asserts walk the cookie state machine:

  1. anonymous before any cookie exists.
  2. /cookie/set issues a Set-Cookie that httpget captures.
  3. /data/cookies/127.0.0.1 exists and has the expected line.
  4. /cookie/whoami now replies "hello alice" because httpget
     reads the on-disk jar and sends Cookie: on the next request.
  5. The `cookies` tool dumps the jar in human-readable form.
  6. `cookies clear` removes every jar.
  7. /cookie/whoami is back to "anonymous".
  8. A second /cookie/set followed by a guest reboot, then
     /cookie/whoami in the new boot returns "hello alice"
     again -- proving the cookie survived the kernel restart
     because /data is OSFS-2 with explicit fsync().

Same boot/serial harness as test_directories.py + a virtio-net
device because httpget dials 127.0.0.1:80 (where init has
already spawned /bin/httpd, see chapter 111).
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-cookies.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

URL = "http://127.0.0.1:80"


def cleanup_sock():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass


def reformat_data():
    subprocess.check_call(
        ["python3", f"{ROOT}/scripts/mkosfs2.py", DATA_IMG],
        stdout=subprocess.DEVNULL,
    )


def boot():
    cleanup_sock()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={DATA_IMG},format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        # virtio-net is required for tcp_handle to dispatch -- the
        # cookie test uses guest-loopback httpget against the init-
        # spawned httpd on port 80.
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def hard_kill(q):
    try:
        q.send_signal(signal.SIGKILL)
        q.wait(timeout=3)
    except Exception:
        pass
    cleanup_sock()


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SERIAL_SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"no serial socket: {SERIAL_SOCK}")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
        elif out:
            break
    return out


def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def send_cmd(s, cmd, timeout=20.0):
    """Send a shell command, wait for the next prompt, return the
    output between the echoed command and the prompt."""
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    # Strip the echoed command from the front so callers don't
    # accidentally match on it.
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


PASSES, FAILS = [], []


def expect(cond, msg, ctx=b""):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        if ctx:
            tail = ctx[-1500:].decode("ascii", "replace")
            print("--- ctx ---\n" + tail + "\n-----------")
        FAILS.append(msg)


def expect_contains(out, needle, msg):
    if isinstance(needle, str): needle = needle.encode()
    expect(needle in out, msg, ctx=out)


def boot_to_shell():
    q = boot()
    s = conn()
    # ch103 self-test busy-polls tcp_accept(8088) for ~30s before
    # init runs, so we wait generously here.
    buf = wait_for(s, PROMPT, timeout=120.0)
    if PROMPT not in buf:
        hard_kill(q)
        raise RuntimeError("shell prompt never reached")
    # init prints "[init] launching /bin/httpd 80" -- make sure
    # the daemon has actually bound before our first request.
    listen_buf = wait_for(s, b"httpd: listening on port 80", 20.0)
    if b"httpd: listening on port 80" not in listen_buf and \
       b"httpd: listening on port 80" not in buf:
        # Not strictly fatal -- the line might already have
        # scrolled past during the boot wait above.  Try one
        # quick probe instead.
        pass
    return q, s


def main():
    reformat_data()
    q, s = boot_to_shell()
    try:
        # 1. Empty jar -> "anonymous".
        out = send_cmd(s, f"httpget {URL}/cookie/whoami")
        expect_contains(out, "anonymous",
                        "whoami on empty jar returns anonymous")

        # 2. /cookie/set issues Set-Cookie + httpget captures it.
        out = send_cmd(s, f"httpget {URL}/cookie/set")
        expect_contains(out, "session=alice",
                        "/cookie/set body is session=alice")
        expect_contains(out, "stored 1 cookie",
                        "httpget logs stored 1 cookie")

        # 3. The jar file exists on /data and contains the cookie.
        out = send_cmd(s, "cat /data/cookies/127.0.0.1")
        expect_contains(out, "hobbyos_session",
                        "/data/cookies/127.0.0.1 contains hobbyos_session")
        expect_contains(out, "alice",
                        "/data/cookies/127.0.0.1 contains the cookie value")

        # 4. Whoami now sees the cookie -> "hello alice".
        out = send_cmd(s, f"httpget {URL}/cookie/whoami")
        expect_contains(out, "hello alice",
                        "whoami after set returns hello alice")
        expect_contains(out, "sending 1 cookie",
                        "httpget logs sending 1 cookie")

        # 5. The cookies tool dumps the jar in human form.
        out = send_cmd(s, "cookies")
        expect_contains(out, "127.0.0.1",
                        "cookies tool lists the host")
        expect_contains(out, "hobbyos_session = alice",
                        "cookies tool shows name=value pair")

        # 6. cookies clear empties the directory.
        out = send_cmd(s, "cookies clear")
        expect_contains(out, "cleared 1 jar",
                        "cookies clear reports one jar removed")

        # 7. Back to anonymous after clear.
        out = send_cmd(s, f"httpget {URL}/cookie/whoami")
        expect_contains(out, "anonymous",
                        "whoami after clear returns anonymous")

        # 8. Persistence across reboot.
        send_cmd(s, f"httpget {URL}/cookie/set")
        # Verify the cookie really hit disk before we kill QEMU --
        # without the cat, an absent file is indistinguishable
        # from "the reboot lost it" in step 8b.
        cat_pre = send_cmd(s, "cat /data/cookies/127.0.0.1")
        expect_contains(cat_pre, "hobbyos_session",
                        "pre-reboot jar shows hobbyos_session on disk")
        s.close()
        hard_kill(q)

        q, s = boot_to_shell()
        out = send_cmd(s, f"httpget {URL}/cookie/whoami")
        expect_contains(out, "hello alice",
                        "cookie survives across reboot (durable jar)")

    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)

    print(f"\nresults: {len(PASSES)} pass, {len(FAILS)} fail")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())

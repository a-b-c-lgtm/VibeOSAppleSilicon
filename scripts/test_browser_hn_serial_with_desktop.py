#!/usr/bin/env python3
"""scripts/test_browser_hn_serial_with_desktop.py -- triage variant.

Boots to desktop, then runs the browser from the SERIAL kernel
shell (not from inside a gui_term).  Because fd 1 = FD_CONSOLE
on the serial shell, the browser's `[timing]` lines DO appear on
serial here.  This lets us measure fetch+parse+render durations
under "desktop is up but no gui_term focused" load.

Comparison:

  scripts/test_browser_hn_repeat.py       -- bare kernel, no GUI: ~1.3s
  scripts/test_browser_hn_desktop.py      -- desktop + gui_term:  ~14.7s
  THIS                                    -- desktop, no gui_term: ?

If this is fast (close to 1.3s), the cost is in gui_term itself
(its event loop, pty plumbing, render thread).  If this is slow
(close to 14.7s), the cost is in the desktop's background
yielders (wm, taskbar, clock, wallpaper, launcher, cursor).
"""
import argparse, os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-hn-ser.sock"
SERIAL_SOCK = "/tmp/osdev-serial-hn-ser.sock"
HOST_PROXY_PORT = 18092


def start_host_proxy(port):
    p = subprocess.Popen(
        ["python3", os.path.join(ROOT, "scripts", "https_proxy.py"),
         str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        line = p.stdout.readline()
        if not line:
            time.sleep(0.05); continue
        sys.stderr.write(f"[host-proxy] {line}")
        if "listening" in line: return p
    raise RuntimeError("host https_proxy.py never reported listening")


def cleanup():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass


def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64", "-M", "virt,gic-version=3", "-cpu", "host",
        "-accel", "hvf", "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-qmp",    f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", "virtio-gpu-device,xres=1280,yres=800",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive",  f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
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


def drain(s, timeout, accum=None):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            try: c = s.recv(8192)
            except OSError: break
            if not c: break
            buf += c
            if accum is not None: accum.append(c)
    return buf


def wait_for(s, needle, timeout, accum=None):
    if isinstance(needle, str): needle = needle.encode()
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        chunk = drain(s, 0.4, accum)
        if chunk:
            buf += chunk
            if needle in buf: return True, buf
    return False, buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="news.ycombinator.com")
    ap.add_argument("--budget-sec", type=float, default=15.0)
    ap.add_argument("--save-log")
    args = ap.parse_args()

    host_proxy = start_host_proxy(HOST_PROXY_PORT)
    qemu = boot()
    transcript = []

    try:
        ser = conn(SERIAL_SOCK)
        ser.setblocking(False)
        qmp = conn(QMP_SOCK)
        # Drain QMP greeting + send capabilities (we don't need
        # qmp here, but the socket has to be accepted or qemu
        # will queue greeting forever).
        buf = b""
        while not buf.endswith(b"\n"):
            c = qmp.recv(4096)
            if not c: break
            buf += c

        ok, _ = wait_for(ser, b"$ ", 90.0, transcript)
        if not ok:
            print("FAIL: shell prompt not reached"); return 1
        print("PASS: kernel shell ready on serial")

        ser.sendall(f"export HTTPD_UPSTREAM=10.0.2.2:{HOST_PROXY_PORT}\n"
                    .encode())
        wait_for(ser, b"$ ", 3.0, transcript)
        ser.sendall(b"httpd 8080 &\n")
        ok, _ = wait_for(ser, b"httpd: listening", 5.0, transcript)
        if not ok:
            print("FAIL: httpd never reported listening"); return 1
        wait_for(ser, b"$ ", 2.0, transcript)
        print("PASS: httpd 8080 backgrounded")

        # Chapter 111: BR_DEFAULT_PROXY now points at the
        # init-spawned port-80 httpd, which has no upstream.
        # Override to the port-8080 httpd we just backgrounded
        # so the browser routes through OUR forwarder.
        ser.sendall(b"export BROWSER_PROXY=http://127.0.0.1:8080/\n")
        wait_for(ser, b"$ ", 3.0, transcript)
        print("PASS: BROWSER_PROXY pinned to local httpd 8080")

        # Browser from the SERIAL sh.  Desktop apps (wm,
        # taskbar, clock, wallpaper, launcher, cursor) are all
        # already running as init's other children -- the
        # serial-sh's foreground is the browser, but the
        # background yielders are intact.  The browser's stdout
        # is FD_CONSOLE = serial, so [timing] lines DO arrive
        # here.
        t0 = time.time()
        ser.sendall(f"browser --timing {args.url} 600\n".encode())

        # Wait for any [timing] line or the prompt to come back.
        ok_exit, _ = wait_for(ser,
            b"thread '/bin/browser' exited",
            args.budget_sec + 5.0, transcript)
        t_exit = time.time()
        fetch_wall_ms = int((t_exit - t0) * 1000) if ok_exit else None

        log = b"".join(transcript).decode("ascii", "replace")
        if args.save_log:
            with open(args.save_log, "w") as f: f.write(log)
            print(f"transcript saved to {args.save_log}")

        if not ok_exit:
            print(f"FAIL: never saw browser sys_exit (budget "
                  f"{args.budget_sec}s)")
            tail = b"".join(transcript)[-2000:]
            print("--- serial tail ---")
            print(tail.decode("ascii", "replace"))
            return 1

        print(f"\n  fetch_wall_ms (keystroke -> sys_exit): "
              f"{fetch_wall_ms} ms")

        # All [timing] lines from the browser, if any.
        timings = re.findall(r"\[timing\] (\S[^\n]*?)\s+(\d+) ms", log)
        if timings:
            print("  browser pipeline:")
            for name, ms in timings:
                print(f"    {name:30s} {ms} ms")
        else:
            print("  WARN: no [timing] lines from browser")

        rejects = re.findall(r"\[tcp\] reject cid=0x([0-9a-fA-F]+)", log)
        if rejects:
            print(f"  WARN: {len(rejects)} TCP reject(s)")

        verdict = (fetch_wall_ms is not None
                   and fetch_wall_ms <= int(args.budget_sec * 1000))
        if verdict:
            print("\nVERDICT: PASS")
            return 0
        else:
            print(f"\nVERDICT: FAIL ({fetch_wall_ms} ms > "
                  f"{int(args.budget_sec*1000)} ms budget)")
            return 1

    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception:
            try: qemu.kill()
            except Exception: pass
        try: host_proxy.terminate()
        except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

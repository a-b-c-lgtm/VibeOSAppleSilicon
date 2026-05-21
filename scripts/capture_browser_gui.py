#!/usr/bin/env python3
"""scripts/capture_browser_gui.py — boot QEMU with full GUI stack
(virtio-gpu + virtio-keyboard + virtio-tablet + virtio-net), wait
for the desktop, launch `/bin/browser <url>` in GUI mode, give it
time to fetch + lay out + render, then screendump the framebuffer
to a PNG.

Use this to iterate visually on the browser without firing up
a real GUI VM:

    python3 scripts/capture_browser_gui.py \
        http://10.0.2.2:8080/plaintextworld.com/block

Outputs /tmp/osdev-browser-fb.png (1280x800) and prints its path.

Optional flags:
  --viewport N      initial viewport width in CSS px (default 800)
  --wait SEC        seconds to wait after launching browser before
                    the screendump (default 12)
  --width W         framebuffer width  (default 1280)
  --height H        framebuffer height (default 800)
  --keep            don't kill QEMU on exit (useful for live poke;
                    QMP socket stays at /tmp/osdev-qmp-br.sock,
                    serial at /tmp/osdev-serial-br.sock)
  --keys "k1 k2..." after the first screendump, send these key
                    presses (one per token) over the virtio keyboard
                    and take a SECOND screendump.  Token format:
                    a single ASCII char, "SPACE", "ESC", or "Q".
                    Useful for paging / quit testing.
"""
import argparse, json, os, select, socket, subprocess, sys, time

ROOT        = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-br.sock"
SERIAL_SOCK = "/tmp/osdev-serial-br.sock"
PPM_PATH    = "/tmp/osdev-browser-fb.ppm"
PNG_PATH    = "/tmp/osdev-browser-fb.png"
PPM_PATH2   = "/tmp/osdev-browser-fb-2.ppm"
PNG_PATH2   = "/tmp/osdev-browser-fb-2.png"


def cleanup():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except FileNotFoundError: pass


def boot(fb_w, fb_h):
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-qmp",    f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", f"virtio-gpu-device,xres={fb_w},yres={fb_h}",
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


def conn(path, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError(f"no socket: {path}")


def qrl(qmp):
    buf = b""
    while not buf.endswith(b"\n"):
        c = qmp.recv(4096)
        if not c: raise RuntimeError("qmp closed")
        buf += c
    return json.loads(buf)


def qsend(qmp, obj):
    qmp.sendall((json.dumps(obj) + "\n").encode())
    while True:
        m = qrl(qmp)
        if "return" in m or "error" in m: return m


def drain(s, until_secs):
    out = b""
    while time.time() < until_secs:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out


def wait_for(s, needle, timeout, tick=5.0):
    """Wait up to `timeout` seconds for `needle` on socket `s`.
    Prints a progress dot every `tick` seconds.  Returns (found, buf)."""
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    last_tick = time.time()
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return True, buf
        if time.time() - last_tick >= tick:
            remaining = int(deadline - time.time())
            print(f"[capture]   ... still waiting "
                  f"({remaining}s left, {len(buf)}B serial)")
            last_tick = time.time()
    return False, buf


def screendump(qmp, ppm_path, png_path):
    try: os.unlink(ppm_path)
    except FileNotFoundError: pass
    qsend(qmp, {"execute": "screendump",
                 "arguments": {"filename": ppm_path}})
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if os.path.exists(ppm_path) and os.path.getsize(ppm_path) > 0:
            time.sleep(0.1); break
        time.sleep(0.05)
    if not os.path.exists(ppm_path):
        print(f"WARN: screendump did not produce {ppm_path}", file=sys.stderr)
        return False
    # PPM -> PNG via macOS sips (no third-party deps required).
    try: os.unlink(png_path)
    except FileNotFoundError: pass
    r = subprocess.run(
        ["sips", "-s", "format", "png", ppm_path, "--out", png_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0 or not os.path.exists(png_path):
        print(f"WARN: sips PPM->PNG conversion failed; PPM still at "
              f"{ppm_path}", file=sys.stderr)
        return False
    return True


# QEMU "send-key" expects a QKeyCodeName.  Map a small subset.
_KEYMAP = {
    "SPACE": "spc", "ESC": "esc", "ENTER": "ret", "TAB": "tab",
    "UP": "up", "DOWN": "down", "LEFT": "left", "RIGHT": "right",
    "HOME": "home", "END": "end", "PGUP": "pgup", "PGDN": "pgdn",
}
def qmp_send_key(qmp, tok):
    if len(tok) == 1 and tok.isalpha():
        name = tok.lower()
    elif len(tok) == 1 and tok.isdigit():
        name = tok
    else:
        name = _KEYMAP.get(tok.upper())
        if not name:
            print(f"WARN: unknown key token {tok!r}", file=sys.stderr)
            return
    qsend(qmp, {"execute": "send-key",
                 "arguments": {"keys": [{"type": "qcode", "data": name}]}})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--viewport", type=int, default=800)
    ap.add_argument("--wait",     type=float, default=90.0,
                    help="seconds to wait for '[browser] gui window=' on "
                         "serial before screendumping (default 90; PTW "
                         "fetch + layout typically takes 60-80s)")
    ap.add_argument("--width",    type=int, default=1280)
    ap.add_argument("--height",   type=int, default=800)
    ap.add_argument("--keep",     action="store_true")
    ap.add_argument("--keys",     default="",
                    help="space-separated key tokens to send after the "
                         "first screendump (e.g. 'SPACE SPACE Q')")
    ap.add_argument("--timing",   action="store_true",
                    help="pass --timing to browser to print per-stage "
                         "uptime_ms() deltas on serial")
    args = ap.parse_args()

    q = boot(args.width, args.height)
    try:
        ser = conn(SERIAL_SOCK)
        qmp = conn(QMP_SOCK)
        qrl(qmp); qsend(qmp, {"execute": "qmp_capabilities"})

        ok, _buf = wait_for(ser, b"$ ", 120.0)
        if not ok:
            try:
                with open("/tmp/osdev-browser-serial.log", "wb") as f:
                    f.write(_buf)
                print(f"[capture] serial transcript (FAIL): "
                      f"/tmp/osdev-browser-serial.log ({len(_buf)}B)")
            except OSError: pass
            print("FAIL: shell prompt not reached"); return 1

        timing_flag = "--timing " if args.timing else ""
        cmd = f"browser --gui {timing_flag}{args.url} {args.viewport}\n".encode()
        ser.sendall(cmd)
        print(f"[capture] sent: {cmd.decode().rstrip()}")

        # Wait for either an obvious failure message or the layout to
        # render.  We can't easily detect "ready" without changing the
        # browser, so just sleep for --wait seconds while logging.
        ok, buf = wait_for(ser, b"[browser] gui window=", args.wait)
        # Always persist the post-command serial transcript so we can
        # diagnose failures (e.g. fetch errors, ELF load failures).
        try:
            with open("/tmp/osdev-browser-serial.log", "wb") as f:
                f.write(buf)
            print(f"[capture] serial transcript: "
                  f"/tmp/osdev-browser-serial.log ({len(buf)}B)")
        except OSError as e:
            print(f"WARN: couldn't write serial log: {e}", file=sys.stderr)
        if not ok:
            print("[capture] warning: no '[browser] gui window=' line "
                  "seen on serial yet, screendumping anyway")
        else:
            print(f"[capture] browser opened window: "
                  f"{buf.splitlines()[-1].decode(errors='replace')}")
        # Extra settle time for the layout + first paint to land.
        time.sleep(2.0)

        if screendump(qmp, PPM_PATH, PNG_PATH):
            print(f"[capture] screenshot saved: {PNG_PATH}")

        if args.keys:
            for tok in args.keys.split():
                qmp_send_key(qmp, tok)
                time.sleep(0.15)
            time.sleep(0.8)
            if screendump(qmp, PPM_PATH2, PNG_PATH2):
                print(f"[capture] post-keys screenshot saved: {PNG_PATH2}")

        if args.keep:
            print(f"[capture] --keep: leaving QEMU running. "
                  f"QMP={QMP_SOCK} serial={SERIAL_SOCK} pid={q.pid}")
            q = None
        return 0
    finally:
        if q is not None:
            try: q.terminate(); q.wait(timeout=3)
            except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())

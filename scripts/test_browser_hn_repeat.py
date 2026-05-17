#!/usr/bin/env python3
"""scripts/test_browser_hn_repeat.py -- chapter 106b GUI-perf repro.

The original scripts/test_browser_hn_timings.py drives proxytest in
its default single-fetch mode (`--once` httpd, one browser).  That
passes in ~4 s -- the kernel/TCP bug it surfaced (FIN-acked-via-RTO
followed by tx_buf off-by-one stranding one byte) is fixed.

But the user is still seeing 30-40 s per page reload in the *GUI*
browser against the same setup (long-lived httpd 8080, GUI browser
typing news.ycombinator.com, then hitting reload).  proxytest's
single fetch can't reproduce that because:

  * `--once` httpd serves one connection then exits, so we never
    exercise back-to-back fetches through the same listener.
  * Plain-mode browser does 1 HTTP fetch (HTML only).  GUI mode
    does HTML + every external <link rel=stylesheet> + every
    <img src=...> + potential favicon -- a real HN load is ~30
    sub-fetches.

This test drives `proxytest --repeat 3 --timing` instead:
  * httpd is long-lived (no --once), serving 3 browser runs.
  * --timing makes the browser print [timing] lines for each
    pipeline stage (fetch / parse / image-decode / re-layout)
    so we can attribute slowdowns to a specific stage.

The test PASSES if every iteration completes in <BUDGET_SEC and
no [tcp] reject lines fire and no DRAIN_FD_MAX_BYTES overflow.

Lives next to test_browser_hn_timings.py; both share the same
HOST_PROXY_PORT.  Run from repo root with the kernel + ramfs
built (`make -j8` first).
"""
import argparse
import os
import re
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-hn-repeat.sock"
HOST_PROXY_PORT = 18091
DEFAULT_URL = "https://news.ycombinator.com/"

BOOT_TIMEOUT     = 120.0
DEFAULT_REPEAT   = 3
DEFAULT_BUDGET   = 90.0  # per iteration


def start_host_proxy(port):
    p = subprocess.Popen(
        ["python3", os.path.join(ROOT, "scripts", "https_proxy.py"),
         str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    deadline = time.time() + 5.0
    while time.time() < deadline:
        line = p.stdout.readline()
        if not line:
            time.sleep(0.05); continue
        sys.stderr.write(f"[host-proxy] {line}")
        if "listening" in line:
            return p
    raise RuntimeError("host https_proxy.py never reported listening")


def cleanup_sock():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass


def boot():
    cleanup_sock()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial_connect():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket")


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


# Persistent leftover buffer for wait_for.  Without this, a fast
# guest can produce two markers in a single drain window, and the
# second wait_for() loses the bytes between the first needle and
# whatever it's looking for next (those bytes are in `transcript`
# but not in this call's local `buf`).  Surfaced after the
# chapter-106b cursor-pump-on-every-yield fix made everything
# fast enough that consecutive proxytest markers arrive in the
# same 400 ms drain.
_wait_for_carry = bytearray()


def wait_for(s, needle, timeout, accum=None):
    global _wait_for_carry
    if isinstance(needle, str): needle = needle.encode()
    end = time.time() + timeout
    # Anything left over from the previous wait_for() drain.
    if needle in _wait_for_carry:
        idx = _wait_for_carry.index(needle) + len(needle)
        out = bytes(_wait_for_carry[:idx])
        del _wait_for_carry[:idx]
        return True, out
    while time.time() < end:
        chunk = drain(s, 0.4, accum)
        if chunk:
            _wait_for_carry += chunk
            if needle in _wait_for_carry:
                idx = _wait_for_carry.index(needle) + len(needle)
                out = bytes(_wait_for_carry[:idx])
                del _wait_for_carry[:idx]
                return True, out
    out = bytes(_wait_for_carry)
    _wait_for_carry.clear()
    return False, out


# ----------------------------------------------------------------
# Per-iteration metrics extracted from the captured transcript.
# ----------------------------------------------------------------

def parse_iterations(text):
    """Return list of dicts, one per `[proxytest] iter N/M ...
    wall=W ms` line."""
    rows = []
    for m in re.finditer(
            r"\[proxytest\] iter (\d+)/(\d+) browser exit code=(-?\d+) "
            r"wall=(\d+) ms",
            text):
        rows.append({
            "iter":  int(m.group(1)),
            "total": int(m.group(2)),
            "exit":  int(m.group(3)),
            "wall_ms": int(m.group(4)),
        })
    return rows


def parse_timings(text):
    """Return list of (stage_name, ms) pairs in order of appearance
    in the log.  Useful for attributing per-iteration slowdowns."""
    out = []
    for m in re.finditer(r"\[timing\] (\S[^\n]*?)\s+(\d+) ms", text):
        out.append((m.group(1).strip(), int(m.group(2))))
    return out


def parse_tcp_lines(text):
    """Return summary of [tcp] connect/release/reject lines."""
    connects = re.findall(
        r"\[tcp\] connect cid=0x([0-9a-fA-F]+) lport=0x([0-9a-fA-F]+) "
        r"-> 0x([0-9a-fA-F]+):0x([0-9a-fA-F]+)", text)
    releases = re.findall(
        r"\[tcp\] release cid=0x([0-9a-fA-F]+) state=0x([0-9a-fA-F]+) "
        r"lport=0x([0-9a-fA-F]+) rport=0x([0-9a-fA-F]+) "
        r"rip=0x([0-9a-fA-F]+) rx_total=0x([0-9a-fA-F]+)", text)
    rejects = re.findall(
        r"\[tcp\] reject cid=0x([0-9a-fA-F]+) n=0x([0-9a-fA-F]+)", text)
    rx_totals = re.findall(
        r"\[tcp\] cid=0x([0-9a-fA-F]+) state=0x([0-9a-fA-F]+) "
        r"rx_total=0x([0-9a-fA-F]+)", text)
    return {
        "connects": len(connects),
        "releases": len(releases),
        "rejects":  rejects,
        "max_rx_per_cid": _max_rx(rx_totals),
    }


def _max_rx(rx_totals):
    """Pick the largest rx_total per cid."""
    out = {}
    for cid, st, tot in rx_totals:
        c = int(cid, 16)
        t = int(tot, 16)
        if out.get(c, -1) < t:
            out[c] = t
    return out


# ----------------------------------------------------------------
# Driver.
# ----------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--repeat", type=int, default=DEFAULT_REPEAT)
    ap.add_argument("--budget-sec", type=float, default=DEFAULT_BUDGET,
                    help="Per-iteration wall-clock budget (default %(default)s)")
    ap.add_argument("--proxy-port", type=int, default=HOST_PROXY_PORT)
    ap.add_argument("--save-log",
                    help="Write captured serial transcript to this path")
    ap.add_argument("--keep-running", action="store_true",
                    help="Leave QEMU + host proxy alive after the test")
    args = ap.parse_args()

    for required in ("build/kernel.elf",
                     "build/userspace/browser/browser.elf",
                     "build/userspace/httpd/httpd.elf",
                     "build/userspace/proxytest/proxytest.elf"):
        if not os.path.exists(os.path.join(ROOT, required)):
            print(f"FAIL: {required} not built -- run `make -j8` first.")
            return 1

    host_proxy = start_host_proxy(args.proxy_port)
    qemu = boot()
    transcript = []
    t0 = time.time()
    rc = 1

    try:
        ser = serial_connect()
        ser.setblocking(False)

        ok, _ = wait_for(ser, b"$ ", BOOT_TIMEOUT, transcript)
        if not ok:
            print(f"FAIL: shell prompt not reached in {BOOT_TIMEOUT}s")
            return 1
        print(f"PASS: shell prompt reached in {time.time()-t0:.1f}s")

        ser.sendall(f"export HTTPD_UPSTREAM=10.0.2.2:{args.proxy_port}\n"
                    .encode())
        wait_for(ser, b"$ ", 5.0, transcript)

        cmd = (f"proxytest --url {args.url} "
               f"--repeat {args.repeat} --timing\n")
        ser.sendall(cmd.encode())
        total_budget = max(args.budget_sec * args.repeat + 30.0, 120.0)
        ok, _ = wait_for(ser, b"[proxytest] done", total_budget, transcript)

        log = b"".join(transcript).decode("ascii", "replace")
        if args.save_log:
            with open(args.save_log, "w") as f: f.write(log)
            print(f"transcript saved to {args.save_log}")

        iters = parse_iterations(log)
        timings = parse_timings(log)
        tcp = parse_tcp_lines(log)

        # Group [timing] lines into iterations: each iteration's
        # browser run prints lines starting with "fetch" and
        # ending with "paint collect" / "re-layout w/ intrinsic
        # sizes".  We split on "fetch" boundaries.
        by_iter = []
        cur = []
        for name, ms in timings:
            if name == "fetch" and cur:
                by_iter.append(cur); cur = []
            cur.append((name, ms))
        if cur: by_iter.append(cur)

        print()
        print("== iterations ==")
        for r in iters:
            print(f"  iter {r['iter']}/{r['total']}: "
                  f"wall={r['wall_ms']} ms exit={r['exit']}")
        if by_iter:
            print()
            print("== per-iteration browser [timing] breakdown ==")
            for i, stages in enumerate(by_iter, 1):
                print(f"  iter {i}:")
                for name, ms in stages:
                    print(f"    {name:30s} {ms} ms")
        print()
        print("== tcp summary ==")
        print(f"  connects: {tcp['connects']}")
        print(f"  releases: {tcp['releases']}")
        print(f"  rejects:  {tcp['rejects']}")
        print(f"  rx_total per cid:")
        for cid, tot in sorted(tcp["max_rx_per_cid"].items()):
            print(f"    cid={cid}: {tot} bytes")

        verdict = True
        if not ok:
            print(f"\nFAIL: proxytest never printed 'done' "
                  f"(timed out after {total_budget:.0f}s)")
            verdict = False
        if "browser: response exceeded" in log:
            print("FAIL: browser tripped DRAIN_FD_MAX_BYTES cap")
            verdict = False
        if tcp["rejects"]:
            print(f"WARN: tcp rejected lines present: {tcp['rejects']}")
        for r in iters:
            if r["exit"] != 0:
                print(f"FAIL: iter {r['iter']} browser exit={r['exit']}")
                verdict = False
            if r["wall_ms"] > args.budget_sec * 1000:
                print(f"FAIL: iter {r['iter']} wall={r['wall_ms']} ms "
                      f"exceeds budget {args.budget_sec*1000:.0f} ms")
                verdict = False

        if verdict:
            print("\nVERDICT: PASS")
            rc = 0
        else:
            print("\nVERDICT: FAIL")
            rc = 1
        return rc

    finally:
        if not args.keep_running:
            try: qemu.terminate()
            except Exception: pass
            try: host_proxy.terminate()
            except Exception: pass
            try: qemu.wait(timeout=5)
            except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

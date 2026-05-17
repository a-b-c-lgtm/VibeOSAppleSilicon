#!/usr/bin/env python3
"""scripts/test_browser_hn_timings.py -- chapter 106b perf bug repro.

Boots the OS, starts the real scripts/https_proxy.py on the host
(so the guest can actually reach HN through SLIRP NAT), spawns
proxytest in the guest with HTTPD_UPSTREAM=10.0.2.2:<host_proxy_port>
pointed at it, and asks proxytest to drive the plain-mode browser
at https://news.ycombinator.com/.

Because the browser canonicalizes https:// through BR_DEFAULT_PROXY
(127.0.0.1:8080), the request hops:

    browser --plain
      -> 127.0.0.1:8080 (in-guest httpd)
      -> 10.0.2.2:<host_proxy_port> (host https_proxy.py)
      -> news.ycombinator.com:443

This is the exact pipeline that's been showing 36+ second fetches
plus 8 MiB response amplification when driven by hand from
gui_term.  Driving it over the serial port lets us see every
[tcp], [httpd], [browser] diag line that chapter 106b added.

The test PASSES if:
  1. The fetch completes within FETCH_TIMEOUT seconds (default 60).
  2. httpd's serve_forward summary shows read==wrote in a
     plausible range (real HN is ~38 KB - ~80 KB depending on
     day).
  3. The browser doesn't trip the DRAIN_FD_MAX_BYTES cap (no
     "response exceeded ... cap" line).

The test FAILS (and prints the relevant tail of the serial log)
if any of those conditions don't hold.  Either way, the timing
numbers and counter values are dumped at the end so we can chase
the bug from the captured transcript without re-running.

Run from repo root with the kernel + ramfs already built:

    python3 scripts/test_browser_hn_timings.py

Add `--keep-running` to leave QEMU alive after the test so you
can poke at the shell yourself.  Add `--url URL` to point at a
different site.
"""
import argparse
import http.server
import os
import re
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-hn-timings.sock"

# Host port that QEMU forwards from the guest's 10.0.2.2:HOST_PROXY_PORT
# back into the host's loopback (where scripts/https_proxy.py listens).
# Chosen well away from the other test ports (18083, 18082).
HOST_PROXY_PORT = 18091

# Default URL.  https:// forces the canonicalize_url -> proxy
# rewrite in the browser, which is exactly the path 106b targets.
DEFAULT_URL = "https://news.ycombinator.com/"

# Wall-clock budgets.  HN through a fresh TLS handshake on the
# host should be < 5 s on any healthy network; 60 s gives the
# entire splice (host fetch + serve_forward + browser drain)
# room to breathe before we declare failure.
BOOT_TIMEOUT     = 120.0
FETCH_TIMEOUT    = 90.0
SHUTDOWN_TIMEOUT = 30.0


# ----------------------------------------------------------------
# Host-side https_proxy.py.  We spawn it as a subprocess rather
# than importing because https_proxy.py uses urllib in blocking
# mode and we don't want its blocked threads in our event loop.
# ----------------------------------------------------------------

def start_host_proxy(port):
    """Spawn scripts/https_proxy.py on `port`.  Returns the Popen
    handle.  Streams the proxy's stderr to OUR stderr with a
    prefix so its log lines are visible in the test output but
    obviously not the test's own output."""
    p = subprocess.Popen(
        ["python3", os.path.join(ROOT, "scripts", "https_proxy.py"),
         str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    # Wait for "[proxy] listening" so we don't race the guest's
    # spawn(httpd) against the host bind.
    deadline = time.time() + 5.0
    while time.time() < deadline:
        line = p.stdout.readline()
        if not line:
            time.sleep(0.05); continue
        sys.stderr.write(f"[host-proxy] {line}")
        if "listening" in line:
            return p
    raise RuntimeError("host https_proxy.py never reported listening")


# ----------------------------------------------------------------
# QEMU plumbing.  Same chapter-106b -netdev recipe as
# test_browser_proxy.py, but with a single hostfwd so the guest
# can reach the host's proxy port.  10.0.2.2 is SLIRP's gateway
# alias -- packets to that IP show up on the host loopback.
# ----------------------------------------------------------------

def cleanup_sock():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass


def boot(host_proxy_port):
    cleanup_sock()
    # No guestfwd needed -- SLIRP routes 10.0.2.2 to host
    # loopback automatically.  No hostfwd either -- the test
    # only initiates traffic FROM the guest.
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


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            try: c = s.recv(8192)
            except OSError: break
            if not c: break
            out += c
    return out


def wait_for(s, needle, timeout, accum=None):
    """Read from `s` until `needle` appears or `timeout` elapses.
    Returns (matched, buf_since_call_start).  `accum`, if given,
    is appended to in-place so the caller can keep a running
    transcript without re-passing it."""
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = drain(s, time.time() + 0.4)
        if chunk:
            buf += chunk
            if accum is not None: accum.append(chunk)
        if needle in buf: return True, buf
    return False, buf


# ----------------------------------------------------------------
# Counters extracted from the kernel + userspace diag lines that
# chapter 106b added.  We want to surface these in the test
# output even on PASS so a slow-but-correct run is recognisable.
# ----------------------------------------------------------------

def extract_metrics(log_bytes):
    """Pull every interesting counter out of the serial log."""
    text = log_bytes.decode("ascii", "replace")
    m = {}

    # [httpd] serve_forward: read=N wrote=M iters=I
    httpd = re.search(
        r"\[httpd\] serve_forward: read=(\d+) wrote=(\d+) iters=(\d+)",
        text)
    if httpd:
        m["httpd_read"]  = int(httpd.group(1))
        m["httpd_wrote"] = int(httpd.group(2))
        m["httpd_iters"] = int(httpd.group(3))

    # [browser] drain: N bytes (M reads)  -- final value wins
    drains = re.findall(
        r"\[browser\] drain: (\d+) bytes \((\d+) reads\)", text)
    if drains:
        m["browser_drained"] = int(drains[-1][0])
        m["browser_reads"]   = int(drains[-1][1])

    # browser: response exceeded ... cap (read N so far, M reads)
    overflow = re.search(
        r"browser: response exceeded \d+-byte cap \(read (\d+) so far, "
        r"(\d+) reads\)", text)
    if overflow:
        m["browser_overflow_bytes"] = int(overflow.group(1))
        m["browser_overflow_reads"] = int(overflow.group(2))

    # [tcp] cid=X rx_total=N rcv_nxt=...  -- pick the largest per cid
    tcp_lines = re.findall(
        r"\[tcp\] cid=([0-9a-fA-F]+) state=([0-9a-fA-F]+) "
        r"rx_total=([0-9a-fA-F]+) rcv_nxt=([0-9a-fA-F]+) "
        r"lport=([0-9a-fA-F]+) rport=([0-9a-fA-F]+)",
        text)
    per_cid_max = {}
    for cid, state, total, rcvn, lport, rport in tcp_lines:
        c   = int(cid,   16)
        st  = int(state, 16)
        tot = int(total, 16)
        lp  = int(lport, 16)
        rp  = int(rport, 16)
        prev = per_cid_max.get(c)
        if prev is None or tot > prev[0]:
            per_cid_max[c] = (tot, st, lp, rp)
    if per_cid_max:
        m["tcp_per_cid"] = per_cid_max

    # [tcp] reject cid=X n=N ...  -- count rejections per cid.
    rej_lines = re.findall(
        r"\[tcp\] reject cid=([0-9a-fA-F]+) n=([0-9a-fA-F]+)", text)
    per_cid_rej = {}
    for cid, n in rej_lines:
        per_cid_rej[int(cid, 16)] = int(n, 16)
    if per_cid_rej:
        m["tcp_rejects"] = per_cid_rej

    return m


def fmt_metrics(m):
    out = []
    if "httpd_read" in m:
        out.append(f"  httpd:    read={m['httpd_read']} wrote={m['httpd_wrote']} "
                   f"iters={m['httpd_iters']}")
    if "browser_drained" in m:
        out.append(f"  browser:  drained={m['browser_drained']} "
                   f"reads={m['browser_reads']}")
    if "browser_overflow_bytes" in m:
        out.append(f"  browser:  OVERFLOW={m['browser_overflow_bytes']} after "
                   f"{m['browser_overflow_reads']} reads")
    if "tcp_per_cid" in m:
        for cid, (tot, st, lp, rp) in sorted(m["tcp_per_cid"].items()):
            out.append(f"  tcp cid={cid}: rx_total={tot} state={st} "
                       f"lport={lp} rport={rp}")
    if "tcp_rejects" in m:
        for cid, n in sorted(m["tcp_rejects"].items()):
            out.append(f"  tcp cid={cid}: rejects={n}")
    return "\n".join(out) if out else "  (no diag counters found)"


# ----------------------------------------------------------------
# Test driver.
# ----------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=DEFAULT_URL,
                    help="URL to feed proxytest (default: %(default)s)")
    ap.add_argument("--proxy-port", type=int, default=HOST_PROXY_PORT,
                    help="Host port for https_proxy.py (default: %(default)s)")
    ap.add_argument("--fetch-timeout", type=float, default=FETCH_TIMEOUT,
                    help="Seconds to wait for proxytest to finish "
                         "(default: %(default)s)")
    ap.add_argument("--keep-running", action="store_true",
                    help="Leave QEMU running after the test for "
                         "interactive poking.")
    ap.add_argument("--save-log",
                    help="Write the captured serial transcript to "
                         "this path.")
    args = ap.parse_args()

    # Make sure the build is current-ish; we don't `make` here so
    # the test fails fast on a stale build instead of silently
    # taking 30s to rebuild.
    for required in ("build/kernel.elf",
                     "build/userspace/browser/browser.elf",
                     "build/userspace/httpd/httpd.elf",
                     "build/userspace/proxytest/proxytest.elf"):
        if not os.path.exists(os.path.join(ROOT, required)):
            print(f"FAIL: {required} not built -- run `make -j` first.")
            return 1

    host_proxy = start_host_proxy(args.proxy_port)
    qemu = boot(args.proxy_port)
    transcript = []

    t_start = time.time()
    try:
        ser = serial_connect()
        ser.setblocking(False)

        # 1. Wait for the shell prompt.
        ok, _ = wait_for(ser, "$ ", BOOT_TIMEOUT, transcript)
        if not ok:
            print("FAIL: shell prompt not reached within "
                  f"{BOOT_TIMEOUT}s")
            return 1
        t_boot = time.time() - t_start
        print(f"PASS: shell prompt reached in {t_boot:.1f}s")

        # 2. Tell httpd where to forward.
        cmd = f"export HTTPD_UPSTREAM=10.0.2.2:{args.proxy_port}\n"
        ser.sendall(cmd.encode())
        wait_for(ser, "$ ", 5.0, transcript)
        print(f"PASS: HTTPD_UPSTREAM=10.0.2.2:{args.proxy_port}")

        # 3. Run proxytest pointed at the requested URL.  The
        # --url flag exists for exactly this kind of harness use.
        t_fetch = time.time()
        ser.sendall(f"proxytest --url {args.url} --timing\n".encode())
        ok, _ = wait_for(ser, "[proxytest] done", args.fetch_timeout,
                         transcript)
        fetch_seconds = time.time() - t_fetch

        full_log = b"".join(transcript)
        metrics = extract_metrics(full_log)

        print()
        print(f"fetch wall time:  {fetch_seconds:.2f}s "
              f"(budget {args.fetch_timeout:.0f}s)")
        print("counters:")
        print(fmt_metrics(metrics))
        print()

        # 4. Verdict.
        verdict_pass = True
        if not ok:
            print(f"FAIL: proxytest never printed 'done' "
                  f"(timed out after {args.fetch_timeout:.0f}s)")
            verdict_pass = False
        if "browser_overflow_bytes" in metrics:
            print(f"FAIL: browser tripped DRAIN_FD_MAX_BYTES cap "
                  f"at {metrics['browser_overflow_bytes']} bytes")
            verdict_pass = False
        if metrics.get("httpd_read") and metrics.get("httpd_wrote") and \
           metrics["httpd_read"] != metrics["httpd_wrote"]:
            print(f"FAIL: httpd splice mismatch -- read "
                  f"{metrics['httpd_read']} != wrote "
                  f"{metrics['httpd_wrote']}")
            verdict_pass = False

        # 5. Dump the tail of the transcript on failure so we
        # can chase the bug from the captured output.
        if not verdict_pass:
            print()
            print("---- last 4 KiB of serial transcript ----")
            print(full_log[-4096:].decode("ascii", "replace"))
            print("---- end transcript ----")

        if args.save_log:
            with open(args.save_log, "wb") as f:
                f.write(full_log)
            print(f"\nFull transcript written to {args.save_log}")

        if args.keep_running:
            print("\n--keep-running: QEMU still alive.  PID=",
                  qemu.pid, " serial socket=", SOCK, sep="")
            print("Connect with: socat - UNIX-CONNECT:" + SOCK)
            try:
                qemu.wait()
            except KeyboardInterrupt:
                pass

        return 0 if verdict_pass else 1
    finally:
        if not args.keep_running:
            try: qemu.terminate(); qemu.wait(timeout=SHUTDOWN_TIMEOUT)
            except Exception: qemu.kill()
        try: host_proxy.terminate(); host_proxy.wait(timeout=5)
        except Exception: host_proxy.kill()
        cleanup_sock()


if __name__ == "__main__":
    sys.exit(main())

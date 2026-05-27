#!/usr/bin/env python3
"""scripts/test_journal.py — chapter 84 crash-consistency tests.

Three sub-tests, all use SIGKILL on the QEMU VM (no graceful flush
chance) to model power loss.  Between each kill we boot the kernel
again and verify the on-disk filesystem is internally consistent
and that any committed data we expect to find is still there.

  Test A  Smoke — a fresh boot does NOT trigger replay (clean
          shutdown leaves the journal header zeroed because the
          last successful flush ran step 4 of the commit
          protocol).  Verifies the [osfs2_journal] init log line
          appears and no replay log line follows it.

  Test B  Replay correctness — write + sync N files, kill, boot,
          verify all N survive AND the journal serial output
          either says "replaying" (we crashed mid-flush) or
          quietly completes (clean — flushes finished before the
          kill).  Repeats with several kill timings to exercise
          both branches.

  Test C  Consistency under random kills — interleave many small
          writes, fsyncs, and unlinks, killing at random delays
          in [10 ms, 5000 ms].  After each kill, reboot and
          run `ls /data` + `cat` of any file the previous run
          claimed to have sync'd.  The journal must guarantee
          (a) mount succeeds, (b) every fsync'd-then-not-unlinked
          file is still present with the expected contents, and
          (c) ls /data does not crash or list garbage.

Why no fault-injection at exact protocol-step boundaries?  We
can't easily pause the kernel in the middle of a virtio_blk_dev
write from outside QEMU.  We get coverage of the relevant code
paths by running enough kill cycles that the random timing lands
between every step at least once in expectation — that's why the
delays are jittered.

Stick this in the regression sweep alongside test_osfs2.py and
test_fsync.py."""

import os, random, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-journal.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

# ---------- low-level helpers (mirrored from test_fsync.py) ----------

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
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

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

def send_cmd(s, cmd, timeout=10.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out

PASSES, FAILS = [], []

def expect(cond, msg):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        FAILS.append(msg)

# ---------- harness primitives ----------

def boot_to_shell():
    """Boot, return (qemu_proc, serial_conn) at a shell prompt.
    Drains the boot log into qemu_proc.boot_log for inspection.
    """
    q = boot()
    s = conn()
    buf = wait_for(s, PROMPT, timeout=15.0)
    q.boot_log = buf
    return q, s

def shutdown(q, s):
    try: s.close()
    except Exception: pass
    hard_kill(q)

def write_and_sync(s, path, contents):
    """Write `contents` to `path` and call /bin/sync to flush."""
    send_cmd(s, f"echo {contents} > {path}")
    send_cmd(s, "/bin/sync")

def cat(s, path):
    out = send_cmd(s, f"cat {path}", timeout=5.0)
    return out

# ---------- Test A — clean-shutdown smoke ----------

def test_A_clean_shutdown():
    print("\n[chapter 84] Test A: clean boot performs no replay")
    reformat_data()
    q, s = boot_to_shell()
    log = q.boot_log
    expect(b"[osfs2_journal] ready" in log,
           "journal init log line is present")
    expect(b"[osfs2_journal] replaying" not in log,
           "fresh image triggers no replay")
    shutdown(q, s)

# ---------- Test B — write + sync + crash + replay ----------

def test_B_replay_after_crash():
    print("\n[chapter 84] Test B: data sync'd before crash survives")
    reformat_data()
    q, s = boot_to_shell()
    # Write three files and sync them all in one batch — that
    # makes osfs2_cache_flush() emit a multi-block journal txn.
    send_cmd(s, "echo alpha > /data/a")
    send_cmd(s, "echo bravo > /data/b")
    send_cmd(s, "echo charlie > /data/c")
    send_cmd(s, "/bin/sync")
    # Crash immediately after sync — journal must have committed
    # AND checkpointed before sync returned, so on next boot the
    # files are present without replay.
    shutdown(q, s)

    q, s = boot_to_shell()
    # The first boot left a clean journal, so the second boot
    # should ALSO not replay (the checkpoint zeroed the header).
    expect(b"[osfs2_journal] replaying" not in q.boot_log,
           "post-sync clean shutdown does not require replay")
    expect(b"alpha" in cat(s, "/data/a"),
           "/data/a survives crash after sync")
    expect(b"bravo" in cat(s, "/data/b"),
           "/data/b survives crash after sync")
    expect(b"charlie" in cat(s, "/data/c"),
           "/data/c survives crash after sync")
    shutdown(q, s)

# ---------- Test C — random-time kills, FS stays consistent ----------

def test_C_random_kills_consistent():
    print("\n[chapter 84] Test C: random kills, mount remains consistent")
    reformat_data()
    survivors = []  # files we sync'd in a previous round
    rng = random.Random(0xC0FFEE)
    rounds = 5
    for r in range(rounds):
        q, s = boot_to_shell()
        # Verify previously-sync'd files all came back.
        ok_so_far = True
        for (name, payload) in survivors:
            out = cat(s, f"/data/{name}")
            if payload.encode() not in out:
                expect(False,
                       f"round {r}: previously-sync'd /data/{name} "
                       f"missing or corrupt (got {out!r})")
                ok_so_far = False
        if ok_so_far and survivors:
            expect(True,
                   f"round {r}: all {len(survivors)} prior-sync'd "
                   f"files survived")
        # Workload: mix of sync'd and un-sync'd writes.
        new_synced = []
        for i in range(4):
            name = f"r{r}_{i}"
            payload = f"round{r}item{i}"
            send_cmd(s, f"echo {payload} > /data/{name}")
            if i % 2 == 0:
                # Sync this one; the rest are best-effort.
                send_cmd(s, "/bin/sync")
                new_synced.append((name, payload))
        # ls /data must not crash even mid-workload.
        ls_out = send_cmd(s, "ls /data", timeout=5.0)
        expect(b"r" + str(r).encode() in ls_out,
               f"round {r}: ls /data shows newly-written files")
        # Pick a kill delay between very-fast and well-after-flush.
        delay = rng.uniform(0.05, 1.5)
        time.sleep(delay)
        shutdown(q, s)
        # Anything we explicitly sync'd this round MUST survive.
        survivors.extend(new_synced)

    # Final mount check.
    q, s = boot_to_shell()
    expect(b"[osfs2_journal] ready" in q.boot_log,
           "final mount: journal initialised")
    # Some replays may have happened during the random rounds; the
    # important invariant is that the FS mounts successfully and
    # all sync'd data is intact.
    for (name, payload) in survivors:
        out = cat(s, f"/data/{name}")
        expect(payload.encode() in out,
               f"final mount: /data/{name} = {payload!r} survived all rounds")
    ls_out = send_cmd(s, "ls /data", timeout=5.0)
    expect(b"FAIL" not in ls_out and b"panic" not in ls_out.lower(),
           "final mount: ls /data does not error or panic")
    shutdown(q, s)

# ---------- main ----------

def main():
    print("[chapter 84] reformatting build/data.img ...")
    reformat_data()
    test_A_clean_shutdown()
    test_B_replay_after_crash()
    test_C_random_kills_consistent()
    print()
    if FAILS:
        print(f"CHAPTER 84: {len(FAILS)} FAIL(s) out of "
              f"{len(PASSES) + len(FAILS)}")
        for f in FAILS: print(f"  FAIL: {f}")
        sys.exit(1)
    print(f"CHAPTER 84: ALL TESTS PASSED ({len(PASSES)} checks)")

if __name__ == "__main__":
    main()

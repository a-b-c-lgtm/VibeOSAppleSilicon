#!/usr/bin/env python3
# scripts/test_doom_full.py
# ─────────────────────────────────────────────────────────────────────────────
# Chapter 194 — in-guest Doom full vendor compile + link.
#
# Boots the OS, extracts /bin/doomgeneric.tar onto /data, runs
# /bin/make -f /bin/doom_full.mk (compiles 81 vendor sources +
# dummy.o), spot-checks a representative subset of .o files,
# then runs /bin/make -f /bin/doom_link.mk to prove the .o set
# is actually linkable into /data/doomgeneric.elf.
#
# The link step catches drift the compile alone cannot:
#  - missing -D flags (e.g. -DOSDEV_LIBC_NO_GLOBAL_DEFS) that
#    leave every TU emitting strong-global duplicates,
#  - missing OBJS entries (e.g. gusconf.o / icon.o) that the
#    host build and doom_link.args expect.
#
# Expected runtime: ~25 min (compile dominates; link is ~15 s).
# ─────────────────────────────────────────────────────────────────────────────
import os, sys, time, socket, subprocess, signal, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SERIAL_SOCK = "/tmp/osdev-doom-full.sock"
DISK_IMG    = f"{ROOT}/build/disk.img"
DATA_IMG    = f"{ROOT}/build/data.img"

QEMU = [
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
    "-m", "8G", "-smp", "2", "-display", "none",
    "-serial", f"unix:{SERIAL_SOCK},server,nowait",
    "-global", "virtio-mmio.force-legacy=off",
    "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-drive", f"if=none,file={DISK_IMG},format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive", f"if=none,file={DATA_IMG},format=raw,id=hd1",
    "-device", "virtio-blk-device,drive=hd1",
    "-kernel", f"{ROOT}/build/kernel.elf",
]

PASS, FAIL = 0, 0
def expect(cond, label):
    global PASS, FAIL
    if cond:
        print(f"PASS: {label}");  PASS += 1
    else:
        print(f"FAIL: {label}");  FAIL += 1

def reformat_data():
    subprocess.check_call(
        [sys.executable, f"{ROOT}/scripts/mkosfs2.py", DATA_IMG],
        cwd=ROOT)

def send_cmd(sock, cmd, timeout=10.0, idle=1.5):
    """Send a command, return everything received until idle."""
    sock.sendall((cmd + "\n").encode())
    out = b""
    deadline = time.time() + timeout
    last = time.time()
    sock.settimeout(0.3)
    while time.time() < deadline:
        try:
            chunk = sock.recv(8192)
            if chunk:
                out += chunk
                last = time.time()
            else:
                break
        except socket.timeout:
            if time.time() - last >= idle:
                break
    return out

def wait_for_prompt(sock, deadline_seconds=60.0):
    out = b""
    deadline = time.time() + deadline_seconds
    sock.settimeout(1.0)
    while time.time() < deadline:
        try:
            chunk = sock.recv(8192)
            if chunk:
                out += chunk
                if b"/$ " in out or b"$ " in out:
                    return out
        except socket.timeout:
            pass
    return out

def main():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass

    reformat_data()

    qemu = subprocess.Popen(QEMU, cwd=ROOT,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)

    # Wait for the serial socket to appear and accept connections.
    sock = None
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(SERIAL_SOCK)
                break
            except OSError:
                sock = None
                time.sleep(0.1)
        else:
            time.sleep(0.1)
    if sock is None:
        try: os.killpg(qemu.pid, signal.SIGKILL)
        except Exception: pass
        raise RuntimeError("could not connect to QEMU serial socket")

    try:
        boot = wait_for_prompt(sock, 90.0)
        expect(b"/$ " in boot or b"$ " in boot,
               "boot: reached shell prompt")

        # --- step 1: fixture sanity ---
        out = send_cmd(sock, "/bin/ls /bin/doom_full.mk", timeout=10)
        expect(b"doom_full.mk" in out,
               "step 1a: /bin/doom_full.mk shipped on OSFS-1")

        out = send_cmd(sock, "/bin/ls /bin/doomgeneric.tar", timeout=10)
        expect(b"doomgeneric.tar" in out,
               "step 1b: /bin/doomgeneric.tar shipped on OSFS-1")

        # --- step 2: extract ---
        out = send_cmd(sock,
            "/bin/tar xf /bin/doomgeneric.tar -C /data",
            timeout=60.0)
        expect(b"cannot create" not in out and b"errno=" not in out,
               "step 2a: /bin/tar extracted without errors")

        # quick sanity: doomgeneric.c is present
        out = send_cmd(sock, "/bin/ls /data/src/doomgeneric.c",
                       timeout=10)
        expect(b"doomgeneric.c" in out,
               "step 2b: /data/src/ populated by tar")

        # --- step 3: run the full Makefile ---
        # 77 invocations of /bin/gcc, each runs cc1+as+xgcc; expect
        # ~15-20 min wall time.  Use a long idle to tolerate slow
        # cc1 runs.
        print("--- launching /bin/make -f /bin/doom_full.mk ---")
        print("    (this can take ~20 min; output streams below)")
        sock.sendall(b"/bin/make -f /bin/doom_full.mk\n")
        out = b""
        deadline = time.time() + 1800.0  # 30 min hard cap
        last_byte = time.time()
        sock.settimeout(0.5)
        while time.time() < deadline:
            try:
                chunk = sock.recv(8192)
                if chunk:
                    out += chunk
                    last_byte = time.time()
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.flush()
                    # done?
                    if b"make: built 'all'" in out:
                        break
                    # /bin/make's own failure messages are always
                    # prefixed `make:` and end with a non-zero
                    # code; the bare substring "exited with code"
                    # ALSO appears in the kernel's harmless
                    # `[sys_exit] thread '/bin/cc1' exited with
                    # code 0x0000000000000000` log line, so we
                    # must anchor on the `make:` prefix here.
                    if (b"make: recipe for" in out
                            or b"make: spawn" in out
                            or b"make: waitpid" in out):
                        break
                else:
                    break
            except socket.timeout:
                if time.time() - last_byte > 90.0:
                    # 90 sec without any output suggests a hang
                    print("\n--- 90s idle; bailing ---")
                    break

        expect(b"make: built 'all'" in out,
               "step 3a: /bin/make completed full Doom vendor compile")
        # Real compile/link failures look like one of:
        #   - `fatal error: <hdr>: No such file or directory`
        #   - `error: <something>` (cc1 diagnostics)
        #   - `implicit declaration of function 'X'`
        #   - `undefined reference to 'X'` (would be from /bin/ld)
        #   - `make: recipe for 'X' exited with code <N>`
        # The bare substring `exited with code` is NOT a useful
        # signal because the kernel logs `[sys_exit] thread
        # '/bin/cc1' exited with code 0x0000000000000000` after
        # every successful subprocess.
        expect(b"fatal error" not in out
               and b"undefined reference" not in out
               and b"implicit declaration" not in out
               and b": error:" not in out
               and b"make: recipe for" not in out
               and b"make: spawn" not in out
               and b"make: waitpid" not in out,
               "step 3b: no compile/link errors during full build")

        # --- step 4: spot-check .o files exist ---
        # The list intentionally includes gusconf.o and icon.o:
        # those two were silently absent from doom_full.mk's OBJS
        # for the entire 133d-195 run, so the in-guest compile
        # left /data/src/ with only 80 of the 82 .o files the
        # host build (and doom_link.args) expects.  Spot-checking
        # them here turns that drift into a step-4 FAIL.
        targets = [
            b"m_random.o", b"m_bbox.o", b"m_fixed.o",
            b"am_map.o", b"d_main.o", b"p_setup.o",
            b"r_main.o", b"r_draw.o", b"z_zone.o",
            b"doomgeneric.o", b"w_wad.o",
            b"gusconf.o", b"icon.o",
        ]
        found = 0
        for t in targets:
            out = send_cmd(sock, "/bin/ls /data/src/" + t.decode(),
                           timeout=10)
            if t in out and b"no such" not in out.lower():
                found += 1
        expect(found == len(targets),
               f"step 4: spot-check .o files exist ({found}/{len(targets)})")

        # --- step 5: link the compiled objects into doomgeneric.elf ---
        # The whole point of doom_full.mk is to produce a .o set
        # that /bin/make -f /bin/doom_link.mk can link.  Running
        # the link here catches drift the compile alone can't:
        # missing -D flags (e.g. -DOSDEV_LIBC_NO_GLOBAL_DEFS) that
        # leave each TU emitting its own copy of strong globals
        # like `__cxa_finalize` / `environ`, missing source files
        # that show up as `cannot find /data/src/X.o`, etc.
        out = send_cmd(sock, "/bin/ls /bin/doom_link.mk", timeout=10)
        expect(b"doom_link.mk" in out,
               "step 5a: /bin/doom_link.mk shipped on OSFS-1")

        print("--- launching /bin/make -f /bin/doom_link.mk ---")
        sock.sendall(b"/bin/make -f /bin/doom_link.mk\n")
        link_out = b""
        deadline = time.time() + 600.0  # 10 min hard cap
        last_byte = time.time()
        sock.settimeout(0.5)
        while time.time() < deadline:
            try:
                chunk = sock.recv(8192)
                if chunk:
                    link_out += chunk
                    last_byte = time.time()
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.flush()
                    if b"make: built 'all'" in link_out:
                        break
                    if (b"make: recipe for" in link_out
                            or b"make: spawn" in link_out
                            or b"make: waitpid" in link_out):
                        break
                else:
                    break
            except socket.timeout:
                if time.time() - last_byte > 120.0:
                    print("\n--- 120s idle; bailing on link ---")
                    break

        expect(b"make: built 'all'" in link_out,
               "step 5b: /bin/make completed link rule")

        # Anchor failure detection on /bin/ld's `ld:` prefix.
        # The benign "ld: warning: ... has a LOAD segment with
        # RWX permissions" is filtered by line so a real ld error
        # (`ld: undefined reference`, `ld: cannot find ...`,
        # `ld: multiple definition of ...`) still trips this.
        bad_ld = False
        for line in link_out.splitlines():
            if not line.startswith(b"/bin/ld:") and not line.startswith(b"ld:"):
                continue
            tail = line.split(b"ld:", 1)[1].lstrip()
            if tail.startswith(b"warning"):
                continue
            bad_ld = True
            break
        expect(not bad_ld
               and b"undefined reference" not in link_out
               and b"multiple definition" not in link_out
               and b"cannot find" not in link_out
               and b"make: recipe for" not in link_out
               and b"make: spawn" not in link_out
               and b"make: waitpid" not in link_out,
               "step 5c: no link errors (multi-def, undef ref, missing .o)")

        out = send_cmd(sock, "/bin/ls /data/doomgeneric.elf",
                       timeout=10)
        expect(b"doomgeneric.elf" in out and b"no such" not in out.lower(),
               "step 5d: /data/doomgeneric.elf produced")

        # Size sanity: a real doomgeneric.elf is ~2.4 MiB
        # (chapter 195 baseline: 2,502,904 bytes).  Anything
        # under ~500 KB indicates the link silently dropped TUs;
        # anything over ~50 MB indicates a runaway.
        out = send_cmd(sock, "/bin/wc /data/doomgeneric.elf",
                       timeout=30, idle=3.0)
        size_ok = False
        last_n = None
        for line in out.splitlines():
            s = line.decode(errors="replace").strip()
            parts = s.split()
            if (len(parts) >= 4
                    and parts[0].isdigit() and parts[1].isdigit()
                    and parts[2].isdigit()
                    and parts[3].endswith(".elf")):
                n = int(parts[2])
                last_n = n
                if 500_000 <= n <= 50_000_000:
                    size_ok = True
                    print(f"    (doomgeneric.elf = {n} bytes)")
                    break
        if not size_ok:
            print(f"    (wc raw output: {out[-200:]!r}; last_n={last_n})")
        expect(size_ok,
               "step 5e: /data/doomgeneric.elf size is plausible")

    finally:
        try:
            sock.close()
            os.killpg(qemu.pid, signal.SIGTERM)
            qemu.wait(timeout=5)
        except Exception:
            try: os.killpg(qemu.pid, signal.SIGKILL)
            except Exception: pass

    print()
    print(f"PASS: {PASS}")
    print(f"FAIL: {FAIL}")
    sys.exit(0 if FAIL == 0 else 1)

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""scripts/test_gcc_hello.py -- chapter 132f /bin/gcc smoke.

End-to-end: boot the OS, ask `/bin/gcc` to compile a trivial
hello-world C program, run the resulting binary, assert the
expected exit code.

This is the payoff for the entire chapter-132 arc (gcc cross-
build): the toy chapter-121 `/bin/cc` walked a hand-written
subset of C; `/bin/gcc` is real GCC 14.2 (43 MB cc1) running
inside our OS for the first time.

The shim at `/bin/gcc` is `userspace/gccw/gccw.c` -- it
prepends `-B/bin/` to argv and execs `/bin/xgcc`.  xgcc then
spawns cc1 / as / ld as needed, all of which live at `/bin/`
on the OSFS-1 image after chapter 132f.

Test ladder (so failures pinpoint where the pipeline breaks):

  1. `/bin/gcc --version`           -- xgcc loads at all
  2. `/bin/gcc -E ...`              -- cc1 loads + preprocesses
  3. `/bin/gcc -S ...`              -- cc1 emits .s
  4. `/bin/gcc -c ... -o foo.o`     -- cc1 + as
  5. `/bin/gcc ... -o foo`          -- cc1 + as + ld
  6. `/tmp/foo`                     -- runtime exit code 42

Step 5 uses `-nostdlib -nostdinc -e _start` so we don't depend
on default specs picking up crt0 / libc / libgcc -- those are
shipped on OSFS-1 but the wrapper paths will need follow-up
work (chapter 132g) before the bare `gcc hello.c` invocation
in the user memory ("`gcc hello.c` working in the guest")
matches a hosted-style command line.
"""

import os
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-gcc-hello.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

# Minimal C program that doesn't need libc / crt0:
#   _start sets x0=42 (exit code), x8=2 (SYS_EXIT), then svc.
# Compiled with -nostdlib -nostdinc -e _start so xgcc skips
# default specs (no crti/crtbegin/crt0/-lc/-lgcc).
HELLO_C = r"""void _start(void) {
    register long x0 asm("x0") = 42;
    register long x8 asm("x8") = 2;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x8));
    __builtin_unreachable();
}
"""


def cleanup_sock():
    try:
        os.unlink(SERIAL_SOCK)
    except FileNotFoundError:
        pass


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
    raise RuntimeError("no serial socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(65536)
            if not c:
                break
            out += c
        elif out:
            break
    return out


def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def send_cmd(s, cmd, timeout=60.0):
    """Send a shell command; collect output up to the next $ prompt."""
    if isinstance(cmd, str):
        cmd = cmd.encode()
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


def main():
    print("[chapter 132f] /bin/gcc end-to-end smoke test")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        # --- step 1: does xgcc itself load? -----------------
        out = send_cmd(s, "/bin/gcc --version", timeout=60.0)
        sys.stdout.write("--- gcc --version ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------\n")
        expect(b"gcc" in out.lower() or b"GCC" in out,
               "step 1: /bin/gcc --version executes")
        expect(b"14." in out,
               "step 1: reports GCC 14.x")

        if FAILS:
            # If --version doesn't even work, later steps will
            # only produce noisier failures; stop early.
            print("\nStep 1 failed; skipping later steps.")
            return _report()

        # --- step 2: stage hello.c -------------------------
        send_cmd(s, "rm /tmp/hello.c", timeout=10.0)
        for line in HELLO_C.strip("\n").split("\n"):
            # Single-quote heredoc-style: shell echo passes the
            # literal string.  All chars in HELLO_C are safe for
            # single-quoting (no embedded `'`).
            send_cmd(s, f"echo '{line}' >> /tmp/hello.c",
                     timeout=10.0)
        out = send_cmd(s, "cat /tmp/hello.c", timeout=10.0)
        expect(b"_start" in out and b"svc #0" in out,
               "step 2: /tmp/hello.c contents staged")

        # --- step 3: preprocess only -----------------------
        # The guest shell does not support `2>&1`; use plain `>`
        # for stdout-only redirection and let stderr fall through.
        out = send_cmd(s,
                       "/bin/gcc -nostdinc -E /tmp/hello.c "
                       "> /tmp/hello.i",
                       timeout=120.0)
        sys.stdout.write("--- gcc -E stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------\n")
        out2 = send_cmd(s, "cat /tmp/hello.i", timeout=20.0)
        expect(b"_start" in out2,
               "step 3: cc1 preprocesses (output retains _start)")

        # --- step 4: compile to .s -------------------------
        out = send_cmd(s,
                       "/bin/gcc -nostdinc -S "
                       "-o /tmp/hello.s /tmp/hello.c",
                       timeout=120.0)
        sys.stdout.write("--- gcc -S stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------\n")
        out2 = send_cmd(s, "cat /tmp/hello.s", timeout=20.0)
        expect(b"svc" in out2 or b"_start" in out2,
               "step 4: cc1 emitted assembly")

        # --- step 5: compile to .o -------------------------
        out = send_cmd(s,
                       "/bin/gcc -nostdinc -c "
                       "-o /tmp/hello.o /tmp/hello.c",
                       timeout=120.0)
        sys.stdout.write("--- gcc -c stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------\n")
        out2 = send_cmd(s, "cat /tmp/hello.o", timeout=20.0)
        expect(b"\x7FELF" in out2,
               "step 5: cc1 + as produced ELF .o")

        # --- step 6: compile + link ------------------------
        out = send_cmd(s,
                       "/bin/gcc -nostdlib -nostdinc "
                       "-e _start "
                       "-Wl,-T,/bin/osdev.ld "
                       "-o /tmp/hello /tmp/hello.c",
                       timeout=180.0)
        sys.stdout.write("--- gcc full link stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n----------------------------\n")
        out2 = send_cmd(s, "cat /tmp/hello", timeout=20.0)
        expect(b"\x7FELF" in out2,
               "step 6: linked /tmp/hello is an ELF")

        # --- step 7: run ----------------------------------
        # Guest shell does not split on `;`; run the binary and
        # the exit-status probe as separate commands.
        out = send_cmd(s, "/tmp/hello", timeout=20.0)
        out += send_cmd(s, "echo exit=$?", timeout=10.0)
        sys.stdout.write("--- /tmp/hello run ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n----------------------\n")
        # The kernel reports exited code 0x2a (42) in its
        # process-reaper line; the shell also surfaces exit=42.
        expect(b"exit=42" in out or b"0x2a" in out.lower()
               or b"0x000000000000002a" in out,
               "step 7: /tmp/hello returned 42")

        # --- step 8 (chapter 132g): default-specs hello ----
        # The real prize: `gcc hello.c -o hello` with NO
        # `-nostdlib -nostdinc -e _start` escape hatch.  The
        # specs in aarch64-osdev.h already wire crt0%O%s,
        # `-T /bin/linker_user.ld`, and `-losdevc` by default,
        # and the /bin/gcc shim prepends `-B/bin/` so the
        # startfile-prefix list picks up /bin/crt0.o and
        # /bin/libosdevc.a.  If this lights up green the
        # toolchain is ready for real upstream programs.
        send_cmd(s, "rm /tmp/hello2.c", timeout=10.0)
        send_cmd(s, "echo 'int main(void) { return 7; }' "
                    "> /tmp/hello2.c",
                 timeout=10.0)
        out = send_cmd(s,
                       "/bin/gcc /tmp/hello2.c -o /tmp/hello2",
                       timeout=180.0)
        sys.stdout.write("--- gcc default-link stderr ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n--------------------------------\n")
        out2 = send_cmd(s, "cat /tmp/hello2", timeout=20.0)
        expect(b"\x7FELF" in out2,
               "step 8: default-spec link produces ELF")
        out = send_cmd(s, "/tmp/hello2", timeout=20.0)
        out += send_cmd(s, "echo exit=$?", timeout=10.0)
        sys.stdout.write("--- /tmp/hello2 run ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n-----------------------\n")
        expect(b"exit=7" in out or b"0x07" in out
               or b"0x0000000000000007" in out,
               "step 8: default-spec /tmp/hello2 returned 7")

    finally:
        hard_kill(q)

    return _report()


def _report():
    print()
    print(f"PASS: {len(PASSES)}")
    print(f"FAIL: {len(FAILS)}")
    if FAILS:
        for f in FAILS:
            print(f"  - {f}")
        sys.exit(1)


if __name__ == "__main__":
    main()

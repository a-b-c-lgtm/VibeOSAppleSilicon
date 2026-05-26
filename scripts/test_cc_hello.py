#!/usr/bin/env python3
"""
scripts/test_cc_hello.py — chapter 121 smoke test.

End-to-end pipeline for /bin/cc:

  1. Boot the OS.
  2. Stage a tiny C source at /tmp/hello.c via `echo >>`.
  3. Run /bin/cc /tmp/hello.c -o /tmp/hello
     (which internally drives /bin/as and /bin/ld).
  4. Run /tmp/hello — must print "hello, osdev\\n".
  5. Also verify -S mode (stop after asm emit).
  6. Also verify a return-N program exits with N.
"""
import os, select, signal, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_IMG = f"{ROOT}/build/data.img"
SERIAL_SOCK = "/tmp/osdev-cc.sock"
PROMPT = b"/$ "

HELLO_C = r"""int main(void) {
    printf("hello, osdev\n");
    return 0;
}
"""

EXIT_C = r"""int main() {
    puts("about to exit");
    return 42;
}
"""


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
        q.send_signal(signal.SIGKILL); q.wait(timeout=3)
    except Exception: pass
    cleanup_sock()


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SERIAL_SOCK); return s
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
        elif out: break
    return out


def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def send_cmd(s, cmd, timeout=15.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0: out = out[idx + len(cmd):]
    return out


def stage_source(s, src_text, path):
    """Stage source via echo lines (the same trick test_bin_ld_ar uses)."""
    send_cmd(s, f"rm {path} 2>/dev/null; true")
    for line in src_text.strip("\n").split("\n"):
        # Single-quote the line so the shell doesn't expand anything.
        # Lines in our snippets contain no single quotes, so this is safe.
        send_cmd(s, f"echo '{line}' >> {path}")


PASSES, FAILS = [], []


def expect_pass(cond, msg):
    if cond: print(f"PASS: {msg}"); PASSES.append(msg)
    else: print(f"FAIL: {msg}"); FAILS.append(msg)


def main():
    print("[chapter 121] /bin/cc smoke test")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)

        # ── Test 1: compile and run hello world. ────────────
        stage_source(s, HELLO_C, "/tmp/hello.c")
        out = send_cmd(s, "cat /tmp/hello.c", timeout=10.0)
        expect_pass(b'printf("hello, osdev' in out
                    or b"printf(\"hello, osdev" in out,
                    "hello.c staged correctly")

        out = send_cmd(s, "/bin/cc /tmp/hello.c -o /tmp/hello",
                       timeout=30.0)
        print("--- /bin/cc output ---")
        try: print(out.decode("utf-8", errors="replace"))
        except Exception: print(repr(out))
        print("----------------------")
        expect_pass(b"cc: wrote /tmp/hello" in out,
                    "/bin/cc reported success")
        expect_pass(b"cc: emitted" in out and b".cc.s" in out,
                    "/bin/cc emitted intermediate .cc.s")

        # Verify the .o was produced and is a real ELF relocatable.
        out = send_cmd(s, "cat /tmp/hello.cc.o", timeout=10.0)
        expect_pass(b"\x7FELF" in out,
                    "intermediate .o starts with ELF magic")

        # Verify the linked binary is ELF too.
        out = send_cmd(s, "cat /tmp/hello", timeout=10.0)
        expect_pass(b"\x7FELF" in out,
                    "linked /tmp/hello starts with ELF magic")

        # Run it; expect "hello, osdev" on stdout and exit 0.
        out = send_cmd(s, "/tmp/hello", timeout=20.0)
        print("--- /tmp/hello output ---")
        try: print(out.decode("utf-8", errors="replace"))
        except Exception: print(repr(out))
        print("-------------------------")
        expect_pass(b"hello, osdev" in out,
                    "/tmp/hello printed 'hello, osdev'")
        expect_pass(b"exited with code 0x0000000000000000" in out
                    or b"exit 0" in out,
                    "/tmp/hello exited with code 0")

        # ── Test 2: -S mode (stop after asm emit). ──────────
        out = send_cmd(s, "/bin/cc -S /tmp/hello.c -o /tmp/hello.s",
                       timeout=15.0)
        expect_pass(b"cc: emitted /tmp/hello.s" in out,
                    "/bin/cc -S emitted /tmp/hello.s")
        out = send_cmd(s, "cat /tmp/hello.s", timeout=10.0)
        expect_pass(b".global _user_start" in out,
                    "asm output declares _user_start")
        expect_pass(b'.ascii "hello, osdev\\n"' in out,
                    "asm output contains escaped ascii literal")
        expect_pass(b"mov  x8, #1" in out,
                    "asm output uses SYS_WRITE")
        expect_pass(b"mov  x8, #2" in out,
                    "asm output ends in SYS_EXIT")

        # ── Test 3: return N propagates as exit code. ───────
        stage_source(s, EXIT_C, "/tmp/exit42.c")
        out = send_cmd(s, "/bin/cc /tmp/exit42.c -o /tmp/exit42",
                       timeout=30.0)
        expect_pass(b"cc: wrote /tmp/exit42" in out,
                    "/bin/cc built exit42")
        out = send_cmd(s, "/tmp/exit42", timeout=20.0)
        print("--- /tmp/exit42 output ---")
        try: print(out.decode("utf-8", errors="replace"))
        except Exception: print(repr(out))
        print("--------------------------")
        expect_pass(b"about to exit" in out,
                    "exit42 printed via puts")
        expect_pass(b"exited with code 0x000000000000002a" in out,
                    "exit42 exited with code 42")

        print()
        print(f"{len(PASSES)} PASS / {len(FAILS)} FAIL")
        if FAILS:
            print("FAILED:")
            for f in FAILS: print(f"  - {f}")
            return 1
        return 0
    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)


if __name__ == "__main__":
    sys.exit(main())

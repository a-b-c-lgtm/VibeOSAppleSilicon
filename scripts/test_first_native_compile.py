#!/usr/bin/env python3
"""
scripts/test_first_native_compile.py — chapter 160 smoke test.

The first time the OS compiles a C source file *from disk*
end-to-end:

  host:  seed /data/hello.c via mkosfs2 name=path
  guest: boot OS
         cat /data/hello.c        (file survived the disk image)
         /bin/cc /data/hello.c -o /tmp/hello
         /tmp/hello               (exit 123, prints marker)

The C program exercises chapter 159's language additions:
locals, default-zero-init, `+` arithmetic, an interleaved
printf() that must not corrupt the stack frame.

This is the first program in the codebase whose `.c` source
ships as a file on a real disk image rather than as a string
baked into a Python harness or compiled at host build time.
"""
import os, sys, time, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_cc_hello import (  # type: ignore
    boot, conn, hard_kill, wait_for, send_cmd,
    reformat_data, PROMPT, drain,
)

# The exact source we want on /data/hello.c.  Marker is
# unique to chapter 160 so a grep over the boot log can
# attribute the success.
HELLO_C = b"""int main(void) {
    int a = 100;
    int b = 23;
    int c = a + b;
    printf("M124-COMPILED-OK\\n");
    return c;
}
"""

PASSES, FAILS = [], []


def expect_pass(cond, msg):
    if cond: print(f"PASS: {msg}"); PASSES.append(msg)
    else: print(f"FAIL: {msg}"); FAILS.append(msg)


def seed_data_with_hello():
    """Write HELLO_C to a temp file, then mkosfs2 the data image
    so it ends up at /data/hello.c in the guest."""
    with tempfile.NamedTemporaryFile(suffix=".c", delete=False) as f:
        f.write(HELLO_C)
        src_path = f.name
    try:
        subprocess.check_call(
            ["python3", f"{ROOT}/scripts/mkosfs2.py",
             f"{ROOT}/build/data.img", f"hello.c={src_path}"],
            stdout=subprocess.DEVNULL,
        )
    finally:
        try: os.unlink(src_path)
        except FileNotFoundError: pass


def main():
    print("[chapter 160] first native compile from /data/hello.c")
    seed_data_with_hello()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)
        time.sleep(1.5)
        drain(s, time.time() + 0.5)

        # 1. The seeded source must be on disk.
        out = send_cmd(s, "ls /data", timeout=10.0)
        expect_pass(b"hello.c" in out, "/data/hello.c visible on disk")

        # 2. Source content must survive the disk image round-trip.
        out = send_cmd(s, "cat /data/hello.c", timeout=10.0)
        expect_pass(b"int a = 100;" in out,
                    "/data/hello.c has the expected 'int a = 100;' line")
        expect_pass(b"M124-COMPILED-OK" in out,
                    "/data/hello.c has the expected marker literal")
        expect_pass(b"return c;" in out,
                    "/data/hello.c has the expected 'return c;' line")

        # 3. Compile straight from /data/, output to /tmp/.
        out = send_cmd(s, "/bin/cc /data/hello.c -o /tmp/hello",
                       timeout=45.0)
        ok = b"cc: wrote /tmp/hello" in out
        expect_pass(ok, "/bin/cc compiled the disk source")
        if not ok:
            print("  -- /bin/cc output (last 400 bytes) --")
            print("  " + repr(out[-400:]))

        # 4. Run the result.
        drain(s, time.time() + 0.3)
        out = send_cmd(s, "/tmp/hello", timeout=20.0)
        expect_pass(b"M124-COMPILED-OK" in out,
                    "/tmp/hello printed the runtime marker")
        # Exit code 100 + 23 = 123 = 0x7b.
        expect_pass(b"0x000000000000007b" in out,
                    "/tmp/hello exited with code 123 (a+b)")

        # 5. Recompile is idempotent (proves we didn't mutate the
        # disk source on read).
        send_cmd(s, "rm /tmp/hello /tmp/hello.cc.s /tmp/hello.cc.o "
                    "2>/dev/null; true", timeout=5.0)
        out = send_cmd(s, "/bin/cc /data/hello.c -o /tmp/hello",
                       timeout=45.0)
        expect_pass(b"cc: wrote /tmp/hello" in out,
                    "/bin/cc is idempotent across runs of the same source")
        drain(s, time.time() + 0.3)
        out = send_cmd(s, "/tmp/hello", timeout=20.0)
        expect_pass(b"0x000000000000007b" in out,
                    "re-built binary still exits with code 123")
    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)
        # Reset data.img to empty so other tests are not perturbed.
        reformat_data()

    total = len(PASSES) + len(FAILS)
    print()
    print(f"{len(PASSES)} PASS / {len(FAILS)} FAIL  (of {total})")
    sys.exit(0 if not FAILS else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
scripts/test_self_host_demo.py — chapter 125 smoke test.

Real GCC self-hosting (stage1 → stage2 → stage3 fixed point)
is out of reach for /bin/cc.  What IS in reach is showing the
upper bound of the language our in-guest compiler handles
today.  This test compiles, on the guest, the largest single
C program /bin/cc has ever digested: 11 local variables, a
10-term sum expression, four interleaved printf() calls, and
five dependent intermediate computations.

It is a single boot, a single compile, a single run.  If
this works, /bin/cc has demonstrated that everything it
learned in chapters 121/123 composes — printf calls do not
clobber locals; long left-associative chains do not exhaust
the expression-stack; dependent variables in sequence do
not collide on the same frame slot.

The same source ships on /data so the harness can also
verify "on-disk source, in-guest compile" still works (the
chapter 124 contract).
"""
import os, sys, time, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_cc_hello import (  # type: ignore
    boot, conn, hard_kill, wait_for, send_cmd,
    reformat_data, PROMPT, drain,
)

# The biggest single program /bin/cc has ever compiled.
# Exit code = 55 = 0x37.
DEMO_C = b"""int main(void) {
    int a = 1;
    int b = 2;
    int c = 3;
    int d = 4;
    int e = 5;
    int f = 6;
    int g = 7;
    int h = 8;
    int i = 9;
    int j = 10;
    int sum = a + b + c + d + e + f + g + h + i + j;
    printf("M125-STAGE-1\\n");
    int gauss = sum + 0;
    printf("M125-STAGE-2\\n");
    int doubled = gauss + gauss;
    printf("M125-STAGE-3\\n");
    int half = doubled - gauss;
    printf("M125-DONE\\n");
    int result = half - sum + sum;
    return result;
}
"""

PASSES, FAILS = [], []


def expect_pass(cond, msg):
    if cond: print(f"PASS: {msg}"); PASSES.append(msg)
    else: print(f"FAIL: {msg}"); FAILS.append(msg)


def seed_data_with_demo():
    with tempfile.NamedTemporaryFile(suffix=".c", delete=False) as f:
        f.write(DEMO_C); src = f.name
    try:
        subprocess.check_call(
            ["python3", f"{ROOT}/scripts/mkosfs2.py",
             f"{ROOT}/build/data.img", f"demo.c={src}"],
            stdout=subprocess.DEVNULL,
        )
    finally:
        try: os.unlink(src)
        except FileNotFoundError: pass


def main():
    print("[chapter 125] self-host bootstrap demo (upper bound of /bin/cc)")
    seed_data_with_demo()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)
        time.sleep(1.5)
        drain(s, time.time() + 0.5)

        # 1. Source visible on disk.
        out = send_cmd(s, "ls /data", timeout=10.0)
        expect_pass(b"demo.c" in out, "/data/demo.c is on disk")

        # 2. Compile it with the in-guest compiler.
        out = send_cmd(s, "/bin/cc /data/demo.c -o /tmp/demo",
                       timeout=60.0)
        ok = b"cc: wrote /tmp/demo" in out
        expect_pass(ok, "/bin/cc compiled the upper-bound program")
        if not ok:
            print("  -- /bin/cc output (last 500 bytes) --")
            print("  " + repr(out[-500:]))

        # 3. Inspect asm — must show many frame slot stores.
        out = send_cmd(s, "/bin/cc -S /data/demo.c -o /tmp/demo.s",
                       timeout=60.0)
        expect_pass(b"cc: emitted /tmp/demo.s" in out,
                    "/bin/cc -S produced asm")
        drain(s, time.time() + 0.3)
        out = send_cmd(s, "cat /tmp/demo.s", timeout=15.0)
        # 11 locals = 11 [sp, #N*8] store sites (one per int decl
        # init).  We assert at least 9 to leave slack.
        store_count = out.count(b"str  x0, [sp, #")
        expect_pass(store_count >= 9,
                    f"asm has many frame stores (got {store_count}, want >= 9)")
        # Long add chain should produce many add instructions.
        add_count = out.count(b"add  x0, x1, x0")
        expect_pass(add_count >= 9,
                    f"asm has many add ops for 10-term sum (got {add_count}, want >= 9)")

        # 4. Run the binary.
        drain(s, time.time() + 0.3)
        out = send_cmd(s, "/tmp/demo", timeout=30.0)
        # All four markers must appear, in order.
        for marker in (b"M125-STAGE-1", b"M125-STAGE-2",
                       b"M125-STAGE-3", b"M125-DONE"):
            expect_pass(marker in out,
                        f"runtime printed {marker.decode()}")
        # Exit code = 55 = 0x37 (printf must not have corrupted
        # locals across calls).
        expect_pass(b"0x0000000000000037" in out,
                    "exited with code 55 (locals survived 4 printf calls)")

        # 5. Run again to prove the binary is deterministic.
        drain(s, time.time() + 0.3)
        out = send_cmd(s, "/tmp/demo", timeout=30.0)
        expect_pass(b"0x0000000000000037" in out,
                    "second run also exits with code 55")
        m_count = out.count(b"M125-")
        expect_pass(m_count == 4,
                    f"second run printed exactly 4 markers (got {m_count})")
    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)
        reformat_data()

    total = len(PASSES) + len(FAILS)
    print()
    print(f"{len(PASSES)} PASS / {len(FAILS)} FAIL  (of {total})")
    sys.exit(0 if not FAILS else 1)


if __name__ == "__main__":
    main()

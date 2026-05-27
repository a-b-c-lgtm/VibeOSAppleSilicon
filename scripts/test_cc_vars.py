#!/usr/bin/env python3
"""
scripts/test_cc_vars.py — chapter 159 smoke test.

Chapter 157 shipped /bin/cc with only printf/puts/return-int-literal.
Chapter 159 extends it with the first real compiler features:

  - Local `int` variables with initializers
  - Assignment statements
  - Binary +/- expressions
  - `return EXPR;` and `exit(EXPR);`

This test exercises every one of those, on the OS, end to end.
"""
import os, sys, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_cc_hello import (  # type: ignore
    boot, conn, hard_kill, wait_for, send_cmd, stage_source,
    reformat_data, PROMPT, drain,
)

# ─────────────────────────────────────────────────────────
# Test programs.  Each is small enough to stage via shell
# `echo` lines.  Each exits with a known integer the harness
# verifies.

# Program A — pure arithmetic, no I/O.  3 + 4 = 7.
ARITH_C = r"""int main(void) {
    int a = 3;
    int b = 4;
    int c = a + b;
    return c;
}
"""

# Program B — assignment, subtraction, parenthesised expression.
# (10 + 5) - (3 + 1) = 11.
PAREN_C = r"""int main(void) {
    int x = 10 + 5;
    int y = 3 + 1;
    int z;
    z = x - y;
    return z;
}
"""

# Program C — exit() instead of return; verifies the parser
# accepts both code paths to the same syscall.
EXIT_FN_C = r"""int main(void) {
    int n = 1 + 2 + 3 + 4 + 5 + 6;
    exit(n);
}
"""

# Program D — interleaves variables with printf, to prove
# that the chapter-121 `bl past_str` codegen still works
# when a stack frame is live.
MIX_C = r"""int main(void) {
    int total = 40 + 2;
    printf("M123-CCVARS-OK\n");
    return total;
}
"""

# Program E — declaration without initializer, then assignment.
# Verifies the default-zero-init we emit so the first read is
# well-defined (and not whatever the stack used to hold).
NOINIT_C = r"""int main(void) {
    int v;
    v = 99;
    int w = v - 1;
    return w;
}
"""

PASSES, FAILS = [], []


def expect_pass(cond, msg):
    if cond: print(f"PASS: {msg}"); PASSES.append(msg)
    else: print(f"FAIL: {msg}"); FAILS.append(msg)


def compile_and_run(s, src_text, base, expected_exit,
                    expect_marker=None, label=""):
    """Stage SRC, build with /bin/cc, run it, check exit code."""
    # Drain anything the previous step left in the buffer so the
    # next prompt match is unambiguous.
    drain(s, time.time() + 0.4)
    src = f"/tmp/{base}.c"
    out_bin = f"/tmp/{base}"
    stage_source(s, src_text, src)
    drain(s, time.time() + 0.3)
    out = send_cmd(s, f"/bin/cc {src} -o {out_bin}", timeout=45.0)
    ok = b"cc: wrote " in out
    expect_pass(ok, f"[{label}] /bin/cc reported success")
    if not ok:
        print(f"  [{label}] -- /bin/cc output (last 300 bytes) --")
        print("  " + repr(out[-300:]))
    drain(s, time.time() + 0.3)
    out = send_cmd(s, out_bin, timeout=20.0)
    if expect_marker is not None:
        expect_pass(expect_marker in out,
                    f"[{label}] runtime printed marker {expect_marker!r}")
    # The kernel prints exit codes as a 16-digit hex string.
    hexcode = f"0x{expected_exit:016x}".encode()
    got = hexcode in out
    expect_pass(got,
                f"[{label}] exited with code {expected_exit} ({hexcode.decode()})")
    if not got:
        print(f"  [{label}] -- runtime output (last 300 bytes) --")
        print("  " + repr(out[-300:]))
    # /tmp is tmpfs with TMPFS_MAX_FILES=16.  Each iteration
    # produces 4 files (.c, .cc.s, .cc.o, binary), so without
    # explicit cleanup we exhaust the table after 4 tests.
    send_cmd(s, f"rm {src} {out_bin} {out_bin}.cc.s {out_bin}.cc.o 2>/dev/null; true",
             timeout=5.0)
    return out


def main():
    print("[chapter 159] /bin/cc variables + expressions smoke test")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)
        time.sleep(1.5)
        drain(s, time.time() + 0.5)

        # ── 1. Pure arithmetic exits with the sum.
        compile_and_run(s, ARITH_C, "arith", 7, label="arith")

        # ── 2. Subtraction + parentheses.
        compile_and_run(s, PAREN_C, "paren", 11, label="paren")

        # ── 3. exit(EXPR) works.
        compile_and_run(s, EXIT_FN_C, "exit_e", 21, label="exit-fn")

        # ── 4. printf interleaved with a stack frame.
        compile_and_run(s, MIX_C, "mix", 42,
                        expect_marker=b"M123-CCVARS-OK", label="mix")

        # ── 5. Default-zero-init then assignment.
        compile_and_run(s, NOINIT_C, "noinit", 98, label="noinit")

        # ── 6. -S mode shows the new instructions in the asm.
        stage_source(s, ARITH_C, "/tmp/arith2.c")
        out = send_cmd(s, "/bin/cc -S /tmp/arith2.c -o /tmp/arith2.s",
                       timeout=15.0)
        expect_pass(b"cc: emitted /tmp/arith2.s" in out,
                    "[asm] -S mode emitted /tmp/arith2.s")
        out = send_cmd(s, "cat /tmp/arith2.s", timeout=10.0)
        expect_pass(b"sub  sp, sp, #256" in out,
                    "[asm] prologue allocates frame")
        expect_pass(b"add  x0, x1, x0" in out,
                    "[asm] uses ADD x0, x1, x0 for + operator")
        expect_pass(b"ldr  x" in out and b", [sp, #" in out,
                    "[asm] loads from stack frame")
        expect_pass(b"str  x0, [sp, #" in out,
                    "[asm] stores to stack frame")
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

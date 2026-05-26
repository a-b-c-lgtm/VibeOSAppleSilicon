#!/usr/bin/env python3
"""Quick debug — boot OS, stage NOINIT_C, run /bin/cc -S to see what /bin/cc does."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_cc_hello import (
    boot, conn, hard_kill, wait_for, send_cmd, stage_source,
    reformat_data, PROMPT, drain,
)

NOINIT_C = r"""int main(void) {
    int v;
    v = 99;
    int w = v - 1;
    return w;
}
"""

reformat_data()
q = boot()
s = conn()
try:
    wait_for(s, PROMPT, timeout=20.0)
    time.sleep(1.5)
    drain(s, time.time() + 0.5)
    stage_source(s, NOINIT_C, "/tmp/noinit.c")
    print("=== cat /tmp/noinit.c ===")
    out = send_cmd(s, "cat /tmp/noinit.c", timeout=10.0)
    print(out.decode("utf-8","replace"))
    print("=== /bin/cc -S /tmp/noinit.c -o /tmp/noinit.s ===")
    out = send_cmd(s, "/bin/cc -S /tmp/noinit.c -o /tmp/noinit.s", timeout=15.0)
    print(out.decode("utf-8","replace"))
    print("=== cat /tmp/noinit.s ===")
    out = send_cmd(s, "cat /tmp/noinit.s", timeout=10.0)
    print(out.decode("utf-8","replace"))
    print("=== /bin/cc /tmp/noinit.c -o /tmp/noinit ===")
    out = send_cmd(s, "/bin/cc /tmp/noinit.c -o /tmp/noinit", timeout=30.0)
    print(out.decode("utf-8","replace"))
    print("=== /tmp/noinit ===")
    out = send_cmd(s, "/tmp/noinit", timeout=20.0)
    print(out.decode("utf-8","replace"))
finally:
    try: s.close()
    except Exception: pass
    hard_kill(q)

#!/usr/bin/env python3
"""
scripts/test_make_port.py — chapter 162 smoke test.

The first real `/bin/make` invocation on osdev: parse a real
Makefile from disk, recurse through its dependency graph,
spawn the recipe commands via the kernel's spawn syscall,
and produce a compiled binary at the end.

Disk layout (seeded via mkosfs2):

    /data/greeter.c      a chapter-121-grade .c source
    /data/Makefile       a 3-rule Makefile that uses /bin/cc

The Makefile exercises the bits that distinguish /bin/make
from "just run /bin/cc by hand":

  - first-target-by-default selection (no target on the cmd line)
  - dependency ordering (all -> prepare -> build)
  - multiple recipe lines per rule
  - echo prefix (every recipe line should be printed before
    it's run, so we can grep the boot log for it)

After /bin/make returns, /tmp/greeter must exist and run.
"""
import os, sys, time, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_cc_hello import (  # type: ignore
    boot, conn, hard_kill, wait_for, send_cmd,
    reformat_data, PROMPT, drain,
)

GREETER_C = b"""int main(void) {
    printf("M126-GREETER-OK\\n");
    return 0;
}
"""

# Real Makefile syntax: targets followed by tab-prefixed
# recipe lines.  Three rules.  Default target = "all" (first).
MAKEFILE = (
    b"all: prepare build\n"
    b"\t/bin/echo M126-MAKE-ALL-DONE\n"
    b"\n"
    b"prepare:\n"
    b"\t/bin/echo M126-PREPARE-RAN\n"
    b"\n"
    b"build:\n"
    b"\t/bin/cc /data/greeter.c -o /tmp/greeter\n"
)

PASSES, FAILS = [], []


def expect_pass(cond, msg):
    if cond: print(f"PASS: {msg}"); PASSES.append(msg)
    else: print(f"FAIL: {msg}"); FAILS.append(msg)


def seed_data():
    """Stage greeter.c AND Makefile in one mkosfs2 invocation."""
    with tempfile.NamedTemporaryFile(suffix=".c", delete=False) as fc:
        fc.write(GREETER_C); src_path = fc.name
    with tempfile.NamedTemporaryFile(suffix=".mk", delete=False) as fm:
        fm.write(MAKEFILE); mk_path = fm.name
    try:
        subprocess.check_call(
            ["python3", f"{ROOT}/scripts/mkosfs2.py",
             f"{ROOT}/build/data.img",
             f"greeter.c={src_path}",
             f"Makefile={mk_path}"],
            stdout=subprocess.DEVNULL,
        )
    finally:
        for p in (src_path, mk_path):
            try: os.unlink(p)
            except FileNotFoundError: pass


def main():
    print("[chapter 162] /bin/make port smoke test")
    seed_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)
        time.sleep(1.5)
        drain(s, time.time() + 0.5)

        # 1. /bin/make is on disk (chapter 162 wired it into the
        # build).  ls /bin/make must succeed.
        out = send_cmd(s, "ls /bin/make", timeout=10.0)
        expect_pass(b"/bin/make" in out and b"cannot" not in out,
                    "/bin/make is installed")

        # 2. The Makefile and source are on /data.
        out = send_cmd(s, "ls /data", timeout=10.0)
        expect_pass(b"Makefile" in out, "/data/Makefile is on disk")
        expect_pass(b"greeter.c" in out, "/data/greeter.c is on disk")

        # 3. Run /bin/make with -f to read the Makefile from /data.
        # No explicit target -> first rule "all" is built.
        out = send_cmd(s, "/bin/make -f /data/Makefile",
                       timeout=60.0)
        if b"make: built 'all'" not in out:
            print("  -- /bin/make output (last 600 bytes) --")
            print("  " + repr(out[-600:]))

        # 3a. Default-target selection: the dep DFS reached
        # "prepare" before "build" because "prepare" appears first
        # in the dep list of "all".  Both markers must appear.
        expect_pass(b"M126-PREPARE-RAN" in out,
                    "make ran the 'prepare' rule")
        # 3b. The 'build' rule actually invoked /bin/cc.
        expect_pass(b"cc: wrote /tmp/greeter" in out,
                    "make ran /bin/cc through the 'build' rule")
        # 3c. The 'all' rule's own recipe ran AFTER the deps.
        expect_pass(b"M126-MAKE-ALL-DONE" in out,
                    "make ran the 'all' recipe after its deps")
        # 3d. /bin/make's terminating success line.
        expect_pass(b"make: built 'all'" in out,
                    "/bin/make announced 'built all'")

        # 4. Dependency *ordering* — prepare must appear BEFORE
        # the cc invocation, which must appear BEFORE all-done.
        pos_prepare = out.find(b"M126-PREPARE-RAN")
        pos_cc      = out.find(b"cc: wrote /tmp/greeter")
        pos_done    = out.find(b"M126-MAKE-ALL-DONE")
        order_ok = (0 <= pos_prepare < pos_cc < pos_done)
        expect_pass(order_ok,
                    f"recipe order is prepare < build < all "
                    f"(positions: {pos_prepare}, {pos_cc}, {pos_done})")

        # 5. The compiled binary exists and runs.
        drain(s, time.time() + 0.3)
        out = send_cmd(s, "/tmp/greeter", timeout=15.0)
        expect_pass(b"M126-GREETER-OK" in out,
                    "/tmp/greeter (built by make) prints its marker")
        expect_pass(b"0x0000000000000000" in out,
                    "/tmp/greeter exited with code 0")

        # 6. Re-run /bin/make — second time should still succeed
        # (we always rebuild for now, no stat() yet).
        drain(s, time.time() + 0.3)
        out = send_cmd(s, "/bin/make -f /data/Makefile",
                       timeout=60.0)
        expect_pass(b"make: built 'all'" in out,
                    "second /bin/make invocation also succeeded")

        # 7. Explicit target selection.  Build just 'prepare' —
        # the cc rule must NOT run.
        drain(s, time.time() + 0.3)
        out = send_cmd(s,
                       "/bin/make -f /data/Makefile prepare",
                       timeout=15.0)
        expect_pass(b"M126-PREPARE-RAN" in out,
                    "explicit 'make prepare' ran the prepare recipe")
        expect_pass(b"cc:" not in out,
                    "explicit 'make prepare' did NOT run the cc rule")
        expect_pass(b"make: built 'prepare'" in out,
                    "explicit 'make prepare' announced its own target")
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

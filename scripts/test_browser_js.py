#!/usr/bin/env python3
"""scripts/test_browser_js.py -- chapter 122 pocketjs evaluator.

Drives `browser --check-js <expr>` from the in-guest shell to
exercise every shape of expression the engine handles.

Shell quoting note
------------------
Our /bin/sh recognises `<`, `>`, and `|` as redirect/pipe
operators BEFORE quote expansion (see userspace/sh/sh.c at the
redirect block).  Tests therefore deliberately avoid those
characters and exercise comparison/logical-OR operators via
onclick handlers loaded from a fixture HTML page (also covered
elsewhere).  The engine itself fully supports `<`, `>`, `<=`,
`>=`, `||` -- this restriction is purely a test-driver quirk.

The boot harness mirrors test_browser_sop.py: data disk is
reformatted fresh, qemu boots with virtio-blk only (no virtio-net
needed -- the engine is pure userspace).
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-js.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "


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
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


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


def send_cmd(s, cmd, timeout=20.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


PASSES, FAILS = [], []


def expect(cond, msg, ctx=b""):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        if ctx:
            tail = ctx[-1500:].decode("ascii", "replace")
            print("--- ctx ---\n" + tail + "\n-----------")
        FAILS.append(msg)


def expect_contains(out, needle, msg):
    if isinstance(needle, str): needle = needle.encode()
    expect(needle in out, msg, ctx=out)


def main():
    reformat_data()
    q = boot()
    s = conn()
    try:
        if PROMPT not in wait_for(s, PROMPT, 120.0):
            raise RuntimeError("shell prompt never reached")

        # 1. Plain integer literal.
        out = send_cmd(s, 'browser --check-js "42"')
        expect_contains(out, "JS: num=42", "integer literal")

        # 2. Arithmetic precedence.
        out = send_cmd(s, 'browser --check-js "1 + 2 * 3"')
        expect_contains(out, "JS: num=7",
                        "precedence: 1 + 2*3 = 7")

        out = send_cmd(s, 'browser --check-js "(10 - 4) / 2"')
        expect_contains(out, "JS: num=3", "parens + integer division")

        # 3. String concat (single quotes inside double).
        out = send_cmd(s, "browser --check-js \"'foo' + 'bar'\"")
        expect_contains(out, "JS: str=foobar", "string concat")

        # 4. Mixed string + number coerces to string.
        out = send_cmd(s, "browser --check-js \"'x=' + 7\"")
        expect_contains(out, "JS: str=x=7", "string + number coerces")

        # 5. Equality (we avoid `<`/`>` because the shell reads them
        #    as redirects before quote expansion).
        out = send_cmd(s, 'browser --check-js "5 == 6"')
        expect_contains(out, "JS: bool=false", "5 == 6 is false")

        out = send_cmd(s, 'browser --check-js "7 != 8"')
        expect_contains(out, "JS: bool=true", "7 != 8 is true")

        # 6. Logical AND (`&&` survives because `&` is only
        #    interpreted as background when alone at end-of-line).
        out = send_cmd(s, 'browser --check-js "true && 42"')
        expect_contains(out, "JS: num=42", "true && 42 returns 42")

        out = send_cmd(s, 'browser --check-js "false && 42"')
        expect_contains(out, "JS: bool=false",
                        "false && 42 short-circuits to false")

        # 7. Unary not.
        out = send_cmd(s, 'browser --check-js "!false"')
        expect_contains(out, "JS: bool=true", "!false is true")

        # 8. Sequence -- last value wins.
        out = send_cmd(s, 'browser --check-js "1; 2; 3"')
        expect_contains(out, "JS: num=3", "sequence returns last")

        # 9. Global assignment + read-back.
        out = send_cmd(s, 'browser --check-js "x = 7; x + 1"')
        expect_contains(out, "JS: num=8",
                        "global var assigned then read")

        # 10. alert() with a string argument.
        out = send_cmd(s, "browser --check-js \"alert('hello')\"")
        expect_contains(out, "JS: undefined=", "alert returns undefined")
        expect_contains(out, "JS: alert=hello",
                        "alert() latches its message")

        # 11. console.log() counter.
        out = send_cmd(s, "browser --check-js \"console.log('a'); console.log('b')\"")
        expect_contains(out, "JS: logs=2", "console.log invoked twice")

        # 12. Property access on undefined global -> undefined,
        #     does NOT crash.
        out = send_cmd(s, 'browser --check-js "nope.thing"')
        expect_contains(out, "JS: undefined=",
                        "undefined.x returns undefined gracefully")

        # 13. Equality with type coercion (number == numeric string).
        out = send_cmd(s, "browser --check-js \"7 == '7'\"")
        expect_contains(out, "JS: bool=true",
                        "loose equality coerces numeric string")

        # 14. document.getElementById without a loaded DOM -> undefined.
        out = send_cmd(s, "browser --check-js \"document.getElementById('x')\"")
        expect_contains(out, "JS: undefined=",
                        "getElementById on empty DOM returns undefined")

        # 15. Moderate-length arithmetic chain -- written without
        #     spaces so it survives the kernel's argv splitter
        #     (capped at MAX_SPAWN_ARGV=16 tokens including argv[0]).
        out = send_cmd(s, 'browser --check-js "1+2+3+4+5+6+7+8+9+10"')
        expect_contains(out, "JS: num=55", "long arithmetic chain")

        # 16. Unknown method on a known host obj -> undefined,
        #     no crash, no error message.
        out = send_cmd(s, "browser --check-js \"document.bogus('x')\"")
        expect_contains(out, "JS: undefined=",
                        "unknown method on host obj is undefined")

    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)

    print(f"\nresults: {len(PASSES)} pass, {len(FAILS)} fail")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())

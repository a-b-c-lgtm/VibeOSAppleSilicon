#!/usr/bin/env python3
"""scripts/test_make_v2.py -- chapter 133b expanded /bin/make.

Chapter 126 shipped a 351-LoC /bin/make that handled exactly
the toy shape `target: deps\n\tcmd...`.  No variables, no
$(VAR) expansion, no $@ / $< / $^, no pattern rules, no
.PHONY, no recipe prefixes, no line continuation.  That was
fine for one-off "build me one .c file" demos but a real
multi-file project cannot describe its build with that
vocabulary.

Chapter 133b expands /bin/make so it can drive the Makefiles
real C projects ship.  The added vocabulary:

  1. Variable definitions:  CC = /bin/gcc
  2. Variable expansion:    $(CC), ${CC}, $$  (recursive,
                            cycle-guarded at depth 8)
  3. Automatic variables:   $@ (target), $< (first dep),
                            $^ (all deps)
  4. Pattern rules:         %.o: %.c
  5. .PHONY targets         (re-run every time)
  6. Recipe prefixes:       @ (silent), - (ignore error)
  7. Line continuation:     trailing `\\` joins next line

Storage caveats baked in:
  - 768 KiB bss for the per-rule dep slab (32 rules x 256
    deps x 96 bytes) -- needed to be able to express
    "doom: am_map.o doomdef.o ..." with ~200 .o deps
  - The synthetic pattern slot is shared, so mk_build()
    snapshots the matched rule onto its own stack frame
    before recursing on deps.  Snapshot is ~8 KiB to fit
    inside our 64 KiB (USER_STACK_PAGES=16) user stack.

Test ladder (4 steps, 8 expectations):

  1. /bin/make is shipped as an ELF; -f flag works
  2. `make -f /bin/mk_test.mk` runs to completion and
     emits the silent @echo banners for compile + link
  3. The produced /tmp/mk_hello is an ELF
  4. /tmp/mk_hello runs and prints "hello A=42"
     (proving $@ / $< / $^ / pattern-rule expansion all
      produced sane gcc command lines)

Fixture lives at:
  /bin/mk_test.mk        (Makefile under test)
  /bin/mk_helloA.c       (calls hello_from_B, prints n)
  /bin/mk_helloB.c       (returns 42)

We do not test 'make clean' explicitly -- /bin/rm doesn't
exist, but the recipe lines use `-` prefix so make should
swallow the spawn failures.  Step 2's "make: built 'all'"
proves the whole chain reached the top successfully.
"""

import os
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-make-v2.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "


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
    print("[chapter 133b] expanded /bin/make")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        # --- sanity: fixture present ------------------------
        out = send_cmd(s, "cat /bin/mk_test.mk", timeout=10.0)
        expect(b"CC = /bin/gcc" in out and b"%.o: /bin/%.c" in out,
               "sanity: /bin/mk_test.mk shipped on OSFS-1")

        out = send_cmd(s, "cat /bin/mk_helloA.c", timeout=10.0)
        expect(b"hello_from_B" in out,
               "sanity: /bin/mk_helloA.c shipped on OSFS-1")

        # --- step 1: /bin/make is an ELF --------------------
        out = send_cmd(s, "cat /bin/make", timeout=20.0)
        expect(b"\x7FELF" in out,
               "step 1: /bin/make is an ELF binary")

        # --- step 2: run the Makefile -----------------------
        out = send_cmd(s, "/bin/make -f /bin/mk_test.mk",
                       timeout=300.0)
        sys.stdout.write("--- /bin/make output ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n------------------------\n")

        expect(b"[Compiling /bin/mk_helloA.c]" in out,
               "step 2a: pattern rule fired for mk_helloA.c "
               "($< expanded correctly)")
        expect(b"[Compiling /bin/mk_helloB.c]" in out,
               "step 2b: pattern rule fired for mk_helloB.c "
               "(second pattern instance)")
        expect(b"[Linking /tmp/mk_hello]" in out,
               "step 2c: top-level link recipe fired "
               "($@ expanded to /tmp/mk_hello)")
        expect(b"make: built 'all'" in out,
               "step 2d: make completed all targets")

        # --- step 3: produced binary is an ELF --------------
        out = send_cmd(s, "cat /tmp/mk_hello", timeout=20.0)
        produced = b"\x7FELF" in out
        expect(produced,
               "step 3: /tmp/mk_hello is an ELF binary")

        if not produced:
            print("\nBuild failed; skipping run.")
            return _report()

        # --- step 4: run the produced binary ----------------
        out = send_cmd(s, "/tmp/mk_hello", timeout=15.0)
        sys.stdout.write("--- /tmp/mk_hello output ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n----------------------------\n")
        expect(b"hello A=42" in out,
               "step 4: produced binary runs and "
               "prints 'hello A=42' (proves link order "
               "via $^ was correct)")

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

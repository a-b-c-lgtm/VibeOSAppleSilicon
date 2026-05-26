#!/usr/bin/env python3
"""scripts/test_bin_ld_ar.py -- /bin/ld + /bin/ar smoke.

Chapter 119 originally shipped a toy hand-rolled linker that
hard-coded the user load address (0x1000100000) and entry
symbol.  Chapter 131f swapped /bin/ld for the real GNU
binutils `ld-new` cross-built for our aarch64-osdev target
(see scripts/test_guest_ld.py + the Makefile BINUTILS_LD_NEW
rules).  /bin/ar is still the toy chapter-119 implementation.

GNU ld's default linker script places .text at 0x00400000 —
NOT the 0x1000100000 the kernel ELF loader expects.  Chapter
131f ships userspace/linker_user.ld on the OSFS-1 disk as
/bin/osdev.ld; every test that links via /bin/ld passes
`-T /bin/osdev.ld` to override the default script (this is
also what /bin/cc does in userspace/cc/cc.c).

End-to-end pipeline:
  1. Stage a tiny `.s` file in /tmp.
  2. Assemble it with /bin/as → /tmp/hello.o
  3. Link with /bin/ld -T /bin/osdev.ld → /tmp/hello
  4. Wrap the object with /bin/ar → /tmp/libhello.a
  5. List the archive with `ar t`.
  6. Spawn the linked binary directly via the shell.  It
     should exit 42 (our SOURCE sets x0=42 then sys_exit).

Asserts:
  - /bin/as ran without printing 'error'/'fatal' (GNU as is
    silent on success).
  - /bin/ld produced a non-empty ELF starting with the right
    magic bytes and containing the encoded MOVZ x0,#42 + MOVZ
    x8,#2 bytes.  GNU ld is also silent on success.
  - /bin/ar archive starts with the "!<arch>\\n" magic.
  - `ar t /tmp/libhello.a` reports `hello.o` and "1 members".
  - Running /tmp/hello exits with status 42 (encoded as a
    "exited with code 0x000000000000002a" line from the
    kernel sys_exit reporter).
"""

import os, signal, socket, subprocess, sys, time, select, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-bin-ld.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

# A minimal program: MOVZ x0,#42 then SVC #0 with x8=2 (sys_exit).
# Our crt0 isn't used because we set the entry symbol directly.
SOURCE = r""".text
.global _start
_start:
    mov x0, #42
    mov x8, #2
    svc #0
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


PASSES, FAILS = [], []


def expect(cond, msg):
    if cond: print(f"PASS: {msg}"); PASSES.append(msg)
    else: print(f"FAIL: {msg}"); FAILS.append(msg)


def main():
    print("[chapter 131f] /bin/ld (GNU binutils) + /bin/ar smoke test")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)

        # Stage source.
        send_cmd(s, "rm /tmp/hello.s 2>/dev/null; true")
        for line in SOURCE.strip("\n").split("\n"):
            send_cmd(s, f"echo '{line}' >> /tmp/hello.s")

        # Assemble.  GNU as is silent on success.
        out = send_cmd(s, "/bin/as /tmp/hello.s -o /tmp/hello.o",
                        timeout=20.0)
        expect(b"error" not in out.lower() and b"fatal" not in out.lower(),
               "/bin/as ran without error/fatal output")

        # Verify .o is a real ELF64 AArch64 ET_REL.
        out = send_cmd(s, "cat /tmp/hello.o", timeout=20.0)
        expect(b"\x7FELF" in out, ".o starts with ELF magic")
        expect(b"\xB7\x00" in out, ".o has EM_AARCH64 bytes")

        # Link with -T /bin/osdev.ld (kernel ELF-loader VA
        # contract: PT_LOAD @ 0x1000100000, entry _start for this
        # raw-asm program — overrides the script's default
        # ENTRY(_user_start) via the explicit -e flag).
        out = send_cmd(s,
            "/bin/ld -T /bin/osdev.ld -e _start "
            "-o /tmp/hello /tmp/hello.o",
            timeout=30.0)
        print("--- /bin/ld output ---")
        try: print(out.decode("utf-8", errors="replace"))
        except Exception: print(repr(out))
        print("----------------------")
        expect(b"error" not in out.lower() and b"fatal" not in out.lower(),
               "/bin/ld ran without error/fatal output")

        # Inspect the linked ELF.
        out = send_cmd(s, "cat /tmp/hello", timeout=20.0)
        expect(b"\x7FELF" in out,
               "linked output starts with ELF magic")
        # MOVZ x0, #42 = 40 05 80 D2
        expect(b"\x40\x05\x80\xD2" in out,
               "encoded MOVZ x0,#42 present in linked image")
        # MOVZ x8, #2 = 48 00 80 D2
        expect(b"\x48\x00\x80\xD2" in out,
               "encoded MOVZ x8,#2 present in linked image")

        # Run the linked binary; expect exit code 42.
        out = send_cmd(s, "/tmp/hello", timeout=20.0)
        print("--- /tmp/hello output ---")
        try: print(out.decode("utf-8", errors="replace"))
        except Exception: print(repr(out))
        print("-------------------------")
        expect(b"exited with code 0x000000000000002a" in out
               or b"exit code 0x000000000000002a" in out
               or b"exited with code 0x2a" in out
               or b"exit 42" in out,
               "/tmp/hello exited with code 42")

        # Make an archive.
        out = send_cmd(s, "/bin/ar rc /tmp/libhello.a /tmp/hello.o",
                        timeout=20.0)
        expect(b"ar: wrote /tmp/libhello.a" in out,
               "/bin/ar rc printed success line")

        # Verify ar magic and listing.
        out = send_cmd(s, "cat /tmp/libhello.a", timeout=20.0)
        expect(b"!<arch>\n" in out,
               "archive starts with !<arch> magic")

        out = send_cmd(s, "/bin/ar t /tmp/libhello.a", timeout=20.0)
        expect(b"hello.o" in out,
               "ar t lists hello.o")
        expect(b"1 members" in out,
               "ar t reports 1 members")

    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)

    print(f"\n{len(PASSES)} PASS / {len(FAILS)} FAIL")
    if FAILS:
        print("FAILED:")
        for f in FAILS: print(f"  - {f}")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()

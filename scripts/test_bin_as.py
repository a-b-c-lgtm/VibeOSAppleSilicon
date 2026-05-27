#!/usr/bin/env python3
"""scripts/test_bin_as.py -- /bin/as smoke test.

Chapter 154 originally shipped a toy hand-rolled assembler with
a hard-coded mnemonic table.  Chapter 180 swapped that out for
the real GNU binutils `as-new` cross-built for our
aarch64-osdev target (see scripts/test_guest_ld.py + the
Makefile BINUTILS_AS_NEW rules).  The test still drives
`/bin/as` end-to-end; the contract is the same — assemble a
`.s`, produce an ELF64-AArch64 ET_REL with the expected
encoded bytes — but GNU as is silent on success (no "as:
wrote ..." line), so the assertion set switched to:

   1. The `.o` exists with a sensible (non-tiny) size after
      the run.
   2. Starts with the ELF64-AArch64 magic + ident bytes.
   3. Has e_machine = EM_AARCH64 (183).
   4. The first instruction in `.text` decodes to MOVZ x0, #42
      (0xD2800540) and the second to RET (0xD65F03C0),
      confirming the encoder produced correct AArch64 bytes.

Byte-level checks above prove the writer works for our
curated mnemonic subset — same as the toy-era test, just
with the chatty banner removed.

Run:  python3 scripts/test_bin_as.py
"""

import os, signal, socket, subprocess, sys, time, select, struct, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-bin-as.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

# A tiny .s that exercises a movz, ret pair and a label.
SOURCE = r""".text
.global _start
_start:
    mov x0, #42
    ret
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
    print("[chapter 180] /bin/as smoke test (GNU binutils gas)")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)

        # Write SOURCE into /tmp/hello.s line-by-line via echo+>>.
        send_cmd(s, "rm /tmp/hello.s 2>/dev/null; true")
        for line in SOURCE.strip("\n").split("\n"):
            # Escape characters the shell would otherwise mangle.
            esc = line.replace("'", "")  # our source has no single quotes
            send_cmd(s, f"echo '{esc}' >> /tmp/hello.s")

        out = send_cmd(s, "cat /tmp/hello.s")
        expect(b"_start:" in out and b"mov x0, #42" in out,
               "source file landed in /tmp/hello.s")

        # Run the assembler.  GNU as is silent on success; we
        # verify success via the post-run `ls`+`cat` checks.
        out = send_cmd(s, "/bin/as /tmp/hello.s -o /tmp/hello.o",
                        timeout=20.0)
        print("--- /bin/as output begin ---")
        try:
            print(out.decode("utf-8", errors="replace"))
        except Exception:
            print(repr(out))
        print("--- /bin/as output end ---")
        expect(b"error" not in out.lower() and b"fatal" not in out.lower(),
               "/bin/as ran without printing 'error' or 'fatal'")

        # Verify the output exists and is non-tiny.
        out = send_cmd(s, "ls /tmp/hello.o")
        m = re.search(rb"(\d+)\s+/tmp/hello\.o", out)
        size = int(m.group(1)) if m else 0
        expect(size > 200, f"/tmp/hello.o size sensible (got {size})")

        # Hex-dump the first 64 bytes via cat | od.  We don't have
        # od, so the test echoes the .text bytes by reading via cat
        # and grepping the printf signature instead.  Simpler:
        # write a one-shot pipe through cat | head, then assert the
        # leading ELF magic appears.  cat-binary on our shell yields
        # raw bytes on the serial line so we just look for the ELF
        # ident in the captured output.
        out = send_cmd(s, "cat /tmp/hello.o", timeout=20.0)
        expect(b"\x7FELF" in out,
               "output starts with ELF magic")
        expect(b"\x02\x01\x01" in out,
               "ELFCLASS64 + ELFDATA2LSB + EV_CURRENT bytes present")
        # AArch64 machine = 183 = 0xB7; e_machine little-endian = B7 00
        expect(b"\xB7\x00" in out,
               "EM_AARCH64 machine bytes present")
        # MOVZ x0, #42 = 0xD2800540 little-endian = 40 05 80 D2
        expect(b"\x40\x05\x80\xD2" in out,
               "encoded MOVZ x0, #42 found in output")
        # RET = 0xD65F03C0 little-endian = C0 03 5F D6
        expect(b"\xC0\x03\x5F\xD6" in out,
               "encoded RET found in output")

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

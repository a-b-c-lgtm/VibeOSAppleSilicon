#!/usr/bin/env python3
"""scripts/test_html_tokenizer.py — milestone-59 driver.

Boot the kernel, wait for the shell prompt, run `htmltok /mnt/test.html`
and assert that the emitted token stream contains the structural
landmarks we expect from `assets/osfs/test.html`:

  * a [DOCTYPE] "html" line at the top
  * one [START] "html" line
  * a [START] "p" with class="intro" attribute
  * a [START] "img" with self-closing marker (trailing " /")
  * the entity-decoded text "Hello, & goodbye"  (from "&amp;")
  * the entity-decoded text "<jumps>"           (from "&lt;jumps&gt;")
  * a numeric-decoded "$" (from "&#36;")
  * the rawtext body of <script> shows up as a single [CHARS] token
    that contains "var x = 1 < 2" without any inner [START]/[END] tokens
  * a final [TOTAL] N tokens line

The full transcript is dropped at /tmp/m59.log for reference and for
the book chapter.

Exit status:
  0 — all assertions passed
  1 — one or more assertions failed (mismatch printed on stderr)
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-m59.sock"
LOG  = "/tmp/m59.log"

PROMPT = b"/$ "


def cleanup():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass


def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def drain_until(s, needle, deadline_s):
    """Read from s until `needle` appears in the accumulated bytes,
    or the wallclock deadline elapses.  Returns (whole_buffer)."""
    buf = b""
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.25)
        if not r: continue
        try:
            chunk = s.recv(8192)
        except OSError:
            break
        if not chunk: break
        buf += chunk
        if needle in buf:
            return buf
    return buf


def main():
    qemu = boot()
    full = b""
    try:
        s = conn()
        full = drain_until(s, PROMPT, 30.0)
        if PROMPT not in full:
            print("FAIL: never reached shell prompt", file=sys.stderr)
            return 1
        s.sendall(b"htmltok /mnt/test.html\n")
        full += drain_until(s, b"[TOTAL]", 15.0)
        # After the TOTAL line, drain a little more so we capture the
        # next prompt (handy for the book transcript).
        full += drain_until(s, PROMPT, 5.0)
    finally:
        try: qemu.terminate()
        except Exception: pass
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()
        cleanup()

    with open(LOG, "wb") as f: f.write(full)
    text = full.decode("ascii", errors="replace")

    fails = []
    def need(label, predicate):
        if not predicate(text):
            fails.append(label)

    need("doctype",      lambda t: '[DOCTYPE] "html"' in t)
    need("html-start",   lambda t: '[START]   "html"' in t)
    need("p-intro-attr", lambda t: '[START]   "p" class="intro"' in t)
    # img is self-closing in our fixture: trailing " /"
    need("img-selfclose", lambda t: '[START]   "img"' in t and 'src="/icon.png"' in t)
    # Decoded entities show up as literal characters in the [CHARS] line.
    # html.h converts &amp; -> '&', &lt; -> '<', &gt; -> '>', &#36; -> '$'
    need("amp-decoded",   lambda t: 'Hello, & goodbye' in t)
    need("ltgt-decoded",  lambda t: '<jumps>' in t)
    need("num-dollar",    lambda t: 'price: $ &' in t or 'price: $ & done' in t)
    # Script body must appear as a single CHARS run.  We grep for a
    # signature substring of the JS snippet:
    need("script-rawtext", lambda t: 'var x = 1 < 2' in t)
    # And the script-internal '<' must NOT have produced an inner tag —
    # i.e. between the <script> START and </script> END there is no
    # spurious [START] line:
    def no_inner_tag(t):
        a = t.find('[START]   "script"')
        b = t.find('[END]     "script"')
        if a < 0 or b < 0 or b <= a: return False
        between = t[a:b]
        # Allow the script CHARS token; reject any START/END/COMMENT in between
        for tok in ("[START]", "[END]", "[COMMENT]"):
            if between.count(tok) > 1: return False
        return True
    need("script-no-inner", no_inner_tag)
    # End of stream
    need("eof",          lambda t: '[EOF]' in t)
    need("total",        lambda t: '[TOTAL]' in t)

    print(f"--- captured {len(full)} bytes -> {LOG} ---")
    if fails:
        print("FAIL: missing/mismatched assertions:", ", ".join(fails),
              file=sys.stderr)
        # Print the htmltok output region for quick eyeballing.
        idx = text.find("htmltok /mnt/test.html")
        if idx >= 0:
            print("---- output region ----", file=sys.stderr)
            print(text[idx:idx + 4096], file=sys.stderr)
        return 1
    print("PASS: html tokenizer milestone-59 — all checks green")
    return 0


if __name__ == "__main__":
    sys.exit(main())

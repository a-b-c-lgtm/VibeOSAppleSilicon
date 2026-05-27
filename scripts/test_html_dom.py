#!/usr/bin/env python3
"""scripts/test_html_dom.py — HTML DOM driver.

Boot the kernel, wait for the shell prompt, run `htmldom /mnt/test.html`
and assert that the printed tree contains the structural landmarks
we expect from `assets/osfs/test.html` after html.h tokenizes it
and dom.h folds those tokens into a tree:

  * a top-level [DOC] root
  * exactly one [DOCTYPE] "html" attached to it
  * an [ELEM] "html" with lang="en"
  * an [ELEM] "head" inside it, containing meta/title/style
  * the title text "Tokenizer fixture"
  * the style tag with its rawtext body
  * an [ELEM] "body" containing a comment, h1, p, ul (with li's),
    void <img>, <script> with rawtext, and a final <p>
  * the entity-decoded text "Hello, & goodbye" inside the h1
  * the "<jumps>" text inside the first paragraph
  * the "$ &" text inside the last paragraph
  * the void <img> attached to body (no "/" attribute, two attrs)
  * the script body "var x = 1 < 2" present as a single TEXT child
    of <script> with no spurious inner ELEM
  * a final [TOTAL] N nodes line

The full transcript is dropped at /tmp/m60.log for the book chapter.

Exit status:
  0 — all assertions passed
  1 — one or more assertions failed (mismatch printed on stderr)
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-m60.sock"
LOG  = "/tmp/m60.log"

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
        s.sendall(b"htmldom /mnt/test.html\n")
        full += drain_until(s, b"[TOTAL]", 15.0)
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

    # --- structural skeleton ---
    need("doc-root",     lambda t: '[DOC]' in t)
    need("doctype",      lambda t: '[DOCTYPE] "html"' in t)
    need("html-elem",    lambda t: '[ELEM] "html" lang="en"' in t)
    need("head-elem",    lambda t: '[ELEM] "head"' in t)
    need("body-elem",    lambda t: '[ELEM] "body"' in t)

    # --- head children ---
    need("meta-charset", lambda t: '[ELEM] "meta" charset="utf-8"' in t)
    need("title-elem",   lambda t: '[ELEM] "title"' in t)
    need("title-text",   lambda t: '[TEXT] "Tokenizer fixture"' in t)
    need("style-elem",   lambda t: '[ELEM] "style"' in t)
    need("style-rawtext",lambda t: 'body { color: red; }' in t)

    # --- body children ---
    need("comment",      lambda t: '[COMMENT] " a comment in the middle "' in t)
    need("h1-elem",      lambda t: '[ELEM] "h1"' in t)
    # entity-decoded "&amp;" -> "&"
    need("amp-decoded",  lambda t: '[TEXT] "Hello, & goodbye"' in t)
    need("p-elem-attrs", lambda t: '[ELEM] "p" class="intro" id="lead"' in t)
    # entity-decoded "&lt;jumps&gt;" -> "<jumps>"
    need("ltgt-decoded", lambda t: '<jumps>' in t)

    # --- list ---
    need("ul-elem",      lambda t: '[ELEM] "ul"' in t)
    need("li-one",       lambda t: '[TEXT] "one"' in t)
    need("li-two",       lambda t: '[TEXT] "two"' in t)
    need("li-three",     lambda t: '[TEXT] "three"' in t)
    # last li carried unquoted attr value + boolean attr
    need("li-attrs",     lambda t: 'data-x="42"' in t and 'disabled=""' in t)

    # --- void img: parsed as element with attrs but no children ---
    need("img-elem",     lambda t: '[ELEM] "img" src="/icon.png" alt="x"' in t)

    # --- script: one TEXT child carrying the raw JS, no inner ELEM ---
    need("script-elem",  lambda t: '[ELEM] "script"' in t)
    need("script-text",  lambda t: 'var x = 1 < 2' in t)

    def script_no_inner(t):
        # The script ELEM line, then a single TEXT line for its body,
        # and no [ELEM] line in between that script and its TEXT.
        i = t.find('[ELEM] "script"')
        if i < 0: return False
        # Slice forward to the next [TEXT]
        j = t.find('[TEXT]', i)
        if j < 0: return False
        # Between those there must be no [ELEM] at deeper indent
        between = t[i + len('[ELEM] "script"'):j]
        return '[ELEM]' not in between
    need("script-no-inner", script_no_inner)

    # --- final paragraph: numeric/hex entity decoding ---
    need("dollar-decoded", lambda t: 'price: $' in t)

    # --- footer ---
    need("total",        lambda t: '[TOTAL]' in t and 'nodes' in t)

    print(f"--- captured {len(full)} bytes -> {LOG} ---")
    if fails:
        print("FAIL: missing/mismatched assertions:", ", ".join(fails),
              file=sys.stderr)
        idx = text.find("htmldom /mnt/test.html")
        if idx >= 0:
            print("---- output region ----", file=sys.stderr)
            print(text[idx:idx + 4096], file=sys.stderr)
        return 1
    print("PASS: DOM construction — all checks green")
    return 0


if __name__ == "__main__":
    sys.exit(main())

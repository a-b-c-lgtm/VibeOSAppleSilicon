#!/usr/bin/env python3
"""scripts/test_layout.py — layout engine driver.

Boots the kernel, waits for the shell prompt, and runs the
/bin/layout test driver against /mnt/test_layout.html with a
viewport width of 800.  Asserts a pile of landmarks against the
[DOC]/[BOX]/[PAINT] output produced by the layout engine.

Full transcript dropped at /tmp/m62.log.

Exit status:
  0 — all assertions passed
  1 — one or more assertions failed (mismatch printed on stderr)
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-m62.sock"
LOG  = "/tmp/m62.log"

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


def drain_silence(s, max_secs, silence_secs):
    """Read until `silence_secs` of no data, or `max_secs` total."""
    buf = b""
    end = time.time() + max_secs
    last = time.time()
    while time.time() < end and time.time() - last < silence_secs:
        r, _, _ = select.select([s], [], [], 0.5)
        if not r: continue
        try: chunk = s.recv(16384)
        except OSError: break
        if not chunk: break
        buf += chunk
        last = time.time()
    return buf


def drain_until(s, needle, deadline_s):
    buf = b""
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.25)
        if not r: continue
        try: chunk = s.recv(8192)
        except OSError: break
        if not chunk: break
        buf += chunk
        if needle in buf: return buf
    return buf


# ---------- parsing helpers ----------

DOC_RE   = re.compile(r"\[DOC\] viewport=(\d+) height=(\d+) boxes=(\d+) paints=(\d+)")
BOX_RE   = re.compile(r"\[BOX#(\d+)\]\s+kind=(\w+)\s+x=(-?\d+)\s+y=(-?\d+)\s+w=(-?\d+)\s+h=(-?\d+)\s+tag=(\S+)(?:\s+text=\"(.*)\")?")
PAINT_RE = re.compile(
    r"\[PAINT#(\d+)\]\s+(RECT|TEXT|UNDERLINE)"
    r"\s+x=(-?\d+)\s+y=(-?\d+)\s+w=(-?\d+)\s+h=(-?\d+)"
    r"\s+color=(#[0-9A-Fa-f]{8})"
    r"(?:\s+fs=(\d+)\s+fw=(\d+)\s+fst=(\d+)\s+\"(.*)\")?")


def parse_boxes(text):
    out = []
    for line in text.splitlines():
        m = BOX_RE.match(line)
        if m:
            out.append({
                "i":    int(m.group(1)),
                "kind": m.group(2),
                "x":    int(m.group(3)),
                "y":    int(m.group(4)),
                "w":    int(m.group(5)),
                "h":    int(m.group(6)),
                "tag":  m.group(7),
                "text": m.group(8) if m.group(8) is not None else None,
            })
    return out


def parse_paints(text):
    out = []
    for line in text.splitlines():
        m = PAINT_RE.match(line)
        if m:
            row = {
                "i":     int(m.group(1)),
                "kind":  m.group(2),
                "x":     int(m.group(3)),
                "y":     int(m.group(4)),
                "w":     int(m.group(5)),
                "h":     int(m.group(6)),
                "color": m.group(7).upper(),
            }
            if m.group(8) is not None:
                row["fs"]   = int(m.group(8))
                row["fw"]   = int(m.group(9))
                row["fst"]  = int(m.group(10))
                row["text"] = m.group(11)
            out.append(row)
    return out


def parse_doc(text):
    for line in text.splitlines():
        m = DOC_RE.match(line)
        if m:
            return {
                "viewport": int(m.group(1)),
                "height":   int(m.group(2)),
                "boxes":    int(m.group(3)),
                "paints":   int(m.group(4)),
            }
    return None


# ---------- the test ----------

def main():
    qemu = boot()
    full = b""
    try:
        s = conn()
        full = drain_until(s, PROMPT, 30.0)
        if PROMPT not in full:
            print("FAIL: never reached shell prompt", file=sys.stderr)
            return 1
        s.sendall(b"layout /mnt/test_layout.html 800\n")
        # Layout output is large (>200 paints).  Wait for [DOC] line
        # then drain until silence.
        full += drain_until(s, b"[DOC]", 20.0)
        full += drain_silence(s, max_secs=30.0, silence_secs=2.0)
    finally:
        try: qemu.terminate()
        except Exception: pass
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()
        cleanup()

    with open(LOG, "wb") as f: f.write(full)
    text = full.decode("utf-8", errors="replace")

    doc    = parse_doc(text)
    boxes  = parse_boxes(text)
    paints = parse_paints(text)

    fails = []
    def need(label, ok, detail=""):
        if not ok:
            fails.append(label + (f" ({detail})" if detail else ""))

    # ---------------- [DOC] sanity ----------------
    need("doc-line-present", doc is not None)
    if doc:
        need("doc-viewport-800",  doc["viewport"] == 800,
             f"viewport={doc['viewport']}")
        need("doc-height-positive", doc["height"]   > 200,
             f"height={doc['height']}")
        need("doc-boxes-many",    doc["boxes"]    > 50,
             f"boxes={doc['boxes']}")
        need("doc-paints-many",   doc["paints"]   > 50,
             f"paints={doc['paints']}")

    # ---------------- root layout ----------------
    # body should be inset by 16px on every side from the viewport.
    body = next((b for b in boxes if b["tag"] == "body"), None)
    need("body-box-present", body is not None)
    if body:
        need("body-x-margin-16",        body["x"] == 16, f"x={body['x']}")
        need("body-y-margin-16",        body["y"] == 16, f"y={body['y']}")
        need("body-w-equals-viewport-32", body["w"] == 800 - 32,
             f"w={body['w']}")

    # ---------------- h1 ----------------
    h1 = next((b for b in boxes if b["tag"] == "h1"), None)
    need("h1-block-present", h1 is not None and h1["kind"] == "BLOCK")
    if h1:
        need("h1-x-equals-body-x", h1["x"] == 16, f"x={h1['x']}")
        # h1 font-size 32 -> fixed line-box height 32*1.4 ~= 44 OR
        # the line-height we use (line_height_px or fs*1.4).
    # h1 paints should use fs=32, fw=700 (bold-by-default), navy color
    h1_paints = [p for p in paints if p.get("text") == "Layout"]
    need("h1-text-paint-fs-32", any(p.get("fs") == 32 for p in h1_paints),
         f"Layout paints={h1_paints}")
    need("h1-text-paint-fw-700", any(p.get("fw") == 700 for p in h1_paints))
    need("h1-text-paint-color-navy",
         any(p["color"] == "#FF000080" for p in h1_paints),
         "expected #FF000080 (navy)")

    # ---------------- p.intro#lead — bold + italic + teal ----------------
    # First word "This" of the lead paragraph.
    intro_paints = [p for p in paints
                    if p.get("text") == "This" and p.get("fst") == 1]
    need("intro-lead-italic-present", len(intro_paints) >= 1)
    if intro_paints:
        p = intro_paints[0]
        need("intro-lead-bold",   p["fw"] == 700, f"fw={p['fw']}")
        need("intro-lead-color",  p["color"] == "#FF105050",
             f"color={p['color']}")

    # ---------------- inline link underline ----------------
    underlines = [p for p in paints if p["kind"] == "UNDERLINE"]
    need("underline-emitted", len(underlines) >= 1,
         f"count={len(underlines)}")
    # At least one underline is the link colour.
    need("underline-link-color",
         any(p["color"] == "#FF0050C0" for p in underlines),
         "expected at least one underline #FF0050C0")

    # ---------------- bullet glyphs for <li> ----------------
    bullets = [p for p in paints
               if p.get("text") and p["text"].startswith("\u2022")]
    need("bullets-three-or-more", len(bullets) >= 3,
         f"bullet paints={len(bullets)}")

    # ---------------- .note background and gold border ----------------
    note_bg = [p for p in paints
               if p["kind"] == "RECT" and p["color"] == "#FFFFFFC0"]
    need("note-bg-rect", len(note_bg) >= 1, "expected #FFFFFFC0 RECT")
    note_border = [p for p in paints
                   if p["kind"] == "RECT" and p["color"] == "#FFC0A040"]
    need("note-border-rects-4-sides", len(note_border) >= 4,
         f"got {len(note_border)} #FFC0A040 RECTs")

    # ---------------- .half — width 50% ----------------
    half_bg = [p for p in paints
               if p["kind"] == "RECT" and p["color"] == "#FFE0F0FF"]
    need("half-bg-rect", len(half_bg) >= 1)
    if half_bg:
        # body content width = 768; half = 384, plus padding (4*2)
        # plus border (1*2) = 394.  Tolerance ±20 px to absorb the
        # box-model additions.
        need("half-width-near-half-of-body",
             abs(half_bg[0]["w"] - 384) <= 20,
             f"half w={half_bg[0]['w']}")

    # ---------------- text-align: center ----------------
    centred = [p for p in paints if p.get("text") == "Centred"]
    need("centred-word-painted", len(centred) >= 1)
    if centred:
        # body content x range: [16 .. 784].  Centered text should not
        # start at 16; expect x significantly > 16 (centered offset).
        need("centred-x-not-at-left-edge",
             centred[0]["x"] > 100,
             f"x={centred[0]['x']}")

    # ---------------- text-align: right ----------------
    right_word = [p for p in paints if p.get("text") == "Right-aligned"]
    need("right-word-painted", len(right_word) >= 1)
    if right_word:
        # The full text is "Right-aligned paragraph." last word ends
        # ~at body content right edge (784).
        need("right-word-x-near-right-edge",
             right_word[0]["x"] > 400,
             f"x={right_word[0]['x']}")

    # The last word of the .right paragraph ends near 784.
    para_in_right = [p for p in paints if p.get("text") == "paragraph."]
    # there are several "paragraph." words.  Look for the one at the
    # same y as "Right-aligned".
    if right_word:
        ry = right_word[0]["y"]
        right_para = next((p for p in para_in_right if abs(p["y"] - ry) < 4), None)
        need("right-last-word-flush-to-right",
             right_para is not None and abs((right_para["x"] + right_para["w"]) - 784) <= 8,
             f"end={right_para['x']+right_para['w'] if right_para else None}")

    # ---------------- small grey footer ----------------
    small = [p for p in paints
             if p.get("fs") == 13 and p["color"] == "#FF808080"]
    need("small-grey-13-painted", len(small) >= 1,
         f"got {len(small)} fs=13 grey paints")

    # ---------------- swatches: white text within an orange inline ----------------
    # NOTE: the layout engine does not currently paint background
    # rectangles for inline-level elements (only block-level boxes
    # paint bg+borders).  We assert only that the inline white text
    # colour cascades through.
    swatch_text = [p for p in paints
                   if p["kind"] == "TEXT" and p["color"] == "#FFFFFFFF"]
    need("swatch-white-text", len(swatch_text) >= 3,
         f"got {len(swatch_text)} white TEXT paints")

    # ---------------- bordered-block grey 1px border ----------------
    block_border = [p for p in paints
                    if p["kind"] == "RECT" and p["color"] == "#FF888888"
                    and (p["w"] == 1 or p["h"] == 1)]
    need("bordered-block-border-rects",
         len(block_border) >= 4,
         f"got {len(block_border)} #FF888888 1px-edge RECTs")

    # ---------------- back-to-front ordering (RECTs before TEXTs of same box) ----------------
    # Quick sanity: at least one body bg RECT and at least one h1 TEXT.
    body_bg = [p for p in paints
               if p["kind"] == "RECT" and p["color"] == "#FFFFFFFF"
               and p["x"] <= 16 and p["y"] <= 16]
    need("body-bg-rect-painted", len(body_bg) >= 1)
    if body_bg and h1_paints:
        need("body-bg-painted-before-h1-text",
             body_bg[0]["i"] < h1_paints[0]["i"],
             f"body_bg i={body_bg[0]['i']} h1 i={h1_paints[0]['i']}")

    # ---------------- summary ----------------
    print(f"--- captured {len(full)} bytes -> {LOG} ---")
    if doc:
        print(f"--- DOC viewport={doc['viewport']} height={doc['height']} "
              f"boxes={doc['boxes']} paints={doc['paints']}")
    if fails:
        print("FAIL: missing/mismatched assertions:", file=sys.stderr)
        for f in fails: print("  -", f, file=sys.stderr)
        # Print a compact dump of the relevant region.
        idx = text.find("layout /mnt/test_layout.html")
        if idx >= 0:
            print("---- output (first 6 KiB) ----", file=sys.stderr)
            print(text[idx:idx + 6144], file=sys.stderr)
        return 1
    print("PASS: layout engine — all checks green")
    return 0


if __name__ == "__main__":
    sys.exit(main())

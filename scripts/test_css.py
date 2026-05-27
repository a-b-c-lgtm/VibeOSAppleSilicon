#!/usr/bin/env python3
"""scripts/test_css.py — CSS parser driver.

Boot the kernel, wait for the shell prompt, run two cssparse
invocations:

  1. `cssparse /mnt/test.css` — parse-only.  Assert that the rule
     count and key selector / declaration landmarks are present.
  2. `cssparse /mnt/test.css /mnt/test.html` — parse + match.
     Assert that the right rules apply to the right elements.

The fixture lives at assets/osfs/test.css; the matching fixture
HTML is the same assets/osfs/test.html that the tokenizer and DOM
tests already used.

Full transcript dropped at /tmp/m61.log.

Exit status:
  0 — all assertions passed
  1 — one or more assertions failed (mismatch printed on stderr)
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-m61.sock"
LOG  = "/tmp/m61.log"

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

        # --- Phase 1: parse-only ---
        s.sendall(b"cssparse /mnt/test.css\n")
        full += drain_until(s, b"[STYLESHEET]", 10.0)
        full += drain_until(s, PROMPT, 5.0)

        # --- Phase 2: parse + match ---
        s.sendall(b"cssparse /mnt/test.css /mnt/test.html\n")
        full += drain_until(s, b"[STYLESHEET]", 10.0)
        full += drain_until(s, PROMPT, 10.0)
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

    # --- Phase 1: parse-only landmarks ---
    # We expect 11 rules from the fixture (the @media block is skipped):
    # 0:* 1:body 2:h1 3:body p 4:ul li 5:ul>li
    # 6:p.intro 7:p.intro#lead 8:#lead 9:.note,.warn 10:script
    need("stylesheet-line",
         lambda t: '[STYLESHEET] 11 rules,' in t)
    need("rule-universal",
         lambda t: '[RULE 0] selectors=1 specificity=0' in t and '[SEL] *' in t)
    need("rule-body",
         lambda t: '[RULE 1] selectors=1 specificity=1' in t and '[SEL] body' in t)
    need("decl-background-white",
         lambda t: '[DECL] background: "white"' in t)
    need("rule-h1-color",
         lambda t: '[DECL] color: "navy"' in t)
    need("rule-descendant",
         lambda t: '[SEL] body p' in t)
    need("rule-child",
         lambda t: '[SEL] ul > li' in t)
    need("rule-compound",
         lambda t: '[SEL] p.intro' in t and '[DECL] font-weight: "bold"' in t)
    need("rule-id-only",
         lambda t: '[SEL] #lead' in t and '[DECL] text-decoration: "underline"' in t)
    need("rule-comma-list",
         lambda t: '[SEL] .note' in t and '[SEL] .warn' in t and
                   '[DECL] border: "1px solid red"' in t)
    need("specificity-id",
         # #lead = 10000; p.intro#lead = 10101; pick a rule we know:
         lambda t: 'specificity=10000' in t)
    need("specificity-id-class-type",
         lambda t: 'specificity=10101' in t)
    # @media skipped: pink background must NOT appear in any [DECL] line
    need("at-rule-skipped",
         lambda t: '[DECL] background: "pink"' not in t)
    need("after-at-rule-still-parses",
         lambda t: '[SEL] script' in t and '[DECL] display: "none"' in t)

    # --- Phase 2: matcher landmarks ---
    # Universal (rule 0) hits every element including <html>, <head>,
    # <body>, etc.
    need("match-universal-h1",
         lambda t: ('[MATCH] <h1>' in t or '[MATCH] <h1 ' in t)
                   and 'rules' in t)
    # body element matches: rule 0 (*), rule 1 (body)
    # We just check the body line includes both 0 and 1.
    def body_has_rules_0_1(t):
        for line in t.splitlines():
            if line.startswith('[MATCH] <body'):
                # rules listed as space-sep ints after "-> rules"
                _, _, rest = line.partition('-> rules')
                tokens = rest.split()
                return '0' in tokens and '1' in tokens
        return False
    need("match-body-includes-0-and-1", body_has_rules_0_1)

    # The first <p> in the fixture is class="intro" id="lead".  It
    # should match: 0 (*), 3 (body p), 6 (p.intro), 7 (p.intro#lead),
    # 8 (#lead).
    def p_intro_lead_matches(t):
        for line in t.splitlines():
            if line.startswith('[MATCH] <p ') and 'class="intro"' in line and 'id="lead"' in line:
                _, _, rest = line.partition('-> rules')
                toks = rest.split()
                # Must include all of 0, 3, 6, 7, 8.
                return all(x in toks for x in ['0', '3', '6', '7', '8'])
        return False
    need("match-p-intro-lead-all", p_intro_lead_matches)

    # The <li> elements should match rule 4 (ul li) AND rule 5 (ul>li).
    def li_matches_4_and_5(t):
        seen = 0
        for line in t.splitlines():
            if line.startswith('[MATCH] <li'):
                _, _, rest = line.partition('-> rules')
                toks = rest.split()
                if '4' in toks and '5' in toks: seen += 1
        return seen >= 2
    need("match-li-descendant-and-child", li_matches_4_and_5)

    # The <script> element should match rule 10 (script).
    need("match-script-rule-10",
         lambda t: any(line.startswith('[MATCH] <script')
                       and '10' in line.partition('-> rules')[2].split()
                       for line in t.splitlines()))

    print(f"--- captured {len(full)} bytes -> {LOG} ---")
    if fails:
        print("FAIL: missing/mismatched assertions:", ", ".join(fails),
              file=sys.stderr)
        idx = text.find("cssparse /mnt/test.css")
        if idx >= 0:
            print("---- output region ----", file=sys.stderr)
            print(text[idx:idx + 8192], file=sys.stderr)
        return 1
    print("PASS: CSS parser + matcher — all checks green")
    return 0


if __name__ == "__main__":
    sys.exit(main())

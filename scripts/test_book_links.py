#!/usr/bin/env python3
# scripts/test_book_links.py
# ─────────────────────────────────────────────────────────────────────────────
# Unit test for book/preprocessors/github_links.py.
# Run: python3 scripts/test_book_links.py
# Exits 0 if every rewrite matches, 1 otherwise.
# ─────────────────────────────────────────────────────────────────────────────
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "book", "preprocessors"))
import github_links as gl

base = "https://github.com/a-b-c-lgtm/VibeOSAppleSilicon/blob/main"
raw  = "https://github.com/a-b-c-lgtm/VibeOSAppleSilicon/raw/main"

# (src_path_relative_to_book, input markdown, expected output)
CASES = [
    # Source-code refs that escape book/ must rewrite to /blob/.
    ("chapters/01-foundations/003-first-boot.md",
     "[boot.s](../../../kernel/arch/boot.s)",
     f"[boot.s]({base}/kernel/arch/boot.s)"),
    ("chapters/01-foundations/003-first-boot.md",
     "[Makefile](../../../Makefile)",
     f"[Makefile]({base}/Makefile)"),
    # Line anchors preserved.
    ("chapters/01-foundations/003-first-boot.md",
     "[serial.c](../../../kernel/core/serial.c#L42)",
     f"[serial.c]({base}/kernel/core/serial.c#L42)"),
    ("chapters/01-foundations/003-first-boot.md",
     "[serial.c](../../../kernel/core/serial.c#L42-L67)",
     f"[serial.c]({base}/kernel/core/serial.c#L42-L67)"),
    # Within-book cross-chapter link must be left alone (mdbook
    # will convert .md to .html).
    ("chapters/12-system-services/103-guard-pages.md",
     "[Chapter 26](../05-devices/026-argc-argv.md)",
     "[Chapter 26](../05-devices/026-argc-argv.md)"),
    ("chapters/01-foundations/001-why-apple-silicon.md",
     "[book index](../../INDEX.md)",
     "[book index](../../INDEX.md)"),
    # Already-absolute https URL must be left alone.
    ("chapters/01-foundations/001-why-apple-silicon.md",
     "[github](https://github.com/foo/bar)",
     "[github](https://github.com/foo/bar)"),
    # Image link to an asset outside book/ should use /raw/ so the
    # browser actually fetches the bytes.
    ("chapters/12-system-services/097-virtio-snd.md",
     "![chime](../../../assets/screenshots/chime.png)",
     f"![chime]({raw}/assets/screenshots/chime.png)"),
    # INDEX.md itself: chapters/* paths stay relative.
    ("INDEX.md",
     "[Chapter 3](chapters/01-foundations/003-first-boot.md)",
     "[Chapter 3](chapters/01-foundations/003-first-boot.md)"),
    # Pure anchor / fragment-only link is untouched.
    ("chapters/01-foundations/003-first-boot.md",
     "[jump](#the-pl011-uart)",
     "[jump](#the-pl011-uart)"),
    # mailto / non-http scheme untouched.
    ("chapters/01-foundations/001-why-apple-silicon.md",
     "[mail](mailto:foo@bar)",
     "[mail](mailto:foo@bar)"),
    # Backticked link text preserved verbatim inside the rewritten link.
    ("chapters/13-tcp-server/111-end-to-end-loop.md",
     "[`userspace/init/init.c`](../../../userspace/init/init.c)",
     f"[`userspace/init/init.c`]({base}/userspace/init/init.c)"),
]

ok = bad = 0
for src, inp, expected in CASES:
    got = gl.rewrite_content(inp, src, base, raw)
    if got == expected:
        ok += 1
        print(f"PASS  {src}: {inp}")
    else:
        bad += 1
        print(f"FAIL  {src}:")
        print(f"  input    {inp}")
        print(f"  expected {expected}")
        print(f"  got      {got}")

print(f"\n{ok} pass / {bad} fail")
sys.exit(1 if bad else 0)

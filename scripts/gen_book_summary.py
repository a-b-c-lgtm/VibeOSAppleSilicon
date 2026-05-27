#!/usr/bin/env python3
# scripts/gen_book_summary.py
# ─────────────────────────────────────────────────────────────────────────────
# Regenerate book/SUMMARY.md from book/INDEX.md.
#
# INDEX.md is the human-readable table of contents and project
# status doc.  mdBook needs a strict SUMMARY.md that lists the
# parts and chapters in nav order.  Rather than maintain two
# parallel files we parse INDEX.md once per build.
#
# Rules:
#   * ### Part X — Title              -> `# Part X — Title` separator
#   * ordered/unordered list items whose first markdown link
#     resolves to a chapters/*.md file become SUMMARY entries
#   * indent depth determines nesting (top-level = no leading
#     spaces, sub-items = 5+ leading spaces OR `   - ` markers)
#   * items without a chapters/* link are skipped (they're prose
#     placeholders like "Higher-half kernel — *deferred*")
#   * stops parsing at the first H2 that is not "## Parts"
#     (i.e. Appendices, Dependency map, Project status).
# ─────────────────────────────────────────────────────────────────────────────
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INDEX = os.path.join(ROOT, "book", "INDEX.md")
SUMMARY = os.path.join(ROOT, "book", "SUMMARY.md")

PART_RE  = re.compile(r"^###\s+Part\s+([IVXLC]+)\s+[—\-]\s+(.+?)\s*$")
LINK_RE  = re.compile(r"\[([^\]]+)\]\((chapters/[^)#]+\.md)(#[^)]*)?\)")
ITEM_RE  = re.compile(r"^(?P<indent>\s*)(?P<marker>\d+\.|-)\s+(?P<rest>.+?)\s*$")
H2_RE    = re.compile(r"^##\s+(.+?)\s*$")

def parse_index(path):
    parts = []   # list of {"name": str, "items": [(depth, title, target)]}
    current = None
    in_parts = False
    saw_first_h2_parts = False
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line_rstrip = line.rstrip("\n")

            h2 = H2_RE.match(line_rstrip)
            if h2:
                if h2.group(1).strip() == "Parts":
                    in_parts = True
                    saw_first_h2_parts = True
                    continue
                if saw_first_h2_parts:
                    in_parts = False
                continue
            if not in_parts:
                continue

            m = PART_RE.match(line_rstrip)
            if m:
                current = {"name": f"Part {m.group(1)} — {m.group(2)}",
                           "items": []}
                parts.append(current)
                continue

            if current is None:
                continue

            it = ITEM_RE.match(line_rstrip)
            if not it:
                continue
            indent = len(it.group("indent").expandtabs(4))
            rest = it.group("rest")
            link = LINK_RE.search(rest)
            if not link:
                continue
            title = link.group(1).strip()
            target = link.group(2).strip()
            # Numbered top-level items have indent 0; the Doom block
            # uses 3-space indent + `-` for sub-items; the libc
            # 148-block uses 5-space indent + numbered. Anything
            # with indent > 0 is a sub-item.
            depth = 1 if indent > 0 else 0
            current["items"].append((depth, title, target))
    return parts

def render_summary(parts):
    out = []
    out.append("# Summary")
    out.append("")
    out.append("[Book Index](INDEX.md)")
    out.append("")
    for part in parts:
        out.append(f"# {part['name']}")
        out.append("")
        for depth, title, target in part["items"]:
            indent = "  " * depth
            # Strip backticks from titles; mdbook accepts them but
            # the navigation panel is plain-text-friendlier without.
            clean = title.replace("`", "")
            out.append(f"{indent}- [{clean}]({target})")
        out.append("")
    return "\n".join(out).rstrip() + "\n"

def main():
    if not os.path.exists(INDEX):
        print(f"error: {INDEX} not found", file=sys.stderr)
        sys.exit(1)
    parts = parse_index(INDEX)
    if not parts:
        print("error: parsed no parts from INDEX.md", file=sys.stderr)
        sys.exit(1)
    text = render_summary(parts)
    with open(SUMMARY, "w", encoding="utf-8") as fh:
        fh.write(text)
    total = sum(len(p["items"]) for p in parts)
    print(f"wrote {SUMMARY}: {len(parts)} parts, {total} chapters")

if __name__ == "__main__":
    main()

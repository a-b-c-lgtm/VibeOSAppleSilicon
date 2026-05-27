#!/usr/bin/env python3
# book/preprocessors/github_links.py
# ─────────────────────────────────────────────────────────────────────────────
# mdBook preprocessor.
#
# Every markdown link in the book that resolves to a file OUTSIDE
# `book/` (e.g. `../../../kernel/core/main.c`) is rewritten to its
# canonical absolute URL on GitHub:
#
#     https://github.com/<owner>/<repo>/blob/<branch>/<repo-rel-path>
#
# Line anchors (`#L42`, `#L42-L67`) are preserved.  Image links use
# `/raw/<branch>/...` so they actually render inside the book.
#
# Links that stay inside `book/` are left alone — mdBook converts
# the `.md` extension to `.html` itself, so chapter-to-chapter and
# INDEX-to-chapter navigation just works.
#
# Protocol:
#   * `... supports <renderer>`  → exit 0 to claim support
#   * no args                     → read [context, book] JSON on
#                                   stdin, write the modified book
#                                   JSON on stdout.
# ─────────────────────────────────────────────────────────────────────────────
from __future__ import annotations

import json
import posixpath
import re
import sys


# ── link rewriting ──────────────────────────────────────────────────────────
#
# Matches both inline links `[text](target)` and image links
# `![alt](target)`. Targets with whitespace, balanced parens, or
# title strings ("...") aren't used anywhere in this book, so a
# simple [^)] body is enough.
LINK_RE = re.compile(r"(?P<bang>!?)\[(?P<text>[^\]]*)\]\((?P<target>[^)\s]+)\)")

# Schemes / shapes that are absolute or fragment-only and should be
# left untouched.
ABSOLUTE_RE = re.compile(r"^(?:[a-z][a-z0-9+.\-]*:|//|#|mailto:)", re.IGNORECASE)


def repo_relative(src_path_in_book: str, target: str) -> str | None:
    """Resolve `target` relative to the chapter at `book/{src_path_in_book}`
    and return a repo-rooted POSIX path if the result escapes `book/`.
    Returns None if the resolved path stays inside `book/`.
    """
    # The chapter lives at book/<src_path_in_book>. Its containing
    # directory is what relative links resolve against.
    chapter_dir = posixpath.dirname(posixpath.join("book", src_path_in_book))
    resolved = posixpath.normpath(posixpath.join(chapter_dir, target))
    if resolved.startswith("../") or resolved == "..":
        # Escaped above the repo root entirely — leave it for the
        # user to fix; we shouldn't fabricate a github URL.
        return None
    if resolved == "book" or resolved.startswith("book/"):
        return None
    return resolved


def rewrite_one(match: re.Match, src_path: str, base: str, raw_base: str) -> str:
    target = match.group("target")
    if ABSOLUTE_RE.match(target):
        return match.group(0)

    # Split off the fragment / line anchor so we can preserve it.
    if "#" in target:
        path_part, frag = target.split("#", 1)
        anchor = f"#{frag}"
    else:
        path_part, anchor = target, ""

    if not path_part:
        # Pure-anchor link like `[foo](#bar)` — leave alone.
        return match.group(0)

    repo_rel = repo_relative(src_path, path_part)
    if repo_rel is None:
        # Intra-book link: mdBook handles `.md` → `.html` itself.
        return match.group(0)

    bang = match.group("bang")
    # Images need /raw/ so the bytes are served inline; ordinary
    # links want /blob/ so GitHub renders the source view.
    root = raw_base if bang else base
    new_target = f"{root}/{repo_rel}{anchor}"
    return f"{bang}[{match.group('text')}]({new_target})"


def rewrite_content(content: str, src_path: str, base: str, raw_base: str) -> str:
    return LINK_RE.sub(
        lambda m: rewrite_one(m, src_path, base, raw_base),
        content,
    )


# ── mdBook book-walk ────────────────────────────────────────────────────────
def walk_items(items, base, raw_base):
    for item in items:
        if not isinstance(item, dict):
            continue
        ch = item.get("Chapter")
        if ch is None:
            continue
        src_path = ch.get("path")
        if src_path:
            ch["content"] = rewrite_content(
                ch.get("content", ""), src_path, base, raw_base
            )
        sub_items = ch.get("sub_items") or []
        if sub_items:
            walk_items(sub_items, base, raw_base)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "supports":
        # We support every renderer; the rewrites are pure markdown.
        sys.exit(0)

    raw = sys.stdin.read()
    context, book = json.loads(raw)

    cfg = (
        context.get("config", {})
               .get("preprocessor", {})
               .get("github-links", {})
    )
    owner  = cfg.get("owner",  "OWNER")
    repo   = cfg.get("repo",   "REPO")
    branch = cfg.get("branch", "main")
    base     = f"https://github.com/{owner}/{repo}/blob/{branch}"
    raw_base = f"https://github.com/{owner}/{repo}/raw/{branch}"

    walk_items(book.get("items", []), base, raw_base)

    json.dump(book, sys.stdout)


if __name__ == "__main__":
    main()

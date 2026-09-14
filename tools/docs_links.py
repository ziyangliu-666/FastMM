#!/usr/bin/env python3
"""Offline check of relative links and anchors in the docs.

For every Markdown file under docs/ and the top-level README.md, CONTRIBUTING.md and SECURITY.md,
each inline link, image and reference definition whose target is not a URL must name an existing
file or directory (relative to the page), and a `#fragment` must match a heading of the target
Markdown page (GitHub's anchor rules: lower case, punctuation removed, spaces become hyphens,
repeated headings get -1, -2, ...) or an explicit `<a id="...">`. Links inside code spans and
fenced code blocks are ignored. Nothing is fetched from the network.

usage:
  python3 tools/docs_links.py --check [files...]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from urllib.parse import unquote

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs_md import ROOT, markdown_files, rel  # noqa: E402

FENCE = re.compile(r"^\s*(```+|~~~+)")
INLINE_CODE = re.compile(r"(`+)(?:(?!\1).)+?\1")
# [text](target "title") and ![alt](target); the target may be wrapped in <>.
INLINE_LINK = re.compile(r"!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*(<[^>]*>|[^)\s]+)(?:\s+\"[^\"]*\")?\s*\)")
REF_DEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>]*>|\S+)")
HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
HTML_ID = re.compile(r"<a\s+(?:name|id)=\"([^\"]+)\"")
SCHEME = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")


def strip_code(lines: list[str]) -> list[str]:
    """The lines with fenced blocks blanked and inline code spans removed."""
    out = []
    fence = None
    for ln in lines:
        m = FENCE.match(ln)
        if fence:
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence):
                fence = None
            out.append("")
            continue
        if m:
            fence = m.group(1)
            out.append("")
            continue
        out.append(INLINE_CODE.sub("", ln))
    return out


def github_slug(heading: str) -> str:
    text = re.sub(r"<[^>]+>", "", heading)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # links keep their text
    text = text.replace("`", "")
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


_anchor_cache: dict[Path, set[str]] = {}


def anchors(page: Path) -> set[str]:
    if page in _anchor_cache:
        return _anchor_cache[page]
    seen: dict[str, int] = {}
    result: set[str] = set()
    fence = None
    for ln in page.read_text(encoding="utf-8").splitlines():
        m = FENCE.match(ln)
        if fence:
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence):
                fence = None
            continue
        if m:
            fence = m.group(1)
            continue
        for a in HTML_ID.findall(ln):
            result.add(a)
        h = HEADING.match(ln)
        if not h:
            continue
        slug = github_slug(h.group(2))
        n = seen.get(slug, 0)
        seen[slug] = n + 1
        result.add(slug if n == 0 else f"{slug}-{n}")
    _anchor_cache[page] = result
    return result


def check_file(page: Path) -> list[str]:
    errors = []
    lines = page.read_text(encoding="utf-8").splitlines()
    for lineno, ln in enumerate(strip_code(lines), start=1):
        targets = [m.group(1) for m in INLINE_LINK.finditer(ln)]
        d = REF_DEF.match(ln)
        if d:
            targets.append(d.group(1))
        for raw in targets:
            target = raw[1:-1] if raw.startswith("<") else raw
            if not target or SCHEME.match(target):
                continue
            path_part, _, fragment = target.partition("#")
            path_part = unquote(path_part)
            dest = page if not path_part else (page.parent / path_part).resolve()
            where = f"{rel(page)}:{lineno}"
            if not dest.exists():
                errors.append(f"{where}: {target}: {path_part} does not exist")
                continue
            if ROOT not in dest.parents and dest != ROOT:
                errors.append(f"{where}: {target}: points outside the repository")
                continue
            if fragment and dest.is_file() and dest.suffix == ".md":
                if unquote(fragment) not in anchors(dest):
                    errors.append(f"{where}: {target}: no heading '#{fragment}' in {rel(dest)}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="exit 1 on a broken link (the only mode)")
    ap.add_argument("files", nargs="*", type=Path, help="Markdown files (default: docs/ and the top-level pages)")
    args = ap.parse_args()
    files = [p.resolve() for p in args.files] or markdown_files()
    errors: list[str] = []
    for p in files:
        errors.extend(check_file(p))
    for e in errors:
        print(e, file=sys.stderr)
    print(f"docs_links: {len(files)} file(s), {len(errors)} broken link(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

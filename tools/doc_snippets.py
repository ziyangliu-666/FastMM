#!/usr/bin/env python3
"""Keeps code blocks in the docs identical to the source files they quote.

A Markdown page marks a fenced block with the file and region it shows; paths are relative to the
repository root, and without `#region` the whole file is shown:

    <!-- snippet: examples/quickstart/my_mm.hpp#strategy -->
    ```cpp
    ...
    ```

The source marks the region with comment lines, which are left out of the snippet (as are the
markers of other regions nested inside it):

    // [start:strategy]            C++, CMake-free sources
    // [end:strategy]
    # --8<-- [start:backtest]      shell, TOML, Python, CMake
    # --8<-- [end:backtest]

The common indentation of the region is removed. A marker without a fenced block below it gets one.

usage:
  python3 tools/doc_snippets.py           rewrite every stale block
  python3 tools/doc_snippets.py --check   exit 1 if a block differs from its source (CI)
"""
from __future__ import annotations

import argparse
import re
import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs_md import ROOT, fence_language, finish, markdown_files, rel  # noqa: E402

MARKER = re.compile(r"^<!-- snippet: (?P<path>[^#\s]+)(?:#(?P<region>[\w.-]+))? -->\s*$")
FENCE = re.compile(r"^(?P<fence>```+|~~~+)(?P<info>.*)$")
REGION_LINE = re.compile(r"(?://|#)\s*(?:--8<--\s*)?\[(?P<kind>start|end):(?P<name>[\w.-]+)\]\s*$")


class SnippetError(Exception):
    pass


def extract(path: str, region: str | None) -> str:
    src = ROOT / path
    if not src.is_file():
        raise SnippetError(f"{path}: no such file")
    lines = src.read_text(encoding="utf-8").splitlines()
    if region is None:
        body = [ln for ln in lines if not REGION_LINE.search(ln)]
    else:
        body = []
        inside = False
        found = False
        for ln in lines:
            m = REGION_LINE.search(ln)
            if m and m.group("name") == region:
                if m.group("kind") == "start":
                    if found:
                        raise SnippetError(f"{path}: region '{region}' starts twice")
                    inside = found = True
                else:
                    if not inside:
                        raise SnippetError(f"{path}: region '{region}' ends before it starts")
                    inside = False
                continue
            if inside and not m:
                body.append(ln)
        if not found:
            raise SnippetError(f"{path}: no region '{region}'")
        if inside:
            raise SnippetError(f"{path}: region '{region}' is not closed")
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    if not body:
        raise SnippetError(f"{path}#{region}: the region is empty")
    return textwrap.dedent("\n".join(body)) + "\n"


def process(text: str, where: str) -> tuple[str, int]:
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    count = 0
    open_fence: str | None = None  # markers inside a code block are examples, not snippets
    i = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        i += 1
        f = FENCE.match(line.strip())
        if open_fence is not None:
            if f and f.group("fence")[0] == open_fence[0] and len(f.group("fence")) >= len(open_fence) \
                    and not f.group("info").strip():
                open_fence = None
            continue
        if f:
            open_fence = f.group("fence")
            continue
        m = MARKER.match(line.rstrip("\n"))
        if not m:
            continue
        count += 1
        path, region = m.group("path"), m.group("region")
        try:
            body = extract(path, region)
        except SnippetError as e:
            raise SnippetError(f"{where}:{i}: {e}") from None
        fence, info = "```", fence_language(path)
        if i < len(lines):
            f = FENCE.match(lines[i].rstrip("\n"))
            if f:
                fence, info = f.group("fence"), f.group("info") or info
                j = i + 1
                while j < len(lines) and lines[j].rstrip("\n") != fence:
                    j += 1
                if j == len(lines):
                    raise SnippetError(f"{where}:{i + 1}: the code block after the snippet marker is not closed")
                i = j + 1
        if fence.startswith("`") and "```" in body:
            fence = "````"
        out.append(f"{fence}{info}\n{body}{fence}\n")
    return "".join(out), count


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if a snippet is stale instead of rewriting it")
    ap.add_argument("files", nargs="*", type=Path, help="Markdown files (default: docs/ and the top-level pages)")
    args = ap.parse_args()
    files = [p.resolve() for p in args.files] or markdown_files()
    changed: list[Path] = []
    total = 0
    try:
        for p in files:
            text = p.read_text(encoding="utf-8")
            new, count = process(text, rel(p))
            total += count
            if new != text:
                changed.append(p)
                if not args.check:
                    p.write_text(new, encoding="utf-8")
    except SnippetError as e:
        print(f"doc_snippets: error: {e}", file=sys.stderr)
        return 2
    print(f"doc_snippets: {total} snippet(s) in {len(files)} file(s)")
    return finish(changed, args.check, "doc_snippets", "python3 tools/doc_snippets.py")


if __name__ == "__main__":
    sys.exit(main())

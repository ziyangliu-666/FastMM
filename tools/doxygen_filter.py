#!/usr/bin/env python3
"""Doxygen INPUT_FILTER: presents FastMM's `//` comments to Doxygen as documentation.

The headers document themselves with ordinary `//` comments, which Doxygen ignores. This filter
rewrites a header on its way into Doxygen (the files on disk are untouched):

  * the comment block at the top of a file, before any code, becomes a `@file` block;
  * a comment line on its own becomes `///`, documenting whatever follows;
  * a comment after code on the same line becomes `///<`, documenting what precedes it;
  * snippet region markers (`// [start:name]`), section rules (`// ---- market data ----`) and
    tool directives (`// NOLINT`, `// clang-format off`) are dropped.

`//` inside a string or character literal, and `://` inside a URL, are left alone.

The comments are prose, not Doxygen markup, so outside `` `code spans` `` the characters Doxygen
would read as markup (`@`, `#`, `<`, `>`, `&`, `%`, `--`) are escaped. Without that, a placeholder
like `/dev/shm/fastmm-<engine>.status` disappears from the page as an unknown HTML tag.

usage (Doxyfile):
  INPUT_FILTER = "python3 tools/doxygen_filter.py"
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

# Lines that are directives for other tools, not prose.
DIRECTIVE = re.compile(
    r"^\s*(?:\[(?:start|end):[\w.-]+\]|--8<--|NOLINT|clang-format\b|NOLINTNEXTLINE|NOLINTBEGIN|NOLINTEND)"
)
# A rule between sections of a header, not a sentence about the next declaration.
SEPARATOR = re.compile(r"^\s*[-=~_*]{3,}")
# A line that only opens a scope or is otherwise not a declaration carries no trailing doc.
NO_TRAILING_DOC = re.compile(r"^\s*(?:#|\}|//)")
CODE_SPAN = re.compile(r"`[^`]*`")
MARKUP = re.compile(r"\\|[@#<>&%]|--")


def escape(body: str) -> str:
    """Doxygen markup characters escaped outside code spans, which Doxygen already takes as is."""
    out, pos = [], 0
    for span in CODE_SPAN.finditer(body):
        out.append(MARKUP.sub(lambda m: "\\" + m.group(0), body[pos : span.start()]))
        out.append(span.group(0))
        pos = span.end()
    out.append(MARKUP.sub(lambda m: "\\" + m.group(0), body[pos:]))
    return "".join(out)


def comment_start(line: str) -> int:
    """The index of the `//` that starts a comment, or -1. Skips literals and `://`."""
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c in "\"'":
            quote, i = c, i + 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    break
                i += 1
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            if i and line[i - 1] == ":":  # https://...
                i += 2
                continue
            return i
        i += 1
    return -1


def convert(text: str) -> str:
    lines = text.splitlines()
    out: list[str] = []
    i = 0

    # Leading block: `#pragma once`, blank lines and comments, before the first line of code.
    while i < len(lines) and (not lines[i].strip() or lines[i].strip() == "#pragma once"):
        out.append(lines[i])
        i += 1
    head: list[str] = []
    while i < len(lines) and lines[i].lstrip().startswith("//"):
        body = lines[i].lstrip()[2:]
        if not DIRECTIVE.match(body) and not SEPARATOR.match(body):
            head.append(escape(body.rstrip()))
        i += 1
    if head:
        out.append("/** @file")
        out.extend(" *" + ln for ln in head)
        out.append(" */")

    for line in lines[i:]:
        start = comment_start(line)
        if start < 0:
            out.append(line)
            continue
        code, body = line[:start], line[start + 2 :]
        if body.startswith("/") or body.startswith("!"):  # already a Doxygen comment
            out.append(line)
            continue
        if DIRECTIVE.match(body) or SEPARATOR.match(body):
            if code.strip():
                out.append(code.rstrip())
            continue
        if not code.strip():
            out.append(f"{code}///{escape(body)}")
        elif NO_TRAILING_DOC.match(code):
            out.append(code.rstrip())
        else:
            out.append(f"{code}///<{escape(body)}")
    return "\n".join(out) + "\n"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    src = Path(sys.argv[1])
    sys.stdout.write(convert(src.read_text(encoding="utf-8", errors="replace")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

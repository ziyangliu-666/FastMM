"""Shared helpers of the docs tools (doc_snippets.py, docs_links.py, docs_cli_help.py and
docs_config_ref.py): the Markdown files they read and the generated regions they rewrite.

A generated region is a pair of HTML comments around content that a tool owns:

    <!-- BEGIN <kind> <argument> -->
    ... generated ...
    <!-- END <kind> -->

GitHub does not render the comments, so the page shows only the generated content.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Callable, Iterable

ROOT = Path(__file__).resolve().parent.parent


def markdown_files(extra: Iterable[str] = ()) -> list[Path]:
    """Every Markdown file under docs/, then README.md, CONTRIBUTING.md and SECURITY.md."""
    files = sorted((ROOT / "docs").rglob("*.md"))
    for name in ("README.md", "CONTRIBUTING.md", "SECURITY.md", *extra):
        p = ROOT / name
        if p.is_file():
            files.append(p)
    return files


def rel(p: Path) -> str:
    try:
        return str(p.resolve().relative_to(ROOT))
    except ValueError:
        return str(p)


def fence_language(path: str) -> str:
    name = Path(path).name
    if name == "CMakeLists.txt":
        return "cmake"
    return {
        ".cpp": "cpp",
        ".hpp": "cpp",
        ".h": "cpp",
        ".sh": "bash",
        ".toml": "toml",
        ".py": "python",
        ".cmake": "cmake",
        ".json": "json",
    }.get(Path(path).suffix, "text")


def rewrite_regions(
    text: str, kind: str, render: Callable[[str], str], where: str
) -> tuple[str, list[str]]:
    """Replaces the content of every `<!-- BEGIN kind arg -->` ... `<!-- END kind -->` region with
    render(arg). Returns the new text and the arguments seen. Raises ValueError for a region
    without its END line."""
    begin = re.compile(r"^<!-- BEGIN " + re.escape(kind) + r" (.+?) -->\s*$")
    end = re.compile(r"^<!-- END " + re.escape(kind) + r" -->\s*$")
    out: list[str] = []
    args: list[str] = []
    lines = text.splitlines(keepends=True)
    i = 0
    while i < len(lines):
        m = begin.match(lines[i])
        out.append(lines[i])
        i += 1
        if not m:
            continue
        arg = m.group(1).strip()
        args.append(arg)
        j = i
        while j < len(lines) and not end.match(lines[j]):
            j += 1
        if j == len(lines):
            raise ValueError(f"{where}: <!-- BEGIN {kind} {arg} --> has no <!-- END {kind} -->")
        body = render(arg)
        if body and not body.endswith("\n"):
            body += "\n"
        out.append(body)
        out.append(lines[j])
        i = j + 1
    return "".join(out), args


def finish(changed: list[Path], check: bool, tool: str, hint: str) -> int:
    """Reports the files a tool changed (or, with --check, would change); returns the exit code."""
    if not changed:
        print(f"{tool}: up to date")
        return 0
    for p in changed:
        print(f"{tool}: {'stale' if check else 'updated'}: {rel(p)}")
    if check:
        print(f"{tool}: run `{hint}` and commit the result", file=sys.stderr)
        return 1
    return 0

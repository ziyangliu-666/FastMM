#!/usr/bin/env python3
"""Writes the dark variant of each diagram under docs/assets/diagrams/.

<name>.svg is the source; <name>-dark.svg is the same drawing with the colours of its <style>
block swapped for the dark palette below. Pages show them with `#only-light` and `#only-dark`.

usage:
  python3 tools/docs_diagrams.py           write every <name>-dark.svg
  python3 tools/docs_diagrams.py --check   fail when one is missing or out of date
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

DIR = Path(__file__).resolve().parent.parent / "docs" / "assets" / "diagrams"

# light colour -> dark colour, by role
DARK = {
    "#ffffff": "#12171d",  # background, box fill, text on header bars
    "#1b1f24": "#e6e9ed",  # ink: lines, header bars, labels
    "#5b636c": "#9aa3ad",  # muted text
    "#f3f4f6": "#1a2028",  # process frame
    "#b9bfc6": "#3d4651",  # process frame outline
    "#eceef1": "#232a33",  # grey item
    "#fbe4e7": "#3a1a20",  # highlighted item
    "#cf1027": "#ff5c6c",  # red
    "#9aa2ab": "#6b7581",  # ring outline and arrowheads
    "#f4f5f7": "#1f262e",  # ring cell
    "#c8cdd3": "#3d4651",  # ring cell outline
    "#e2e5e9": "#2a313a",  # legend separator
}
STYLE = re.compile(r"<style>.*?</style>", re.S)
COLOUR = re.compile(r"#[0-9a-f]{6}\b")


def dark(svg: str, name: str) -> str:
    def swap(m: re.Match) -> str:
        def one(c: re.Match) -> str:
            if c.group(0) not in DARK:
                sys.exit(f"docs_diagrams: {name}: {c.group(0)} has no dark colour")
            return DARK[c.group(0)]
        return COLOUR.sub(one, m.group(0))

    out, n = STYLE.subn(swap, svg, count=1)
    if n != 1:
        sys.exit(f"docs_diagrams: {name}: no <style> block")
    return out


def main() -> int:
    check = "--check" in sys.argv[1:]
    stale = []
    for src in sorted(DIR.glob("*.svg")):
        if src.stem.endswith("-dark"):
            continue
        want = dark(src.read_text(), src.name)
        dst = src.with_name(src.stem + "-dark.svg")
        if dst.exists() and dst.read_text() == want:
            continue
        if check:
            stale.append(dst.name)
        else:
            dst.write_text(want)
            print(f"docs_diagrams: wrote {dst.relative_to(DIR.parent.parent.parent)}")
    if stale:
        print("docs_diagrams: out of date, run python3 tools/docs_diagrams.py: " + ", ".join(stale),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

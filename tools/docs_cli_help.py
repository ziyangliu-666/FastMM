#!/usr/bin/env python3
"""Generates the usage blocks of docs/reference/cli.md from the programs' --help output.

Each program has a region in the page, which this tool fills with `<program> --help`:

    <!-- BEGIN cli-help fastmm-live -->
    ```text
    usage: fastmm-live --config <file.toml> [options]
    ...
    ```
    <!-- END cli-help -->

Every program in PROGRAMS must have a region, and every region must name one of them.

usage:
  python3 tools/docs_cli_help.py [--bin build/release/bin]           rewrite the page
  python3 tools/docs_cli_help.py --check [--bin build/release/bin]   exit 1 if it is stale (CI)
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs_md import ROOT, finish, rel, rewrite_regions  # noqa: E402

PROGRAMS = ("fastmm-live", "fastmm-backtest", "fastmm-data", "fastmm-replay", "fastmm-sim-exchange", "fastmm-sim-itch", "fastmm-top")
PAGE = ROOT / "docs" / "reference" / "cli.md"


def help_text(bin_dir: Path, program: str) -> str:
    exe = bin_dir / program
    if not exe.is_file():
        raise SystemExit(f"docs_cli_help: {exe} not found (build the {program} target)")
    r = subprocess.run([str(exe), "--help"], capture_output=True, text=True, timeout=30, check=False)
    if r.returncode != 0:
        raise SystemExit(f"docs_cli_help: {program} --help exited with {r.returncode}: {r.stderr}")
    return r.stdout.rstrip("\n") + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if the page is stale instead of rewriting it")
    ap.add_argument("--bin", type=Path, default=ROOT / "build" / "release" / "bin", help="directory of the built programs")
    args = ap.parse_args()

    def render(program: str) -> str:
        if program not in PROGRAMS:
            raise SystemExit(f"docs_cli_help: {rel(PAGE)}: unknown program '{program}'")
        return "```text\n" + help_text(args.bin, program) + "```"

    text = PAGE.read_text(encoding="utf-8")
    try:
        new, seen = rewrite_regions(text, "cli-help", render, rel(PAGE))
    except ValueError as e:
        print(f"docs_cli_help: error: {e}", file=sys.stderr)
        return 2
    missing = [p for p in PROGRAMS if p not in seen]
    if missing:
        print(f"docs_cli_help: {rel(PAGE)} has no cli-help region for {', '.join(missing)}", file=sys.stderr)
        return 1
    changed = [PAGE] if new != text else []
    if changed and not args.check:
        PAGE.write_text(new, encoding="utf-8")
    return finish(changed, args.check, "docs_cli_help", "python3 tools/docs_cli_help.py --bin <build>/bin")


if __name__ == "__main__":
    sys.exit(main())

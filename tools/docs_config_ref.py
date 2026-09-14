#!/usr/bin/env python3
"""Generates the key tables of docs/reference/configuration.md from include/fastmm/config/schema.hpp.

The page holds hand-written text and one generated table per schema section:

    <!-- BEGIN config-keys engine -->
    | Key | Type | Required | Meaning |
    ...
    <!-- END config-keys -->

The argument is a schema section (`engine`, `venues.*`, `venues.*.fees`, `instruments[]`,
`strategy`, `risk`, `logging`); `venues.*:connector` selects the connector-specific (passthrough)
keys of `venues.*` and `venues.*` the others. Every schema key must appear in exactly one table,
except the free-form `*` entries (`[strategy.params]`, `[sim]`, `[backtest]`), which the page
describes by hand. The meaning column is the key's doc string.

usage:
  python3 tools/docs_config_ref.py           rewrite the page
  python3 tools/docs_config_ref.py --check   exit 1 if it is stale or a key is missing (CI)
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs_md import ROOT, finish, rel, rewrite_regions  # noqa: E402

SCHEMA = ROOT / "include" / "fastmm" / "config" / "schema.hpp"
PAGE = ROOT / "docs" / "reference" / "configuration.md"
TYPES = {
    "String": "string",
    "Int": "integer",
    "Float": "number",
    "Bool": "boolean",
    "IntArray": "integer array",
    "StringArray": "string array",
    "Table": "table",
    "Any": "any",
}
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/|[A-Za-z_][\w:]*|[{},]|\S', re.S)


@dataclass
class Key:
    section: str
    key: str
    type: str
    required: bool
    doc: str
    passthrough: bool


def parse_schema(text: str) -> list[Key]:
    start = text.find("kConfigSchema[] = {")
    if start < 0:
        raise SystemExit(f"docs_config_ref: {rel(SCHEMA)}: kConfigSchema not found")
    tokens = [t for t in TOKEN.findall(text, start + len("kConfigSchema[] = {")) if not t.startswith("/")]
    keys: list[Key] = []
    i = 0
    while i < len(tokens) and tokens[i] != "}":
        if tokens[i] == ",":
            i += 1
            continue
        if tokens[i] != "{":
            raise SystemExit(f"docs_config_ref: {rel(SCHEMA)}: unexpected token {tokens[i]!r}")
        fields: list[str] = []
        current = ""
        i += 1
        while tokens[i] != "}":
            t = tokens[i]
            if t == ",":
                fields.append(current)
                current = ""
            elif t.startswith('"'):
                current += bytes(t[1:-1], "utf-8").decode("unicode_escape")
            else:
                current = t
            i += 1
        fields.append(current)
        i += 1
        if len(fields) not in (5, 6):
            raise SystemExit(f"docs_config_ref: {rel(SCHEMA)}: an entry has {len(fields)} fields: {fields}")
        keys.append(
            Key(
                section=fields[0],
                key=fields[1],
                type=TYPES[fields[2].removeprefix("KeyType::")],
                required=fields[3] == "true",
                doc=fields[4],
                passthrough=len(fields) == 6 and fields[5] == "true",
            )
        )
    return keys


def select(keys: list[Key], arg: str) -> list[Key]:
    section, _, flavour = arg.partition(":")
    if flavour not in ("", "connector"):
        raise SystemExit(f"docs_config_ref: {rel(PAGE)}: bad region argument '{arg}'")
    chosen = [k for k in keys if k.section == section and k.key != "*" and k.passthrough == (flavour == "connector")]
    if not chosen:
        raise SystemExit(f"docs_config_ref: {rel(PAGE)}: no schema keys for '{arg}'")
    return chosen


def table(keys: list[Key]) -> str:
    rows = ["| Key | Type | Required | Meaning |", "|---|---|---|---|"]
    for k in keys:
        doc = k.doc.replace("|", "\\|") or "(no description in schema.hpp)"
        rows.append(f"| `{k.key}` | {k.type} | {'yes' if k.required else ''} | {doc} |")
    return "\n".join(rows)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if the page is stale instead of rewriting it")
    args = ap.parse_args()
    keys = parse_schema(SCHEMA.read_text(encoding="utf-8"))
    text = PAGE.read_text(encoding="utf-8")
    try:
        new, seen = rewrite_regions(text, "config-keys", lambda a: table(select(keys, a)), rel(PAGE))
    except ValueError as e:
        print(f"docs_config_ref: error: {e}", file=sys.stderr)
        return 2
    covered: dict[tuple[str, str], int] = {}
    for arg in seen:
        for k in select(keys, arg):
            covered[(k.section, k.key)] = covered.get((k.section, k.key), 0) + 1
    problems = [
        f"[{k.section}] {k.key} is in no config-keys table"
        for k in keys
        if k.key != "*" and (k.section, k.key) not in covered
    ]
    problems += [f"[{s}] {key} is in {n} tables" for (s, key), n in covered.items() if n > 1]
    for p in problems:
        print(f"docs_config_ref: {rel(PAGE)}: {p}", file=sys.stderr)
    changed = [PAGE] if new != text else []
    if changed and not args.check:
        PAGE.write_text(new, encoding="utf-8")
    rc = finish(changed, args.check, "docs_config_ref", "python3 tools/docs_config_ref.py")
    print(f"docs_config_ref: {len(keys)} schema entries, {len(seen)} table(s)")
    return 1 if problems else rc


if __name__ == "__main__":
    sys.exit(main())

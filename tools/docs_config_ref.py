#!/usr/bin/env python3
"""Generates the key tables of docs/reference/configuration.md.

The generic keys come from include/fastmm/config/schema.hpp. A connector's keys belong to the
connector, not to the central schema: each venue declares them in its registration file
(src/venues/<venue>/*_registration.cpp, `constexpr VenueKeySpec k...[]`) next to the entry it
registers, and this tool reads them from there.

The page holds hand-written text and one generated table per region:

    <!-- BEGIN config-keys engine -->        a schema section (engine, venues.*, risk, ...)
    <!-- BEGIN config-keys connectors -->    the registered connectors and what they support
    <!-- BEGIN config-keys venue:bybit -->    the keys the bybit connector owns
    ...
    <!-- END config-keys -->

Every schema key and every venue key must appear in exactly one table, except the free-form `*`
entries ([strategy.params], [sim], [backtest]), which the page describes by hand. The meaning
column is the key's doc string.

usage:
  python3 tools/docs_config_ref.py           rewrite the page
  python3 tools/docs_config_ref.py --check   exit 1 if it is stale or a key is missing (CI)
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs_md import ROOT, finish, rel, rewrite_regions  # noqa: E402

SCHEMA = ROOT / "include" / "fastmm" / "config" / "schema.hpp"
VENUES = ROOT / "src" / "venues"
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
STRING = re.compile(r'\s*"((?:\\.|[^"\\])*)"')


ESCAPES = {"n": "\n", "t": "\t", "0": "\0"}


def unquote(literal: str) -> str:
    """The text of a C++ string literal's body; leaves non-ASCII alone (unicode_escape would not)."""
    return re.sub(r"\\(.)", lambda m: ESCAPES.get(m.group(1), m.group(1)), literal)


@dataclass
class Key:
    section: str
    key: str
    type: str
    required: bool
    doc: str


@dataclass
class Connector:
    name: str
    summary: str
    aliases: list[str]
    caps: dict[str, bool]
    keys: list[Key]
    source: Path


def entries(text: str, start: int, fields: int, where: str) -> list[list[str]]:
    """The brace-delimited entries of a constexpr table starting at `start`, each `fields` long."""
    tokens = [t for t in TOKEN.findall(text, start) if not t.startswith("/")]
    rows: list[list[str]] = []
    i = 0
    while i < len(tokens) and tokens[i] != "}":
        if tokens[i] == ",":
            i += 1
            continue
        if tokens[i] != "{":
            raise SystemExit(f"docs_config_ref: {where}: unexpected token {tokens[i]!r}")
        row: list[str] = []
        current = ""
        i += 1
        while tokens[i] != "}":
            t = tokens[i]
            if t == ",":
                row.append(current)
                current = ""
            elif t.startswith('"'):
                current += unquote(t[1:-1])
            else:
                current = t
            i += 1
        row.append(current)
        i += 1
        if len(row) != fields:
            raise SystemExit(f"docs_config_ref: {where}: an entry has {len(row)} fields: {row}")
        rows.append(row)
    return rows


def parse_schema(text: str) -> list[Key]:
    marker = "kConfigSchema[] = {"
    start = text.find(marker)
    if start < 0:
        raise SystemExit(f"docs_config_ref: {rel(SCHEMA)}: kConfigSchema not found")
    return [
        Key(section=r[0], key=r[1], type=TYPES[r[2].removeprefix("KeyType::")], required=r[3] == "true", doc=r[4])
        for r in entries(text, start + len(marker), 5, rel(SCHEMA))
    ]


def joined_string(text: str, at: int) -> str:
    """The value of a `... = "a" "b"` initializer at `at`: adjacent literals joined."""
    out = ""
    while (m := STRING.match(text, at)) is not None:
        out += unquote(m.group(1))
        at = m.end()
    return out


def parse_connector(path: Path) -> Connector:
    text = path.read_text(encoding="utf-8")
    where = rel(path)
    name_at = text.find('.name = "')
    if name_at < 0:
        raise SystemExit(f"docs_config_ref: {where}: no `.name = \"...\"` venue entry")
    name = joined_string(text, name_at + len(".name ="))
    summary_at = text.find(".summary =")
    summary = joined_string(text, summary_at + len(".summary =")) if summary_at >= 0 else ""

    aliases: list[str] = []
    if (m := re.search(r"\.aliases = (\w+)", text)) is not None:
        table = re.search(r"std::string_view\s+" + m.group(1) + r"\[\] = \{(.*?)\};", text, re.S)
        if table is None:
            raise SystemExit(f"docs_config_ref: {where}: alias table {m.group(1)} not found")
        aliases = [unquote(a) for a in re.findall(r'"((?:\\.|[^"\\])*)"', table.group(1))]

    caps = {c: v == "true" for c, v in re.findall(r"\.(\w+) = (true|false)", text)}

    keys: list[Key] = []
    if (m := re.search(r"\.keys = (\w+)", text)) is not None:
        marker = f"VenueKeySpec {m.group(1)}[] = {{"
        start = text.find(marker)
        if start < 0:
            raise SystemExit(f"docs_config_ref: {where}: key table {m.group(1)} not found")
        keys = [
            Key(section=name, key=r[0], type=TYPES[r[1].removeprefix("KeyType::")], required=r[2] == "true", doc=r[3])
            for r in entries(text, start + len(marker), 4, where)
        ]
    return Connector(name=name, summary=summary, aliases=aliases, caps=caps, keys=keys, source=path)


def parse_connectors() -> list[Connector]:
    found = [parse_connector(p) for p in sorted(VENUES.glob("*/*_registration.cpp"))]
    if not found:
        raise SystemExit(f"docs_config_ref: no venue registrations under {rel(VENUES)}")
    return sorted(found, key=lambda c: c.name)


def key_table(keys: list[Key]) -> str:
    rows = ["| Key | Type | Required | Meaning |", "|---|---|---|---|"]
    for k in keys:
        doc = k.doc.replace("|", "\\|") or "(no description)"
        rows.append(f"| `{k.key}` | {k.type} | {'yes' if k.required else ''} | {doc} |")
    return "\n".join(rows)


CAPS = [("credentials", "API keys"), ("order_entry", "Order entry"), ("replace", "Replace"), ("positions", "Positions")]


def connector_table(connectors: list[Connector]) -> str:
    rows = ["| `kind` | Aliases | Connector | " + " | ".join(label for _, label in CAPS) + " |"]
    rows.append("|---|---|---|" + "---|" * len(CAPS))
    for c in connectors:
        aliases = ", ".join(f"`{a}`" for a in c.aliases)
        marks = " | ".join("yes" if c.caps.get(cap, False) else "no" for cap, _ in CAPS)
        rows.append(f"| `{c.name}` | {aliases} | {c.summary} | {marks} |")
    return "\n".join(rows)


def select(keys: list[Key], connectors: list[Connector], arg: str) -> tuple[str, list[Key]]:
    """(rendered table, the keys it covers) for one region argument."""
    if arg == "connectors":
        return connector_table(connectors), []
    if arg.startswith("venue:"):
        name = arg.removeprefix("venue:")
        for c in connectors:
            if c.name == name:
                if not c.keys:
                    raise SystemExit(f"docs_config_ref: {rel(PAGE)}: connector '{name}' declares no keys")
                return key_table(c.keys), c.keys
        raise SystemExit(f"docs_config_ref: {rel(PAGE)}: no registered connector '{name}'")
    chosen = [k for k in keys if k.section == arg and k.key != "*"]
    if not chosen:
        raise SystemExit(f"docs_config_ref: {rel(PAGE)}: no schema keys for '{arg}'")
    return key_table(chosen), chosen


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if the page is stale instead of rewriting it")
    args = ap.parse_args()
    keys = parse_schema(SCHEMA.read_text(encoding="utf-8"))
    connectors = parse_connectors()
    text = PAGE.read_text(encoding="utf-8")
    try:
        new, seen = rewrite_regions(text, "config-keys", lambda a: select(keys, connectors, a)[0], rel(PAGE))
    except ValueError as e:
        print(f"docs_config_ref: error: {e}", file=sys.stderr)
        return 2
    covered: dict[tuple[str, str], int] = {}
    for arg in seen:
        for k in select(keys, connectors, arg)[1]:
            covered[(k.section, k.key)] = covered.get((k.section, k.key), 0) + 1
    wanted = [k for k in keys if k.key != "*"] + [k for c in connectors for k in c.keys]
    problems = [f"[{k.section}] {k.key} is in no config-keys table" for k in wanted if (k.section, k.key) not in covered]
    problems += [f"[{s}] {key} is in {n} tables" for (s, key), n in covered.items() if n > 1]
    if "connectors" not in seen:
        problems.append("no <!-- BEGIN config-keys connectors --> table")
    for p in problems:
        print(f"docs_config_ref: {rel(PAGE)}: {p}", file=sys.stderr)
    changed = [PAGE] if new != text else []
    if changed and not args.check:
        PAGE.write_text(new, encoding="utf-8")
    rc = finish(changed, args.check, "docs_config_ref", "python3 tools/docs_config_ref.py")
    print(f"docs_config_ref: {len(keys)} schema entries, {len(connectors)} connectors, {len(seen)} table(s)")
    return 1 if problems else rc


if __name__ == "__main__":
    sys.exit(main())

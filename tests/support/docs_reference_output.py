#!/usr/bin/env python3
"""The outputs and versions the docs quote are the ones this build has.

usage: docs_reference_output.py <program dir>...

Output: a page marks a fenced block with the command that prints it, the program first (looked up
in the program dirs) and the arguments as the docs give them:

    <!-- output: fastmm-backtest --config configs/backtest-example.toml --data synthetic -->
    ```text
    ...
    ```

Every line of the block must match a line of the output, in order; lines may be skipped. A line
that is only `...` matches nothing and is dropped, `...` inside a line matches anything, and so
does the value of `wall=` (the run's wall-clock time).

The commands run in the order of the pages and of the blocks in them, all in one scratch directory
that links to the repository's top-level entries, so the files a command writes (`runs/...`) stay
out of the tree and a later block can read them.

Versions: each row of the table in docs/reference/compatibility.md whose surface is in VERSIONS
must give the constant's value in its last column.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MARKER = re.compile(r"^<!-- output: (?P<cmd>.+?) -->\s*$")

# surface in compatibility.md -> (header, constant); the cell is the value, the journal's adds
# the oldest version a reader opens.
VERSIONS = {
    "`.fmj` journal": ("include/fastmm/core/journal.hpp", "kJournalVersion", "kJournalMinVersion"),
    "Status file": ("include/fastmm/core/status_segment.hpp", "kStatusVersion", None),
    "Store": ("include/fastmm/store/sqlite_store.hpp", "kSqliteSchemaVersion", None),
    "Gateway protocol": ("include/fastmm/live/gateway.hpp", "kVersion", None),
    "Hot-hook ABI": ("include/fastmm/strategies/hot_abi.h", "FASTMM_HOT_ABI_VERSION", None),
}


def line_pattern(doc_line):
    doc_line = re.sub(r"wall=\S+", "wall=...", doc_line.rstrip())
    return re.compile(".*".join(re.escape(p) for p in doc_line.split("...")) + r"\s*$")


def check_block(bin_dirs, cwd, where, cmd, block):
    argv = cmd.split()
    exe = next((d / argv[0] for d in bin_dirs if (d / argv[0]).is_file()), None)
    if exe is None:
        return [f"{where}: {argv[0]} not found in {', '.join(map(str, bin_dirs))}"]
    r = subprocess.run([str(exe), *argv[1:]], cwd=cwd, capture_output=True, text=True,
                       timeout=300, check=False)
    out = (r.stderr + r.stdout).splitlines()
    i = 0
    for doc_line in block:
        if doc_line.strip() in ("", "..."):
            continue
        pat = line_pattern(doc_line)
        while i < len(out) and not pat.match(out[i]):
            i += 1
        if i == len(out):
            return [f"{where}: `{cmd}` (exit {r.returncode}) prints no line matching, after the "
                    f"lines before it:\n    {doc_line}\n  its output:\n" +
                    "\n".join("    " + o for o in out)]
        i += 1
    return []


def check_outputs(bin_dirs, cwd):
    for entry in ROOT.iterdir():
        if entry.name not in ("runs", ".git"):
            (cwd / entry.name).symlink_to(entry)
    errors = []
    checked = 0
    for page in sorted((ROOT / "docs").rglob("*.md")):
        lines = page.read_text(encoding="utf-8").splitlines()
        for n, line in enumerate(lines):
            m = MARKER.match(line)
            if not m:
                continue
            where = f"{page.relative_to(ROOT)}:{n + 1}"
            if n + 1 >= len(lines) or not lines[n + 1].startswith("```"):
                errors.append(f"{where}: no fenced block after the output marker")
                continue
            end = next((k for k in range(n + 2, len(lines)) if lines[k].startswith("```")), None)
            if end is None:
                errors.append(f"{where}: the fenced block is not closed")
                continue
            errors += check_block(bin_dirs, cwd, where, m.group("cmd"), lines[n + 2:end])
            checked += 1
    if checked == 0:
        errors.append("no <!-- output: ... --> marker in docs/")
    return errors


def constant(header, name):
    text = (ROOT / header).read_text(encoding="utf-8")
    m = re.search(rf"\b{name}\b\s*=?\s*(\d+)", text)
    if not m:
        raise SystemExit(f"{header}: no {name}")
    return int(m.group(1))


def check_versions():
    page = ROOT / "docs" / "reference" / "compatibility.md"
    rows = {}
    for line in page.read_text(encoding="utf-8").splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if line.startswith("|") and len(cells) >= 3:
            rows[cells[0]] = cells[-1]
    errors = []
    for surface, (header, name, min_name) in VERSIONS.items():
        want = str(constant(header, name))
        if min_name:
            want += f", readers open {constant(header, min_name)} to {want}"
        if rows.get(surface) != want:
            errors.append(f"{page.relative_to(ROOT)}: {surface}: '{rows.get(surface)}', "
                          f"{header} says '{want}'")
    return errors


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="fastmm-docs-output-") as cwd:
        errors = check_outputs([Path(d) for d in sys.argv[1:]], Path(cwd)) + check_versions()
    for e in errors:
        print(e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

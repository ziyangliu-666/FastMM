#!/usr/bin/env python3
"""fastmm-live --help and the exit-code table in the docs list the same codes.

usage: exit_codes_doc.py <fastmm-live> <doc.md>...

--help lists the codes under "Exit codes:" as indented "<code>  <meaning>" lines. Each document
must have a heading containing "Exit codes" followed by a table whose rows start with "| <code> |";
the codes in every table must equal the codes in --help.
"""
import re
import subprocess
import sys


def help_codes(exe):
    out = subprocess.run([exe, "--help"], capture_output=True, text=True, check=True).stdout
    _, found, block = out.partition("Exit codes:\n")
    if not found:
        return None
    codes = set()
    for line in block.splitlines():
        m = re.match(r"\s+(\d+)\s+\S", line)
        if not m:
            break
        codes.add(int(m.group(1)))
    return codes


def doc_codes(path):
    lines = open(path, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        if line.startswith("#") and "Exit codes" in line:
            codes = set()
            for row in lines[i + 1:]:
                if row.startswith("#"):
                    break
                m = re.match(r"\|\s*(\d+)\s*\|", row)
                if m:
                    codes.add(int(m.group(1)))
            return codes
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, docs = sys.argv[1], sys.argv[2:]
    want = help_codes(exe)
    if not want:
        print(f"FAIL {exe} --help has no 'Exit codes:' list")
        return 1
    print(f"--help: {sorted(want)}")
    failures = 0
    for doc in docs:
        got = doc_codes(doc)
        if got is None:
            print(f"FAIL {doc}: no 'Exit codes' heading")
            failures += 1
        elif got != want:
            print(f"FAIL {doc}: {sorted(got)} (missing {sorted(want - got)}, extra {sorted(got - want)})")
            failures += 1
        else:
            print(f"ok   {doc}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

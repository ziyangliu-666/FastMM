#!/usr/bin/env python3
"""Summarise an Autobahn|Testsuite report and fail on any case that did not pass.

    python3 tools/autobahn_check.py <reports>/clients/index.json

A case passes when both its behavior and its close behavior are OK, NON-STRICT or INFORMATIONAL.
FAILED, UNIMPLEMENTED or a case group with no result fails the check (exit 1).
"""

import argparse
import collections
import json
import pathlib
import sys

PASS = {"OK", "NON-STRICT", "INFORMATIONAL"}
GROUPS = ("1", "2", "3", "4", "5", "6", "7", "9", "10")


def case_key(case_id: str) -> tuple:
    return tuple(int(p) for p in case_id.split("."))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("index", type=pathlib.Path, help="index.json written by wstest")
    args = ap.parse_args()
    if not args.index.is_file():
        print(f"autobahn: {args.index} not written", file=sys.stderr)
        return 1
    report = json.loads(args.index.read_text())
    if not report:
        print(f"autobahn: {args.index} holds no agent", file=sys.stderr)
        return 1
    rc = 0
    for agent, cases in sorted(report.items()):
        counts = collections.Counter()
        bad = []
        for case_id in sorted(cases, key=case_key):
            r = cases[case_id]
            behavior, close = r.get("behavior"), r.get("behaviorClose")
            counts[behavior] += 1
            if behavior not in PASS or close not in PASS:
                bad.append(f"  {case_id}: {behavior} (close {close}) {r.get('reportfile', '')}")
        missing = [g for g in GROUPS if not any(c.split(".")[0] == g for c in cases)]
        summary = ", ".join(f"{n} {k}" for k, n in sorted(counts.items()))
        print(f"{agent}: {len(cases)} cases: {summary}")
        if missing:
            print(f"  no results for case groups {', '.join(missing)}")
            rc = 1
        if bad:
            print("\n".join(bad))
            rc = 1
    print(f"report: {args.index.parent / 'index.html'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())

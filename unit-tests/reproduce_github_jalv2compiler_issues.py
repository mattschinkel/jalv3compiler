#!/usr/bin/env python3
"""Run jalv2 against tmp/github_issues/repro/*.jal and print TSV for investigation doc.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAL = os.path.join(ROOT, "bin", "jalv3-x86-64")
LIB = os.path.join(ROOT, "lib")
REPRO = os.path.join(ROOT, "tmp", "github_issues", "repro")

# issue_7: design-only — no source file
SKIP_FILES = set()


def run_one(path: str) -> tuple[int, str]:
    env = os.environ.copy()
    p = subprocess.run(
        [JAL, "-s", LIB, path],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=120,
    )
    out = (p.stdout or "") + (p.stderr or "")
    tail = " ".join(out.strip().split())[-400:]
    return p.returncode, tail


def main() -> int:
    if not os.path.isfile(JAL):
        print("missing", JAL, file=sys.stderr)
        return 1
    rows = []
    for path in sorted(glob.glob(os.path.join(REPRO, "issue_*.jal"))):
        base = os.path.basename(path)
        num = re.search(r"issue_(\d+)", base)
        n = int(num.group(1)) if num else -1
        try:
            code, tail = run_one(path)
        except subprocess.TimeoutExpired:
            code, tail = -9, "TIMEOUT"
        except subprocess.CalledProcessError as e:
            code, tail = e.returncode, str(e)
        st = "ABORT" if code in (134, -6) else ("TIMEOUT" if code == -9 else ("OK" if code == 0 else f"exit{code}"))
        if "Assertion" in tail or "assertion" in tail.lower():
            st = "ASSERT"
        rows.append((n, base, code, st, tail))
    rows.sort(key=lambda r: r[0])
    print("issue\tfile\traw_exit\tsummary\tlast_msg")
    for n, base, code, st, tail in rows:
        print(f"{n}\t{base}\t{code}\t{st}\t{tail}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

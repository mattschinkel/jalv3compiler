#!/usr/bin/env python3
"""Run every sims/picsim/*.jal test through sims/picsim_validate_engine.py.

Requires a built picsim tree (the fork carrying the 16F18857 model):
  PICSIM_DIR=/path/to/picsim python3 sims/run_picsim_regression.py
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--picsim", default=os.environ.get("PICSIM_DIR"))
    ap.add_argument("--compiler", default=str(ROOT / "bin" / "beta" / "jalv3-x86-64"))
    ap.add_argument("tests", nargs="*")
    args = ap.parse_args()
    if not args.picsim:
        raise SystemExit("set --picsim DIR or PICSIM_DIR")
    tests = args.tests or sorted(str(p) for p in (ROOT / "sims" / "picsim").glob("*.jal"))
    if not tests:
        raise SystemExit("no tests found")
    failed = 0
    for t in tests:
        r = subprocess.run([sys.executable, str(ROOT / "sims" / "picsim_validate_engine.py"),
                            t, "--picsim", args.picsim, "--compiler", args.compiler])
        failed += r.returncode != 0
    print(f"{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

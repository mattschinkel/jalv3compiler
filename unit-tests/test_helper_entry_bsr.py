#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""Pin: synthesized call-helper entries must select their own bank.

Compiler-synthesized helpers (`_pic_indirect`, `_pic_multiply`, `_pic_divide`,
`_pic_sdivide`, `_pic_ptr_read`, `_pic_float_*`, ...) are placed inline with
their own label and entered with `CALL` from arbitrary BSR state.  The MOVLB
optimizer once treated them as ordinary straight-line code (it seeded entry
banks only for procs), removed the helper's entry MOVLB, and
`_pic_indirect` then read `_pic_pointer+1` through the caller's bank -- a
garbage indirect-call target (the 18F67J50 float "reset", 2026-10-30).

This pin compiles `unit-tests/fixtures/helper_bsr_probe.jal` and requires
that, for every `l__pic_*` helper entry, a `movlb` appears before the first
banked data access.

Usage: unit-tests/test_helper_entry_bsr.py [--compiler PATH]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURE = REPO / "unit-tests/fixtures/helper_bsr_probe.jal"

ENTRY = re.compile(r"^(l__pic_[A-Za-z0-9_]+)\s*$")
INSTR = re.compile(r"^\s{2,}([a-z][a-z0-9]*)\s+(.*)$")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(REPO / "bin/jalv3-x86-64"))
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="helperbsr_") as td:
        asm = Path(td) / "probe.asm"
        r = subprocess.run(
            [args.compiler, "-asm", str(asm),
             "-s", f"{REPO}/demo_lib;{REPO}/third_party/jallib/lib",
             str(FIXTURE)],
            capture_output=True, text=True)
        if "0 errors" not in r.stdout:
            print("FAIL: fixture did not compile cleanly:")
            print(r.stdout[-1500:])
            return 1
        lines = asm.read_text(errors="replace").splitlines()

    checked = 0
    bad: list[str] = []
    cur: str | None = None
    saw_movlb = False
    for ln in lines:
        m = ENTRY.match(ln.strip())
        if m:
            cur = m.group(1)
            saw_movlb = False
            continue
        if cur is None:
            continue
        ins = INSTR.match(ln)
        if not ins:
            continue
        op, rest = ins.group(1), ins.group(2)
        if op == "movlb":
            saw_movlb = True
            continue
        if "v__banked" in rest and op not in ("return", "retlw"):
            checked += 1
            if not saw_movlb:
                bad.append(f"{cur}: first banked access '{op} {rest.split(',')[0]}'"
                           " has no movlb")
            cur = None

    if bad:
        print("FAIL: helper entries without a bank select:")
        for b in bad:
            print("  " + b)
        return 1
    if checked == 0:
        print("FAIL: no helper entry banked access found (fixture drifted?)")
        return 1
    print(f"PASS: {checked} helper entry banked access(es) all bank-selected")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""PIC18 v6-core (Q43) Access Bank regression gate.

Pins the encodings protected by fixes/fix_pic18_v6_access_bank_shared.md:

  1. Q43 witness (`18f47q43_serial_hardware`): every `v__access` file
     operand's variable lives inside the relocated Access Bank windows
     (0x500-0x55F Access RAM / 0x460-0x4FF Access SFR), every variable
     inside those windows is always access-encoded (never `v__banked`),
     and the MOVLB count stays within the access-encoded budget (10 at
     the fix; 61 when the D3 guard rejected the windows).
  2. Classic non-regression (`18f4620_serial_hw_echo`): no `v__access`
     operand targets the 0x60-0x7F bank-0 GPR hole (the classic guard,
     fixes/fix_pic18_variable_shared_flag_access_bank_low_gpr.md).

Usage: python3 unit-tests/test_pic18_v6_access_bank.py [--compiler PATH]
Exit 0 = all checks pass.


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

V6_RAM_WINDOW = (0x500, 0x55F)
V6_SFR_WINDOW = (0x460, 0x4FF)
CLASSIC_GPR_HOLE = (0x60, 0x7F)
V6_MOVLB_BUDGET = 15  # witness: 10; guard-regressed build: 61

EQU_RE = re.compile(r"^(\S+)\s+EQU\s+(0x[0-9A-Fa-f]+|\d+)\b", re.I)
INSTR_RE = re.compile(r"^\s{2,}(\S+)\s+(.*)$")
MOVLB_RE = re.compile(r"^\s+movlb\b", re.I)


def in_range(v, lo, hi):
    return lo <= v <= hi


def parse_asm(path):
    """Return (equ_map, uses, movlb_ct).

    uses: list of (file_operand_name, access: bool) for file-register
    instructions carrying a v__access/v__banked suffix.
    """
    equs = {}
    uses = []
    movlb_ct = 0
    with open(path) as fh:
        for line in fh:
            m = EQU_RE.match(line)
            if m:
                equs[m.group(1)] = int(m.group(2), 0)
                continue
            if MOVLB_RE.match(line):
                movlb_ct += 1
                continue
            m = INSTR_RE.match(line)
            if not m:
                continue
            body = line.rstrip()
            low = body.lower()
            if low.endswith(",v__access"):
                access = True
            elif low.endswith(",v__banked"):
                access = False
            else:
                continue
            operands = [o.strip() for o in m.group(2).split(",")]
            if operands:
                uses.append((operands[0], access))
    return equs, uses, movlb_ct


def compile_sample(compiler, sample, workdir):
    # the samples live in the fetched jallib tree; unit-tests/Sample only
    # ever existed on the original workstation, so this gate crashed instead
    # of pinning anything (found by the 2026-10-22 suite audit).
    jal = os.path.join(ROOT, "third_party", "jallib", "samples", sample + ".jal")
    tmp_jal = os.path.join(workdir, sample + ".jal")
    shutil.copy(jal, tmp_jal)
    include = ";".join([
        os.path.join(ROOT, "demo_lib"),
        os.path.join(ROOT, "third_party", "jallib", "lib"),
        os.path.join(ROOT, "sims", "shared"),
    ])
    proc = subprocess.run(
        [compiler, "-s", include, tmp_jal],
        capture_output=True, text=True, timeout=180,
        cwd=workdir,
    )
    out = proc.stdout + proc.stderr
    ok = ("0 errors" in out) and ("1 errors" not in out) and proc.returncode == 0
    asm = os.path.join(workdir, sample + ".asm")
    return ok, asm if os.path.exists(asm) else None, out


def check_v6(compiler, workdir, failures):
    ok, asm, out = compile_sample(compiler, "18f47q43_serial_hardware", workdir)
    if not ok or not asm:
        failures.append("v6 witness compile failed:\n" + out[-800:])
        return
    equs, uses, movlb_ct = parse_asm(asm)

    # (a) nothing outside the windows is access-encoded
    for name, access in uses:
        if access and name in equs:
            v = equs[name]
            if not (in_range(v, *V6_RAM_WINDOW) or in_range(v, *V6_SFR_WINDOW)):
                failures.append(
                    "v6: %s (EQU 0x%X) access-encoded but outside the "
                    "Access Bank windows" % (name, v))
    # (b) everything inside the windows is always access-encoded
    window_banked = {}
    window_seen = {}
    for name, access in uses:
        v = equs.get(name)
        if v is None:
            continue
        if in_range(v, *V6_RAM_WINDOW) or in_range(v, *V6_SFR_WINDOW):
            window_seen[name] = True
            if not access:
                window_banked[name] = v
    for name, v in sorted(window_banked.items()):
        failures.append(
            "v6: %s (EQU 0x%X) is inside an Access Bank window but got a "
            "banked (MOVLB) encoding" % (name, v))
    if not window_seen:
        failures.append("v6: no window variables used at all -- sample or "
                        "parser regression")
    # (c) MOVLB budget
    if movlb_ct > V6_MOVLB_BUDGET:
        failures.append(
            "v6: MOVLB count %d exceeds budget %d (guard rejecting the "
            "windows again?)" % (movlb_ct, V6_MOVLB_BUDGET))
    print("  v6 witness: %d window vars, %d banked window uses, MOVLB %d "
          "(budget %d)" % (len(window_seen), len(window_banked), movlb_ct,
                           V6_MOVLB_BUDGET))


def check_classic(compiler, workdir, failures):
    ok, asm, out = compile_sample(compiler, "18f4620_serial_hw_echo", workdir)
    if not ok or not asm:
        failures.append("classic witness compile failed:\n" + out[-800:])
        return
    equs, uses, _ = parse_asm(asm)
    for name, access in uses:
        if access and name in equs:
            if in_range(equs[name], *CLASSIC_GPR_HOLE):
                failures.append(
                    "classic: %s (EQU 0x%X) in the 0x60-0x7F GPR hole is "
                    "access-encoded" % (name, equs[name]))
    print("  classic witness: 0x60-0x7F access-encoding check done")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=os.path.join(ROOT, "bin", "jalv3-x86-64"))
    args = ap.parse_args()

    # compile_sample runs with cwd=workdir, so a relative --compiler path
    # (e.g. ./bin/jalv3-x86-64) would not resolve there.
    compiler = os.path.abspath(args.compiler)
    failures = []
    with tempfile.TemporaryDirectory(prefix="jalv2_v6ab_") as workdir:
        check_v6(compiler, workdir, failures)
        check_classic(compiler, workdir, failures)

    if failures:
        print("FAIL:")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS: v6 Access Bank encodings + classic 0x60-0x7F guard OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

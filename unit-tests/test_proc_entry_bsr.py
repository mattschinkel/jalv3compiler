#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""Proc-entry BSR conformance: every procedure body's FIRST banked data
operation must be preceded (since its entry label) by a MOVLB -- the
runtime entry BSR is the caller's leftover and cannot be assumed (the j50
stale-print defect: a caller loading the final W param from bank 1 made
the callee's first movwf store into the wrong bank).

Compiles a multi-proc fixture and scans the -asm listing. Layout-independent
(param homes in the access window or in any bank are both fine -- the rule
is about the MOVLB, not the placement).
Usage: python3 unit-tests/test_proc_entry_bsr.py [--compiler PATH]
"""
from __future__ import annotations
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

FIXTURE = """include 18f4620
pragma target clock 20_000_000
pragma target OSC hs
pragma target WDT control
pragma target LVP enabled
enable_digital_io()
const serial_hw_baudrate = 115_200
include serial_hardware
serial_hw_init()
alias serial is serial_hw_data
include print

var volatile byte pad_window[96]
var byte target_a = 0
var byte target_b = 0
var byte target_c = 88

procedure side_one(byte in v) is
   target_a = v
end procedure
procedure side_two(byte in v) is
   target_b = v
end procedure
function getter return byte is
   return target_c
end function

var byte k
var byte acc
for 20 loop
   k = getter()
   side_one(k)
   side_two(k)
   acc = acc + k
   pad_window[k % 96] = acc
end loop
print_byte_dec(serial, acc)
print_byte_dec(serial, k)
if acc == 1760 then
   print_byte_dec(serial, acc)
end if
"""

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", type=Path, default=REPO / "bin/jalv3-x86-64")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="proc_entry_bsr_") as td:
        jal = Path(td) / "fixture.jal"
        asm = Path(td) / "fixture.asm"
        jal.write_text(FIXTURE)
        r = subprocess.run(
            [str(args.compiler), "-s",
             f"{REPO}/demo_lib;{REPO}/third_party/jallib/lib",
             "-no-codfile", "-debug=pcode", "-asm", str(asm), str(jal)],
            capture_output=True, text=True, cwd=str(REPO))
        if r.returncode != 0 or not asm.is_file():
            print(r.stdout, r.stderr, sep="\n", file=sys.stderr)
            print("error: fixture failed to compile", file=sys.stderr)
            return 2

        fails = 0
        in_proc = False
        saw_movlb = False
        proc_name = ""
        for line in asm.read_text(errors="replace").splitlines():
            m_ent = re.search(r"\{enter: (\S+)\}", line)
            if m_ent:
                in_proc, saw_movlb, proc_name = True, False, m_ent.group(1)
                continue
            if not in_proc:
                continue
            op = line.strip()
            if op.startswith("movlb"):
                saw_movlb = True
                continue
            if "v__banked" in op and not op.startswith(("movlw", "call", "goto")):
                if not saw_movlb:
                    print(f"FAIL: {proc_name}: first banked op has no entry MOVLB:"
                          f"  {op}", file=sys.stderr)
                    fails += 1
                in_proc = False   # only the first banked op is entry-critical
        if fails:
            print(f"{fails} proc entr(ies) violate the entry-BSR contract",
                file=sys.stderr)
            return 1
    print("OK: every proc entry stages BSR before its first banked op")
    return 0

if __name__ == "__main__":
    sys.exit(main())

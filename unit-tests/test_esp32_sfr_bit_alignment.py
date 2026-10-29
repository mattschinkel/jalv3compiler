#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""ESP32 SFR bit-access alignment pin (compile-only gate).

The SFR bit paths in src/libesp32/esp_op.c (esp_load_bit /
esp_store_bit) access peripheral bits with 32-bit loads/stores.  Until
2026-10-27 the address was `base + bit/8` -- the BYTE address -- so any
SFR bit at index >= 8 emitted l32i/s32i at an unaligned address.  On
real silicon that faults immediately (Fatal exception 9:
LoadStoreAlignment, found by the bench LED-monitor program reading
pin_23); QEMU does not enforce alignment, so every suite and sample
(bits 0-7 only, e.g. pin_2/pin_4/pin_5) passed for months.

The fix word-aligns the access base (pool literal folded at compile
time for direct accesses; runtime AND -3 for pointer-carried bits).
This gate compiles a program touching bits 23 (read + direction write)
and pins the emitted literals to the ALIGNED registers:
GPIO_IN      0x3FF4403C (was 0x3FF4403E unaligned)
GPIO_ENABLE  0x3FF44020 (was 0x3FF44022 unaligned)

Usage:  python3 unit-tests/test_esp32_sfr_bit_alignment.py [--compiler PATH]
Exit:   0 = pass, 1 = fail.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_COMPILER = ROOT / "bin" / "jalv3-x86-64"
INCLUDE = ";".join([
    str(ROOT / "demo_lib"),
    str(ROOT / "third_party" / "jallib" / "lib"),
])

SRC = """\
include esp32_xtensa_lx6
pragma target clock 240_000_000
pragma target OSC      PLL
include esp32_clock
esp32_clock_init()
_usec_delay(100_000)
include esp32_serial_hw
alias serial_data is serial_hw_data
const PRINT_MAX_VAR_SIZE = 4
include print
var byte x
pin_23_direction = FALSE
if pin_23 == 1 then
   x = 1
else
   x = 0
end if
print_byte_dec(serial_data, x)
forever loop
end loop
"""

GOOD = ("0x3ff4403c", "0x3ff44020")   # aligned GPIO_IN / GPIO_ENABLE
BAD = ("0x3ff4403e", "0x3ff44022")   # the pre-fix unaligned forms


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    args = ap.parse_args(argv)
    compiler = pathlib.Path(args.compiler)
    if not compiler.is_file():
        print(f"error: compiler not found: {compiler}")
        return 2

    with tempfile.TemporaryDirectory(prefix="esp32_align_") as td:
        src = pathlib.Path(td) / "align.jal"
        src.write_text(SRC)
        proc = subprocess.run(
            [str(compiler), "-s", INCLUDE, str(src)],
            cwd=ROOT, capture_output=True, text=True)
        if "0 errors" not in proc.stdout:
            print("FAIL: compile failed")
            print(proc.stdout[-800:])
            return 1
        asm = (pathlib.Path(td) / "align.asm").read_text()

    ok = True
    for want in GOOD:
        if want not in asm:
            print(f"FAIL: aligned literal {want} missing from .asm")
            ok = False
    for bad in BAD:
        if bad in asm:
            print(f"FAIL: unaligned literal {bad} still emitted")
            ok = False
    print("RESULT:", "PASS" if ok else "FAIL",
          "(SFR bit literals word-aligned)")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

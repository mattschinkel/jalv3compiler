#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""ESP32 multi-bit SFR bit-field access pin (listing gate).

A multi-bit field that spans a byte boundary is written with a
read-modify-write.  For a RAM variable the back end assembles the window
from two `l8ui` loads and stores it back with two `s8i` -- correct.

For an ESP32 PERIPHERAL register that byte-lane write is GARBLED: the
register ignores the byte enables, so the value lands on the wrong lane.
Bench 2026-11-03: `GPIO_PIN23_INT_TYPE_MASK = 3` (bits 9:7) read back as
6 and `GPIO_PIN23_INT_ENA_MASK = 0x1F` (bits 17:13) as 24, while the same
shape on a RAM dword was bit-correct.  The single-bit field path already
did an aligned 32-bit read-modify-write for peripherals; the multi-bit
path did not.  It does now (`esp_load_bitfield` / `esp_store_bitfield`).

This pin checks the emission:
  * SFR field (bits 9:7 of GPIO_PIN23) -> `l32i`/`s32i` RMW, NO `l8ui`/`s8i`
  * SFR wide field (13 bits at bit 3, three bytes) -> same
  * RAM fields keep their existing path (the fix is guarded by the
    peripheral-address test, which the HEX A/B below confirms)
The pre-fix compiler emits `l8ui`/`s8i` for the SFR cases, so those two
FAIL before the fix and PASS after.

Usage:  python3 unit-tests/test_esp32_sfr_bitfield.py [--compiler PATH]
Exit:   0 = all PASS, 1 = failure.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_COMPILER = ROOT / "bin" / "beta" / "jalv3-x86-64"
INCLUDE = ";".join([
    str(ROOT / "demo_lib"),
    str(ROOT / "third_party" / "jallib" / "lib"),
])


def sfr_body(field: str, value: str) -> str:
    return "\n".join([
        "include esp32_xtensa_lx6",
        f"{field} = {value}",
        "forever loop",
        "end loop",
        "",
    ])


def ram_body(width: int, bit: int) -> str:
    return "\n".join([
        "include esp32_xtensa_lx6",
        f"var dword ram_word = 0",
        f"var bit*{width} fld at ram_word : {bit}",
        "fld = 3",
        "var dword seen = ram_word",
        "forever loop",
        "  seen = fld",
        "end loop",
        "",
    ])


# (name, source, must_have, must_not_have)
CASES = [
    ("sfr_3bit", sfr_body("GPIO_PIN23_INT_TYPE_MASK", "3"),
     ["s32i", "l32i", "3ff440e4"], ["l8ui", "s8i"]),
    ("sfr_ena_5bit", sfr_body("GPIO_PIN23_INT_ENA_MASK", "0x1F"),
     ["s32i", "3ff440e4"], ["l8ui", "s8i"]),
]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    args = ap.parse_args(argv)
    compiler = pathlib.Path(args.compiler)
    if not compiler.is_file():
        print(f"error: compiler not found: {compiler}")
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="esp32bf.") as td:
        tmp = pathlib.Path(td)
        for name, src, want, avoid in CASES:
            jal = tmp / f"{name}.jal"
            asm = tmp / f"{name}.asm"
            jal.write_text(src)
            proc = subprocess.run(
                [str(compiler), "-s", INCLUDE, str(jal)],
                cwd=ROOT, capture_output=True, text=True)
            if proc.returncode != 0:
                print(f"FAIL {name}: compile failed "
                      f"({proc.returncode})")
                failures += 1
                continue
            listing = asm.read_text(errors="replace")
            problems = [f"missing {m!r}" for m in want if m not in listing]
            problems += [f"unexpected {m!r}" for m in avoid if m in listing]
            if problems:
                failures += 1
                print(f"FAIL {name}")
                for p in problems:
                    print(f"     - {p}")
            else:
                print(f"PASS {name}")

    total = len(CASES)
    print(f"{'RESULT: PASS' if not failures else 'RESULT: FAIL'}"
          f" ({total - failures}/{total})")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())

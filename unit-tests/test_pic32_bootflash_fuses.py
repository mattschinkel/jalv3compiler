#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""PIC32MZ boot-flash DEVCFG ordering gate (compile-only).

Pins fixes/fix_pic32_bootflash_fuses.md: a boot-flash image (no
`pragma target image pfm`) once emitted the device-file `_fuses`
defaults ON TOP of the solved clock words, so every DEVCFG word read
back erased and the part came up on config defaults -- the image never
ran on silicon.  Loader/PFM images were unaffected (they skip the
defaults block), which is why every QEMU/silicon sample missed it.

QEMU does not model fuses, so this compile-time HEX check is the
regression gate: the boot-flash words must equal the PFM words for the
same program and must carry the solved values (not the erased defaults).

The expected words are the DFP-derived FRC_SPLL 200 MHz solve from the
fix's silicon run; a change to those words is reviewed like any other
intentional codegen change.

Usage:  python3 unit-tests/test_pic32_bootflash_fuses.py [--compiler PATH]
Exit:   0 = all PASS, 1 = failure.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_COMPILER = ROOT / "bin" / "jalv3-x86-64"
INCLUDE = ";".join([
    str(ROOT / "demo_lib"),
    str(ROOT / "third_party" / "jallib" / "lib"),
    str(ROOT / "sims"),
])

# MZ EF physical DEVCFG locations (highest address = DEVCFG0).
DEVCFG_ADDR = {
    0: 0x1FC0FFCC,   # DEVCFG0
    1: 0x1FC0FFC8,   # DEVCFG1
    2: 0x1FC0FFC4,   # DEVCFG2
    3: 0x1FC0FFC0,   # DEVCFG3
}
BOOT_FLASH_BASE = 0x1FC00000
ERASED_WORDS = (0xFFFFFFFF, 0xFFFFF7FF, 0x00000000)

# (name, extra pragmas, DEVCFG0..3 expected)
# DEVCFG1 corrected 2026-10-24 (fix_mips_clock_config_bit_positions):
# the solve now writes FCKSM=CSECMD (bit 14, was bit 15) and POSCMOD in
# 9:8 (was 13:12); the old 0x..B439 words carried FCKSM=CSDCME plus
# stray 13:12 bits.  WDT/DMT DISABLED are no-ops under the DFP-true
# positions (the proven word already ships both off -- the old
# 0x5B74B431 "diff" was the wrong-field artifact), so the second case
# flips them ON to keep a distinct expectation (full on/off matrix in
# sims/test_pic32mz_config_pragma_words.py).
CASES = [
    ("frc_spll_200mhz",
     [],
     (0x403FF753, 0x5F744739, 0x400131A0, 0x41000000)),
    ("frc_spll_200mhz_wdt_dmt_on",
     ["pragma target WDT ENABLED", "pragma target DMT ENABLED"],
     (0x403FF753, 0xDFF44739, 0x400131A0, 0x41000000)),
]


def parse_hex(path: pathlib.Path) -> dict[int, int]:
    mem: dict[int, int] = {}
    base = 0
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        data = bytes.fromhex(line[1:])
        count, addr, rectype = data[0], (data[1] << 8) | data[2], data[3]
        payload = data[4:4 + count]
        if rectype == 0:
            for i, b in enumerate(payload):
                mem[base + addr + i] = b
        elif rectype == 4:
            base = ((payload[0] << 8) | payload[1]) << 16
    return mem


def word_at(mem: dict[int, int], addr: int) -> int:
    return sum(mem.get(addr + i, 0xFF) << (8 * i) for i in range(4))


def compile_case(compiler: pathlib.Path, tmp: pathlib.Path, name: str,
                 pragmas: list[str], pfm: bool) -> tuple[int, str, pathlib.Path]:
    src = tmp / f"{name}.jal"
    hexf = tmp / f"{name}.hex"
    body = ["include 32mz1024efh064",
            "pragma target clock 200_000_000",
            "pragma target OSC FRC_SPLL"]
    if pfm:
        body.append("pragma target image pfm")
    body += list(pragmas)
    body += ["var dword i", "forever loop", "   i = i + 1", "end loop", ""]
    src.write_text("\n".join(body))
    proc = subprocess.run(
        [str(compiler), "-s", INCLUDE, "-hex", str(hexf), str(src)],
        cwd=ROOT, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr, hexf


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    args = ap.parse_args(argv)
    compiler = pathlib.Path(args.compiler)
    if not compiler.is_file():
        print(f"error: compiler not found: {compiler}")
        return 2

    failures = 0
    total = 0
    with tempfile.TemporaryDirectory(prefix="mzfuse.") as td:
        tmp = pathlib.Path(td)
        for name, pragmas, expected in CASES:
            boot_hex = pfm_hex = None
            for pfm in (False, True):
                label = f"{name}_{'pfm' if pfm else 'bootflash'}"
                total += 1
                rc, out, hexf = compile_case(compiler, tmp, label, pragmas, pfm)
                if rc != 0:
                    print(f"FAIL {label}: compile failed\n{out}")
                    failures += 1
                    continue
                words = tuple(word_at(parse_hex(hexf), DEVCFG_ADDR[i])
                              for i in range(4))
                if pfm:
                    pfm_hex = words
                else:
                    boot_hex = (hexf, words)

            if pfm_hex is None or boot_hex is None:
                continue
            hexf, boot_words = boot_hex

            # (1) the boot-flash image really is a boot-flash image
            total += 1
            mem = parse_hex(hexf)
            entry = word_at(mem, BOOT_FLASH_BASE)
            if entry in ERASED_WORDS:
                print(f"FAIL {name}: no code at the boot-flash reset vector "
                      f"0x{BOOT_FLASH_BASE:08X} (word 0x{entry:08X})")
                failures += 1
            else:
                print(f"PASS {name}: boot-flash code at 0x{BOOT_FLASH_BASE:08X}")

            # (2) defaults must not clobber the solved words
            total += 1
            if boot_words != pfm_hex:
                print(f"FAIL {name}: boot-flash DEVCFG differs from PFM\n"
                      f"     bootflash: "
                      + ", ".join(f"DEVCFG{i}=0x{boot_words[i]:08X}"
                                  for i in range(4))
                      + "\n     pfm:       "
                      + ", ".join(f"DEVCFG{i}=0x{pfm_hex[i]:08X}"
                                  for i in range(4)))
                failures += 1
            else:
                print(f"PASS {name}: boot-flash DEVCFG == PFM DEVCFG "
                      + ", ".join(f"DEVCFG{i}=0x{boot_words[i]:08X}"
                                  for i in range(4)))

            # (3) exact solved words (not erased defaults)
            total += 1
            if boot_words != expected:
                print(f"FAIL {name}: solved DEVCFG drift\n"
                      f"     got:      "
                      + ", ".join(f"DEVCFG{i}=0x{boot_words[i]:08X}"
                                  for i in range(4))
                      + "\n     expected: "
                      + ", ".join(f"DEVCFG{i}=0x{expected[i]:08X}"
                                  for i in range(4)))
                failures += 1
            else:
                print(f"PASS {name}: solved DEVCFG words exact")

    print(f"{'RESULT: PASS' if not failures else 'RESULT: FAIL'}"
          f" ({total - failures}/{total})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

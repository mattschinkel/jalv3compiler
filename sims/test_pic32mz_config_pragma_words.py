#!/usr/bin/env python3
"""PIC32MZ1024EFH064 config-pragma word check (compile-only gate).

`pragma target WDT|DMT|IESO|SOSC|DEBUG|MCLR <value>` masks bits into
the solved DEVCFG words.  Until 2026-10-24 the MZ field constants were
off-field (FWDTEN at a DMTCNT bit, FDMTEN at a reserved bit, IESO and
FSOSCEN four bits high, DBGPER two of three bits, POSCMOD in 13:12,
FCKSM in 16:15) so every pragma silently flipped the wrong field --
absent pragmas kept the proven words, which is why nothing ever broke
on silicon (fixes/fix_mips_clock_config_bit_positions.md).  This gate
pins the corrected DFP positions: each pragma flips EXACTLY its
device-file field against the FRC_SPLL 200 MHz baseline, and the
"other" value of each is a no-op when the proven word already carries
it (DBGPER=PG_ALL, SMCLR=NORM, FWDTEN/FDMTEN/IESO/FSOSCEN off).

Usage:  python3 sims/test_pic32mz_config_pragma_words.py [--compiler PATH]
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

DEVCFG_ADDR = {
    0: 0x1FC0FFCC,   # DEVCFG0
    1: 0x1FC0FFC8,   # DEVCFG1
}

# (name, pragma line, word, xor-vs-baseline expected)
CASES = [
    ("wdt_enabled",   "pragma target WDT ENABLED",    1, 0x00800000),
    ("wdt_disabled",  "pragma target WDT DISABLED",   1, 0x00000000),
    ("dmt_enabled",   "pragma target DMT ENABLED",    1, 0x80000000),
    ("dmt_disabled",  "pragma target DMT DISABLED",   1, 0x00000000),
    ("ieso_enabled",  "pragma target IESO ENABLED",   1, 0x00000080),
    ("ieso_disabled", "pragma target IESO DISABLED",  1, 0x00000000),
    ("sosc_enabled",  "pragma target SOSC ENABLED",   1, 0x00000040),
    ("sosc_disabled", "pragma target SOSC DISABLED",  1, 0x00000000),
    ("debug_enabled", "pragma target DEBUG ENABLED",  0, 0x00000000),
    ("debug_disabled","pragma target DEBUG DISABLED", 0, 0x00007000),
    ("mclr_external", "pragma target MCLR EXTERNAL",  0, 0x00000000),
    ("mclr_internal", "pragma target MCLR INTERNAL",  0, 0x00008000),
    # WDTPS is a device-file fuse_def group (not a keyword pragma): the
    # WDT runtime period is fuse-owned on MZ (RUNDIV mirrors WDTPS,
    # read-only -- silicon-proven 2026-10-25, PS4096 word 0x5F6C4739).
    # Baseline WDTPS = PS1048576 (code 20).
    ("wdtps_ps4096",    "pragma target WDTPS PS4096",    1, 0x00180000),
    ("wdtps_ps1048576", "pragma target WDTPS PS1048576", 1, 0x00000000),
]

BASELINE_FIELDS = {
    # field decode of the solved FRC_SPLL 200 MHz baseline (word 1)
    "FNOSC":   ((1 >> 0) & 0x7, 1),        # SPLL
    "POSCMOD": ((1 >> 8) & 0x3, 2),        # OFF (non-POSC boot seeds it inert)
    "FCKSM":   ((1 >> 14) & 0x3, 1),       # CSECMD (clock switching enabled)
}


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
                 pragmas: list[str]) -> tuple[str, pathlib.Path]:
    src = tmp / f"{name}.jal"
    hexf = tmp / f"{name}.hex"
    body = [
        "include 32mz1024efh064",
        "pragma target clock 200_000_000",
        "pragma target OSC FRC_SPLL",
        "pragma target image pfm",
        *pragmas,
        "var byte x = 1",
    ]
    src.write_text("\n".join(body) + "\n")
    proc = subprocess.run(
        [str(compiler), "-s", INCLUDE, "-hex", str(hexf), str(src)],
        cwd=ROOT, capture_output=True, text=True)
    return proc.stdout + proc.stderr, hexf


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    args = ap.parse_args(argv)
    compiler = pathlib.Path(args.compiler)
    if not compiler.is_file():
        print(f"error: compiler not found: {compiler}")
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="mzcfg.") as td:
        tmp = pathlib.Path(td)
        out, base_hex = compile_case(compiler, tmp, "baseline", [])
        if not base_hex.is_file() or "0 errors" not in out:
            print(f"FAIL baseline: compile failed\n{out}")
            return 1
        mem = parse_hex(base_hex)
        base1 = word_at(mem, DEVCFG_ADDR[1])
        for name, (shift, width), want in [
            ("FNOSC", (0, 3), 1), ("POSCMOD", (8, 2), 3), ("FCKSM", (14, 2), 1),
        ]:
            got = (base1 >> shift) & ((1 << width) - 1)
            tag = "PASS" if got == want else "FAIL"
            if got != want:
                failures += 1
            print(f"{tag} baseline {name}=0x{got:X} (want 0x{want:X})")

        for name, pragma, word, want in CASES:
            out, hexf = compile_case(compiler, tmp, name, [pragma])
            if not hexf.is_file() or "0 errors" not in out:
                print(f"FAIL {name}: compile failed\n{out}")
                failures += 1
                continue
            got = word_at(parse_hex(hexf), DEVCFG_ADDR[word]) \
                ^ word_at(parse_hex(base_hex), DEVCFG_ADDR[word])
            tag = "PASS" if got == want else "FAIL"
            if got != want:
                failures += 1
            print(f"{tag} {name:15s} ^DEVCFG{word} = 0x{got:08X}"
                  f" (want 0x{want:08X})")

    total = len(CASES) + 3
    print(f"{'RESULT: PASS' if not failures else 'RESULT: FAIL'}"
          f" ({total - failures}/{total})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

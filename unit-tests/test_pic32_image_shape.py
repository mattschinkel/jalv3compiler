#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""PIC32 image-shape / bootloader-ownership pin (compile + HEX layout).

Pins docs/fixes/2026/fix_pic32_image_shape_split.md:

  * `pragma target image pfm` = placement only (PFM is the default);
  * `pragma target image bfm` = boot flash, explicit;
  * `pragma bootloader {addr}` = an EXTERNAL bootloader owns reset and
    DEVCFG: PFM at {addr}, no boot block, no config words;
  * `pragma bootloader pic32` is target-checked -- it used to compile on a
    16F877 and silently drop that part's CONFIG record.

Pre-fix this pin fails at `classic_pic32` (rc=0 where an error is
required on a classic core), at `pic32_bare` (no diagnostic), and at
`mz_bfm` (the `bfm` image word did not exist).

Usage:  python3 unit-tests/test_pic32_image_shape.py [--compiler PATH]
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
    str(ROOT / "sims" / "shared"),
])

PFM_BASE = 0x1D000000      # physical 0x9D000000
BFM_BASE = 0x1FC00000      # physical 0xBFC00000
MZ_CONFIG = 0x1FC0FFC0     # DEVCFG3..0 block


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
            for i, byte in enumerate(payload):
                mem[base + addr + i] = byte
        elif rectype == 4:
            base = ((payload[0] << 8) | payload[1]) << 16
    return mem


def compile_body(compiler: pathlib.Path, tmp: pathlib.Path, name: str,
                 lines: list[str]) -> tuple[int, str, pathlib.Path]:
    src = tmp / f"{name}.jal"
    hexf = tmp / f"{name}.hex"
    src.write_text("\n".join(lines) + "\n")
    proc = subprocess.run(
        [str(compiler), "-s", INCLUDE, "-hex", str(hexf), str(src)],
        cwd=ROOT, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr, hexf


def records_between(mem: dict[int, int], lo: int, hi: int) -> int:
    return sum(1 for a in mem if lo <= a < hi)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    args = ap.parse_args(argv)
    compiler = pathlib.Path(args.compiler)
    if not compiler.is_file():
        print(f"error: compiler not found: {compiler}")
        return 2

    mz = ["include 32mz1024efh064",
          "pragma target clock 200_000_000",
          "var volatile byte probe",
          "probe = 42",
          "forever loop",
          "end loop"]
    classic = ["include 16f877",
               "var byte x",
               "x = 1",
               "forever loop",
               "end loop"]
    total = failures = 0

    def check(cond: bool, label: str, detail: str = "") -> None:
        nonlocal total, failures
        total += 1
        if cond:
            print(f"PASS {label}")
        else:
            print(f"FAIL {label}{(': ' + detail) if detail else ''}")
            failures += 1

    with tempfile.TemporaryDirectory(prefix="imgshape.") as td:
        tmp = pathlib.Path(td)

        # 1. the target check the old `pic32` word never made
        rc, out, _ = compile_body(compiler, tmp, "classic_pic32",
                                  classic[:1] +
                                  ["pragma bootloader pic32"] + classic[1:])
        check(rc != 0 and "requires the PIC32 back end" in out,
              "classic core + `pragma bootloader pic32` is refused", out[-200:])

        # 2. the auto-detected form requires an address on PIC32
        rc, out, _ = compile_body(compiler, tmp, "pic32_bare",
                                  mz[:2] + ["pragma bootloader"] + mz[2:])
        check(rc != 0 and "requires a program-flash address" in out,
              "PIC32 bare `pragma bootloader` asks for an address", out[-200:])

        # 3. bootloader app: app in PFM, NOTHING in boot flash, no DEVCFG
        rc, out, hexf = compile_body(compiler, tmp, "pic32_bootldr",
                                     mz[:2] + ["pragma bootloader 0x9D000000"]
                                     + mz[2:])
        ok = rc == 0
        mem = parse_hex(hexf) if ok else {}
        check(ok and records_between(mem, PFM_BASE, PFM_BASE + 0x100) > 0,
              "bootloader app lands in program flash", out[-200:])
        check(records_between(mem, BFM_BASE, BFM_BASE + 0x1000) == 0,
              "bootloader app writes no boot-flash records")
        check(records_between(mem, MZ_CONFIG, MZ_CONFIG + 16) == 0,
              "bootloader app writes no config words")

        # 4. PFM is the default: app + boot block + device-file DEVCFG
        rc, out, hexf = compile_body(compiler, tmp, "mz_default", mz)
        ok = rc == 0
        mem = parse_hex(hexf) if ok else {}
        check(ok and records_between(mem, PFM_BASE, PFM_BASE + 0x100) > 0,
              "no pragma: app in program flash (new default)", out[-200:])
        check(records_between(mem, BFM_BASE, BFM_BASE + 0x400) > 0,
              "no pragma: compiler boot block at the reset vector")
        check(records_between(mem, MZ_CONFIG, MZ_CONFIG + 16) > 0,
              "no pragma: device-file config words emitted")

        # 4b. a bootloader app cannot live in boot flash
        rc, out, _ = compile_body(compiler, tmp, "pic32_bootldr_low",
                                  mz[:2] + ["pragma bootloader 0x0800"]
                                  + mz[2:])
        check(rc != 0 and "is not in program flash" in out,
              "bootloader address outside program flash is refused",
              out[-200:])

        # 5. explicit boot flash
        rc, out, hexf = compile_body(compiler, tmp, "mz_bfm",
                                     mz[:2] + ["pragma target image bfm"]
                                     + mz[2:])
        ok = rc == 0
        mem = parse_hex(hexf) if ok else {}
        check(ok and records_between(mem, BFM_BASE, BFM_BASE + 0x100) > 0,
              "`pragma target image bfm` lays out at the reset vector",
              out[-200:])
        check(records_between(mem, PFM_BASE, PFM_BASE + 0x100) == 0,
              "`pragma target image bfm` writes no PFM app records")
        check(records_between(mem, MZ_CONFIG, MZ_CONFIG + 16) > 0,
              "`pragma target image bfm` keeps the config words")

        # 6. MX still refuses boot flash (3 KB) -- now with an explicit word
        rc, out, _ = compile_body(
            compiler, tmp, "mx_bfm",
            ["include 32mx270f256b",
             "pragma target clock 40_000_000",
             "pragma target image bfm",
             "var dword i", "forever loop", "   i = i + 1", "end loop"])
        check(rc != 0 and "boot flash is only 3 KB" in out,
              "MX + `bfm` is still refused (3 KB boot flash)", out[-200:])

        # 7. the classic preamble is not a PIC32 option
        rc, out, _ = compile_body(compiler, tmp, "pic32_loader18",
                                  mz[:1] + ["pragma bootloader loader18"]
                                  + ["forever loop", "end loop"])
        check(rc != 0 and "only available on classic cores" in out,
              "`pragma bootloader loader18` refused on PIC32", out[-200:])

        # 8. the numeric form is the classic loader18 preamble
        rc, out, hexf = compile_body(
            compiler, tmp, "c18_numeric",
            ["include 18f4550",
             "pragma bootloader 0x0800",
             "var byte x", "x = 1", "forever loop", "end loop"])
        ok = rc == 0
        mem = parse_hex(hexf) if ok else {}
        check(ok and records_between(mem, 0x0800, 0x0810) > 0,
              "classic `pragma bootloader 0x0800` is the loader18 preamble",
              out[-200:])

    print(f"RESULT: {'PASS' if failures == 0 else 'FAIL'} "
          f"({total - failures}/{total})")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

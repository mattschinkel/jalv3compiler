#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""PIC32 peripheral-bus clock (PBCLK) pin (compile + HEX layout).

Pins the `pragma target pbdiv` surface and the DEVCFG1.FPBDIV solve:

  * `pragma target pbdiv DIV_1|DIV_2|DIV_4|DIV_8` is a PIC32 keyword
    (parser keyword, like `pragma target OSC`); DIV_2 is the default;
  * the solver overlays DEVCFG1.FPBDIV (bits 13:12) with log2(divide);
  * the solver publishes `_pbdiv` and `_pbclk = target_clock / pbdiv`
    whenever target_clock is known;
  * on MZ the pragma is ignored with a warning (multi-PBxDIV model).

Pre-fix this pin fails at `mx_div1` (the pbdiv word and _pbdiv/_pbclk do
not exist) and at `pbdiv_bad`/`classic_pbdiv` (no keyword to reject).

Usage:  python3 unit-tests/test_pic32_pbclk.py [--compiler PATH]
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

MX320_DEVCFG1 = 0x1FC02FF8     # physical DEVCFG1 (MX320 _FUSE_BASE[1])
DIV2_WORD = 0xFF605F59         # FPBDIV = DIV_2 (proven base)
DIV1_WORD = 0xFF604F59         # FPBDIV = DIV_1


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


def word_at(mem: dict[int, int], addr: int) -> int:
    return (mem.get(addr, 0) | (mem.get(addr + 1, 0) << 8)
            | (mem.get(addr + 2, 0) << 16) | (mem.get(addr + 3, 0) << 24))


def compile_body(compiler: pathlib.Path, tmp: pathlib.Path, name: str,
                 lines: list[str]) -> tuple[int, str, pathlib.Path]:
    src = tmp / f"{name}.jal"
    hexf = tmp / f"{name}.hex"
    src.write_text("\n".join(lines) + "\n")
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

    mx_div1 = [
        "include 32mx320f128h",
        "pragma target clock 80_000_000",
        "pragma target OSC    FRC_SPLL",
        "pragma target pbdiv  DIV_1",
        "if _pbdiv != 1 then",
        "   _error \"_pbdiv is not 1\"",
        "end if",
        "if _pbclk != 80_000_000 then",
        "   _error \"_pbclk is not 80_000_000\"",
        "end if",
        "var dword i",
        "forever loop",
        "   i = i + 1",
        "end loop",
    ]
    mx_default = [
        "include 32mx320f128h",
        "pragma target clock 80_000_000",
        "pragma target OSC    FRC_SPLL",
        "if _pbdiv != 2 then",
        "   _error \"_pbdiv default is not 2\"",
        "end if",
        "if _pbclk != 40_000_000 then",
        "   _error \"_pbclk default is not 40_000_000\"",
        "end if",
        "var dword i",
        "forever loop",
        "   i = i + 1",
        "end loop",
    ]
    pbdiv_bad = [
        "include 32mx320f128h",
        "pragma target clock 80_000_000",
        "pragma target pbdiv  DIV_3",
        "var dword i",
        "forever loop",
        "   i = i + 1",
        "end loop",
    ]
    classic_pbdiv = [
        "include 16f877",
        "pragma target pbdiv DIV_1",
        "var byte x",
        "x = 1",
        "forever loop",
        "end loop",
    ]
    mz_pbdiv = [
        "include 32mz1024efh064",
        "pragma target clock 200_000_000",
        "pragma target pbdiv DIV_1",
        "var volatile byte probe",
        "probe = 42",
        "forever loop",
        "end loop",
    ]

    total = failures = 0

    def check(cond: bool, label: str, detail: str = "") -> None:
        nonlocal total, failures
        total += 1
        if cond:
            print(f"PASS {label}")
        else:
            print(f"FAIL {label}{(': ' + detail) if detail else ''}")
            failures += 1

    with tempfile.TemporaryDirectory(prefix="pbclk.") as td:
        tmp = pathlib.Path(td)

        # 1. explicit DIV_1: published consts + FPBDIV fuse field
        rc, out, hexf = compile_body(compiler, tmp, "mx_div1", mx_div1)
        ok = rc == 0
        mem = parse_hex(hexf) if ok else {}
        check(ok, "pbdiv DIV_1 compiles with _pbdiv/_pbclk", out[-200:])
        check(ok and word_at(mem, MX320_DEVCFG1) == DIV1_WORD,
              "pbdiv DIV_1 puts FPBDIV=DIV_1 in DEVCFG1",
              hex(word_at(mem, MX320_DEVCFG1)))

        # 2. no pbdiv: DIV_2 default, byte-identical proven base
        rc, out, hexf = compile_body(compiler, tmp, "mx_default", mx_default)
        ok = rc == 0
        mem = parse_hex(hexf) if ok else {}
        check(ok, "no pbdiv compiles with the DIV_2 defaults", out[-200:])
        check(ok and word_at(mem, MX320_DEVCFG1) == DIV2_WORD,
              "no pbdiv keeps FPBDIV=DIV_2 (proven base)",
              hex(word_at(mem, MX320_DEVCFG1)))

        # 3. unknown divide is rejected
        rc, out, _ = compile_body(compiler, tmp, "pbdiv_bad", pbdiv_bad)
        check(rc != 0 and "unknown pbdiv" in out,
              "pbdiv DIV_3 is refused", out[-200:])

        # 4. classic cores cannot use the PIC32 keyword
        rc, out, _ = compile_body(compiler, tmp, "classic_pbdiv",
                                  classic_pbdiv)
        check(rc != 0 and "requires the PIC32 back end" in out,
              "classic core + pbdiv is refused", out[-200:])

        # 5. MZ ignores it with a warning
        rc, out, _ = compile_body(compiler, tmp, "mz_pbdiv", mz_pbdiv)
        check(rc == 0 and "ignored on this family" in out,
              "MZ + pbdiv compiles with a warning", out[-200:])

    print(f"{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

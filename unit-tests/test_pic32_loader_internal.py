#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""PIC32 loader surface pin: `pragma target loader internal` + `-bootloader`.

`loader internal` is the explicit spelling of the default self-boot shape
(compiler boot block + DEVCFG), mutually exclusive with `pragma bootloader
{addr}`.  The `-bootloader {addr}` CLI flag OVERRIDES a source-level
`loader internal` after parsing (an external loader owns reset + DEVCFG),
so the device block can stay self-boot while a bootloader board flips it
from the command line.

Pre-fix this pin fails at `internal_ok`, `conflict1` and `cli_override`
(the `loader` word and the `-bootloader` flag do not exist).

Usage:  python3 unit-tests/test_pic32_loader_internal.py [--compiler PATH]
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

BFM_BASE = 0x1FC00000      # physical boot flash
PFM_BASE = 0x1D000000      # physical program flash


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


def records_between(mem: dict[int, int], lo: int, hi: int) -> int:
    return sum(1 for a in mem if lo <= a < hi)


def compile_body(compiler: pathlib.Path, tmp: pathlib.Path, name: str,
                 lines: list[str], extra: list[str] | None = None
                 ) -> tuple[int, str, pathlib.Path]:
    src = tmp / f"{name}.jal"
    hexf = tmp / f"{name}.hex"
    src.write_text("\n".join(lines) + "\n")
    cmd = [str(compiler), "-s", INCLUDE, "-hex", str(hexf)]
    if extra:
        cmd += extra
    cmd.append(str(src))
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr, hexf


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    args = ap.parse_args(argv)
    compiler = pathlib.Path(args.compiler)
    if not compiler.is_file():
        print(f"error: compiler not found: {compiler}")
        return 2

    base = ["include 32mx320f128h",
            "pragma target clock 80_000_000",
            "pragma target OSC    FRC_SPLL"]
    body = ["var dword i", "forever loop", "   i = i + 1", "end loop"]

    total = failures = 0

    def check(cond: bool, label: str, detail: str = "") -> None:
        nonlocal total, failures
        total += 1
        if cond:
            print(f"PASS {label}")
        else:
            print(f"FAIL {label}{(': ' + detail) if detail else ''}")
            failures += 1

    with tempfile.TemporaryDirectory(prefix="loadint.") as td:
        tmp = pathlib.Path(td)

        rc, out, _ = compile_body(compiler, tmp, "internal_ok",
                                  base + ["pragma target loader internal"] + body)
        check(rc == 0, "`loader internal` compiles on PIC32", out[-200:])

        rc, out, _ = compile_body(compiler, tmp, "conflict1",
                                  base + ["pragma target loader internal",
                                          "pragma bootloader 0x9D000000"] + body)
        check(rc != 0 and "conflicting loader" in out,
              "internal then bootloader conflicts", out[-200:])

        rc, out, _ = compile_body(compiler, tmp, "conflict2",
                                  base + ["pragma bootloader 0x9D000000",
                                          "pragma target loader internal"] + body)
        check(rc != 0 and "conflicting loader" in out,
              "bootloader then internal conflicts", out[-200:])

        rc, out, _ = compile_body(compiler, tmp, "classic",
                                  ["include 16f877",
                                   "pragma target loader internal",
                                   "var byte x", "x = 1",
                                   "forever loop", "end loop"])
        check(rc != 0 and "requires the PIC32 back end" in out,
              "classic core + loader is refused", out[-200:])

        rc, out, _ = compile_body(compiler, tmp, "unknown",
                                  base + ["pragma target loader external"] + body)
        check(rc != 0 and "unknown loader" in out,
              "unknown loader word is refused", out[-200:])

        # -bootloader {addr} overrides a source-level `loader internal`
        rc, out, hexf = compile_body(
            compiler, tmp, "cli_override",
            base + ["pragma target loader internal"] + body,
            extra=["-bootloader", "0x9D002000"])
        ok = rc == 0
        mem = parse_hex(hexf) if ok else {}
        check(ok, "`-bootloader` overrides `loader internal`", out[-200:])
        check(ok and records_between(mem, BFM_BASE, BFM_BASE + 0x1000) == 0,
              "`-bootloader` image writes no boot-flash records")
        check(ok and records_between(mem, PFM_BASE + 0x2000,
                                     PFM_BASE + 0x2000 + 0x100) > 0,
              "`-bootloader` app lands at the requested slot")

        rc, out, _ = compile_body(compiler, tmp, "cli_classic",
                                  ["include 16f877",
                                   "var byte x", "x = 1",
                                   "forever loop", "end loop"],
                                  extra=["-bootloader", "0x9D002000"])
        check(rc != 0 and "only available on PIC32" in out,
              "`-bootloader` on a classic core is refused", out[-200:])

        rc, out, _ = compile_body(compiler, tmp, "cli_low",
                                  base + body,
                                  extra=["-bootloader", "0x0800"])
        check(rc != 0 and "is not in program flash" in out,
              "`-bootloader` address outside program flash is refused",
              out[-200:])

    print(f"{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""ESP32 ISR CPU-interrupt selection pin (compile-only gate).

`pragma interrupt N` on the ESP32 selects the **CPU interrupt number** the
handler is armed on, and the compiler programs two things from it: the
INTENABLE bit (which unmasks that CPU interrupt) and -- for level 1 -- the
single vector slot at VECBASE+0x340.

That number used to be IGNORED: `pragma interrupt 5` still armed CPU
interrupt 1, so the image was silently armed on the wrong line (the
interrupt never arrived and nothing said so).  Since 2026-11-03:

* numbers 0..5 (the level-1 CPU interrupts) are honoured: INTENABLE bit
  = 1<<N, entry at VECBASE+0x340, exit `rfe`;
* numbers 6..31 (levels 2..7) are REFUSED with one error -- the per-level
  slots could not be established (see
  fixes/fix_esp32_isr_cpu_int_selection.md);
* a second `pragma interrupt` stays a hard error.

Bench-proven on silicon 2026-11-03: `pragma interrupt 1` and
`pragma interrupt 5` both dispatch (hit=1) with the matrix source routed
to the same number; `pragma interrupt 6` is a compile error.

Usage:  python3 unit-tests/test_esp32_isr_cpu_int.py [--compiler PATH]
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

# INTENABLE write is `movi a2, <1<<N>` then `wsr a2, 228`; the entry slot
# is the level-1 table slot, VECBASE+0x340 (table base is 0x400-aligned,
# so the l32r of the handler literal lands on an address ending 0x340).
WSR_INTENABLE = "wsr a2, 228"
RFE = "00 30 00"       # rfe -- the only exit (level-1 EXC_IRQ)


def build_body(int_num: str) -> str:
    return "\n".join([
        "include esp32_xtensa_lx6",
        "var volatile byte hits = 0",
        "procedure _probe_isr() is",
        "   pragma interrupt {n}".format(n=int_num).rstrip(),
        "   hits = hits + 1",
        "end procedure",
        "forever loop",
        "end loop",
        "",
    ])


def compile_case(compiler: pathlib.Path, tmp: pathlib.Path, name: str,
                 body: str) -> tuple[int, str, str]:
    src = tmp / f"{name}.jal"
    asm = tmp / f"{name}.asm"
    src.write_text(body)
    if asm.exists():
        asm.unlink()
    proc = subprocess.run(
        [str(compiler), "-s", INCLUDE, str(src)],
        cwd=ROOT, capture_output=True, text=True)
    listing = asm.read_text(errors="replace") if asm.exists() else ""
    return proc.returncode, proc.stdout + proc.stderr, listing


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    args = ap.parse_args(argv)
    compiler = pathlib.Path(args.compiler)
    if not compiler.is_file():
        print(f"error: compiler not found: {compiler}")
        return 2

    # (name, int number, must_compile, listing markers, error substrings)
    cases = [
        ("int1", "1", True, ["movi a2, 0x2", WSR_INTENABLE, RFE], []),
        ("int5", "5", True, ["movi a2, 0x20", WSR_INTENABLE, RFE], []),
        ("int6", "6", False, [],
         ["pragma interrupt 6", "CPU interrupts 0..5"]),
        ("int30", "30", False, [],
         ["pragma interrupt 30", "CPU interrupts 0..5"]),
    ]

    failures = 0
    with tempfile.TemporaryDirectory(prefix="esp32int.") as td:
        tmp = pathlib.Path(td)
        for name, num, must_compile, markers, err_needles in cases:
            rc, out, listing = compile_case(
                compiler, tmp, name, build_body(num))
            problems: list[str] = []

            if must_compile:
                if rc != 0:
                    problems.append(f"expected 0 errors, rc={rc}")
                for m in markers:
                    if m not in listing:
                        problems.append(f"missing marker {m!r}")
                # the entry must be the level-1 slot: an l32r a5 sitting
                # 0x340 above a 0x400-aligned table base
                found = False
                for line in listing.splitlines():
                    if "l32r a5" not in line:
                        continue
                    try:
                        addr = int(line.split(":")[0].strip(), 16)
                    except ValueError:
                        continue
                    if (addr - 0x340) % 0x400 == 0:
                        found = True
                        break
                if not found:
                    problems.append(
                        "no vector entry at the table-base + 0x340 slot")
            else:
                if rc == 0:
                    problems.append("expected a compile error, got 0 errors")
                n_err = sum(1 for ln in out.splitlines() if ": error: " in ln)
                if n_err != 1:
                    problems.append(
                        f"expected exactly 1 error line, got {n_err}")
                for needle in err_needles:
                    if needle.lower() not in out.lower():
                        problems.append(f"diagnostic missing {needle!r}")

            if problems:
                failures += 1
                print(f"FAIL {name}")
                for p in problems:
                    print(f"     - {p}")
                tail = "\n".join(out.splitlines()[-6:])
                if tail:
                    print("     output tail:\n" + "\n".join(
                        "       " + ln for ln in tail.splitlines()))
            else:
                print(f"PASS {name}")

    total = len(cases)
    print(f"{'RESULT: PASS' if not failures else 'RESULT: FAIL'}"
          f" ({total - failures}/{total})")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())

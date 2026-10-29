#!/usr/bin/env python3
"""PIC32MX270F256B clock/fuse word check (compile-only gate).

The MX clock tree is solved at compile time and written into the
DEVCFG words.  QEMU does not model fuses (nor the PLL), so this script
is the acceptance gate for the solver: it compiles a matrix of
`pragma target clock` / `pragma target OSC` pairs with the MX device
file and asserts the exact DEVCFG0..3 words.  The expected values were
cross-checked with XC32 4.35 `#pragma config` for the same part.

Usage:  python3 sims/test_pic32mx_clock_words.py [--compiler PATH]
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
    0: 0x1FC00BFC,   # DEVCFG0
    1: 0x1FC00BF8,   # DEVCFG1
    2: 0x1FC00BF4,   # DEVCFG2
    3: 0x1FC00BF0,   # DEVCFG3
}

# (name, pragma lines, expected DEVCFG0..3)
CASES = [
    ("frc_8mhz", ["pragma target clock 8_000_000",
                  "pragma target OSC FRC"], None),  # expected derived below
    # silicon-proven 2026-10-25 (fixes/fix_mx_osc_pllmult_code_const.md
    # addenda): MX270 boots FNOSC=FRCDIV already divided
    ("frcdiv_4mhz", ["pragma target clock 4_000_000",
                     "pragma target OSC FRCDIV"], None),
    ("frc_spll_40mhz", ["pragma target clock 40_000_000",
                        "pragma target OSC FRC_SPLL"], None),
    ("posc_20mhz", ["pragma target oscillator 20_000_000",
                    "pragma target OSC POSC",
                    "pragma target clock 20_000_000"], None),
    ("posc_spll_20mhz", ["pragma target oscillator 20_000_000",
                         "pragma target OSC POSC_SPLL",
                         "pragma target clock 40_000_000"], None),
    ("posc_spll_24mhz", ["pragma target oscillator 24_000_000",
                         "pragma target OSC POSC_SPLL",
                         "pragma target clock 40_000_000"], None),
    ("ec_spll_20mhz", ["pragma target oscillator 20_000_000",
                       "pragma target OSC EC_SPLL",
                       "pragma target clock 40_000_000"], None),
    ("frc_spll_40_wdt", ["pragma target clock 40_000_000",
                         "pragma target OSC FRC_SPLL",
                         "pragma target WDT ENABLED"], None),
    # silicon-proven 2026-10-25 (fixes/fix_mx_wdt_silicon_ab.md): WDT on +
    # PS1024 resets the board every ~1.0 s, PS2048 every ~2.0 s (exact
    # doubling; WDT base ~1 ms, non-windowed)
    ("frc_spll_40_wdt_ps1024", ["pragma target clock 40_000_000",
                                "pragma target OSC FRC_SPLL",
                                "pragma target WDT ENABLED",
                                "pragma target WDTPS PS1024"], None),
    ("frc_spll_40_wdt_ps2048", ["pragma target clock 40_000_000",
                                "pragma target OSC FRC_SPLL",
                                "pragma target WDT ENABLED",
                                "pragma target WDTPS PS2048"], None),
    # silicon-proven 2026-10-26 (fixes/fix_mx_wdt_runtime_sequence.md):
    # the CONTROL-mode combination the wdt sample ships -- fuse stays
    # DISABLED, WDTPS sets the runtime period (SWDTPS in WDTCON is a
    # read-only mirror of it), reset lands ~4.1 s after the last
    # wdt_clear() with RCON.WDTO set
    ("frc_spll_40_wdt_control_ps4096",
                          ["pragma target clock 40_000_000",
                           "pragma target OSC FRC_SPLL",
                           "pragma target WDT DISABLED",
                           "pragma target WDTPS PS4096"], None),
    # device-file fuse_def vocabulary (pragma target <field> <value>);
    # expected words cross-checked against the EDC semantic encodings
    ("fuse_wdtps_ps256", ["pragma target clock 40_000_000",
                          "pragma target OSC FRC_SPLL",
                          "pragma target WDTPS PS256"], None),
    ("fuse_fpbdiv8", ["pragma target clock 40_000_000",
                      "pragma target OSC FRC_SPLL",
                      "pragma target FPBDIV DIV_8"], None),
    ("fuse_jtagen_on", ["pragma target clock 40_000_000",
                        "pragma target OSC FRC_SPLL",
                        "pragma target JTAGEN ON"], None),
    ("fuse_icesel2", ["pragma target clock 40_000_000",
                      "pragma target OSC FRC_SPLL",
                      "pragma target ICESEL ICS_PGX2"], None),
    ("fuse_upllen_on", ["pragma target clock 40_000_000",
                        "pragma target OSC FRC_SPLL",
                        "pragma target UPLLEN ON"], None),
    ("fuse_cp_on", ["pragma target clock 40_000_000",
                    "pragma target OSC FRC_SPLL",
                    "pragma target CP ON"], None),
    ("fuse_combo", ["pragma target clock 40_000_000",
                    "pragma target OSC FRC_SPLL",
                    "pragma target WDTPS PS1048576",
                    "pragma target FPBDIV DIV_1",
                    "pragma target PMDL1WAY OFF"], None),
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
                 pragmas: list[str], pfm: bool = True) -> tuple[int, str, pathlib.Path]:
    src = tmp / f"{name}.jal"
    hexf = tmp / f"{name}.hex"
    body = ["include 32mx270f256b", *pragmas]
    if pfm:
        body.append("pragma target image pfm")
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

    # expected words derived from the XC32 cross-check in the
    # investigation doc (2026-10-21)
    expected = {
        "frc_8mhz":          (0x7FFFFFFB, 0xFF605F58, 0xFFF9F9D9, 0xFFFFFFFF),
        "frcdiv_4mhz":       (0x7FFFFFFB, 0xFF605F5F, 0xFFF9F9D9, 0xFFFFFFFF),
        "frc_spll_40mhz":    (0x7FFFFFFB, 0xFF605F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "posc_20mhz":        (0x7FFFFFFB, 0xFF605E5A, 0xFFF9F9D9, 0xFFFFFFFF),
        "posc_spll_20mhz":   (0x7FFFFFFB, 0xFF605E5B, 0xFFF9F9DC, 0xFFFFFFFF),
        "posc_spll_24mhz":   (0x7FFFFFFB, 0xFF605E5B, 0xFFF9F9DD, 0xFFFFFFFF),
        "ec_spll_20mhz":     (0x7FFFFFFB, 0xFF605C5B, 0xFFF9F9DC, 0xFFFFFFFF),
        "frc_spll_40_wdt":   (0x7FFFFFFB, 0xFFE05F59, 0xFFF9F9D9, 0xFFFFFFFF),
        # silicon-proven 2026-10-25: programmed words measured resetting
        # the board at ~1.0 s / ~2.0 s respectively
        "frc_spll_40_wdt_ps1024":
                             (0x7FFFFFFB, 0xFFEA5F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "frc_spll_40_wdt_ps2048":
                             (0x7FFFFFFB, 0xFFEB5F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "frc_spll_40_wdt_control_ps4096":
                             (0x7FFFFFFB, 0xFF6C5F59, 0xFFF9F9D9, 0xFFFFFFFF),
        # fuse_def vocabulary on the frc_spll_40mhz base
        # (0x7FFFFFFB, 0xFF605F59, 0xFFF9F9D9, 0xFFFFFFFF)
        "fuse_wdtps_ps256":  (0x7FFFFFFB, 0xFF685F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "fuse_fpbdiv8":      (0x7FFFFFFB, 0xFF607F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "fuse_jtagen_on":    (0x7FFFFFFF, 0xFF605F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "fuse_icesel2":      (0x7FFFFFF3, 0xFF605F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "fuse_upllen_on":    (0x7FFFFFFB, 0xFF605F59, 0xFFF979D9, 0xFFFFFFFF),
        "fuse_cp_on":        (0x6FFFFFFB, 0xFF605F59, 0xFFF9F9D9, 0xFFFFFFFF),
        "fuse_combo":        (0x7FFFFFFB, 0xFF744F59, 0xFFF9F9D9, 0xEFFFFFFF),
    }

    failures = 0
    with tempfile.TemporaryDirectory(prefix="mxclk.") as td:
        tmp = pathlib.Path(td)
        for name, pragmas, _ in CASES:
            rc, out, hexf = compile_case(compiler, tmp, name, pragmas)
            if rc != 0:
                print(f"FAIL {name}: compile failed\n{out}")
                failures += 1
                continue
            mem = parse_hex(hexf)
            got = tuple(word_at(mem, DEVCFG_ADDR[i]) for i in range(4))
            exp = expected[name]
            ok = got == exp
            detail = ", ".join(
                f"DEVCFG{i}=0x{got[i]:08X}" for i in range(4))
            if ok:
                print(f"PASS {name}: {detail}")
            else:
                print(f"FAIL {name}: {detail}")
                print(f"     expected: " + ", ".join(
                    f"DEVCFG{i}=0x{exp[i]:08X}" for i in range(4)))
                failures += 1

        # impossible target: must fail with a clear MX constraint message
        rc, out, _ = compile_case(
            compiler, tmp, "bad_33mhz",
            ["pragma target clock 33_000_000", "pragma target OSC FRC_SPLL"])
        if rc != 0 and "cannot produce" in out and "PIC32MX" in out:
            print("PASS bad_33mhz: rejected with a constraint diagnostic")
        else:
            print(f"FAIL bad_33mhz: rc={rc} out={out}")
            failures += 1

        # boot-flash-only images cannot hold the 3 KB boot flash + the
        # device-file const pool: the compiler must say so and point at
        # the PFM shape.
        rc, out, _ = compile_case(
            compiler, tmp, "bad_bootflash",
            ["pragma target clock 40_000_000", "pragma target OSC FRC_SPLL"],
            pfm=False)
        if rc != 0 and "boot flash is only 3 KB" in out and "image pfm" in out:
            print("PASS bad_bootflash: rejected, PFM shape suggested")
        else:
            print(f"FAIL bad_bootflash: rc={rc} out={out}")
            failures += 1

        # raw fuses pragma touching solver-owned bits: refused with the
        # ownership diagnostic (the clock tree is compiler-solved)
        rc, out, _ = compile_case(
            compiler, tmp, "bad_fuse_conflict",
            ["pragma target clock 40_000_000", "pragma target OSC FRC_SPLL",
             "pragma target fuses 1 0xFF7F5F5D"])
        if rc != 0 and "overlap the clock-solved" in out:
            print("PASS bad_fuse_conflict: ownership diagnostic")
        else:
            print(f"FAIL bad_fuse_conflict: rc={rc} out={out}")
            failures += 1

        # solver-owned field names carry no fuse_def vocabulary: the
        # pragma must be rejected as unknown (they are keyword pragmas)
        rc, out, _ = compile_case(
            compiler, tmp, "bad_fnosc",
            ["pragma target clock 40_000_000", "pragma target OSC FRC_SPLL",
             "pragma target FNOSC PRIPLL"])
        if rc != 0 and "unknown pragma target" in out:
            print("PASS bad_fnosc: excluded field rejected")
        else:
            print(f"FAIL bad_fnosc: rc={rc} out={out}")
            failures += 1

        # fuse pragma without a clock solve: nothing carries the bits --
        # must warn instead of silently dropping them
        rc, out, _ = compile_case(
            compiler, tmp, "warn_fuse_noclock",
            ["pragma target WDTPS PS256"])
        if rc == 0 and "fuse pragma bits are not emitted" in out:
            print("PASS warn_fuse_noclock: warned about unsolved image")
        else:
            print(f"FAIL warn_fuse_noclock: rc={rc} out={out}")
            failures += 1

    total = len(CASES) + 5
    print(f"{'RESULT: PASS' if not failures else 'RESULT: FAIL'}"
          f" ({total - failures}/{total})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

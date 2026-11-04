#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""PIC32 interrupt-shape gate (compile-only).

Pins the 2026-10-17 PIC32 ISR work, which QEMU cannot fully model:

* fixes/fix_pic32_isr_srs_default.md -- a plain `pragma interrupt` on a
  PIC32MZ loader/PFM image takes the shadow-register-set (SRS) path;
  `pragma interrupt raw` opts out to software save/restore.  QEMU leaves
  PRISS unmapped, so the emulator cannot tell the two apart -- the
  listing can.
* fixes/fix_pic32_isr_fast_srs_silicon.md -- the SRS seed writes
  PRISS = 0x76543211 and seeds the shadow sets' $sp/$gp with `wrpgpr`.
* fixes/fix_pic32_bootflash_fuses.md -- SRS has no microMIPS encoding:
  plain/fast + `pragma isa micromips` must fail with a message naming
  `pragma interrupt raw`; raw + micromips must compile.
* fixes/fix_isr_label_leak_noise.md -- no `label leak: ...` stderr noise
  on any interrupt compile.
* PIC32MX (fix_pic32mx270f256b_support.md) has no PRISS/shadow sets:
  plain interrupts use software save/restore, `fast` is rejected, and
  numbered/MVEC interrupts are rejected.
* fixes/fix_pic32_bootflash_isr_refusal.md -- a boot-flash (non-PFM)
  image cannot dispatch interrupts: the EIC fetches its vector page at
  EBASE+0x200 and only the PFM boot init points it into the app, so
  `pragma interrupt` there is REFUSED (it used to compile, run, and
  silently never fire).  Any interrupt kind must be refused, exactly one
  error, and a boot-flash program WITHOUT an interrupt must still
  compile.
* fixes/fix_pic32_mz_isr_state_clear.md -- the arming must not inherit
  stale exception state.  Two emissions are pinned: `Cause = IV` (a
  WRITE, so a software-interrupt request latched at power-on is cleared
  -- a read-modify-write preserved it and the released core delivered it
  forever, dark) and, on MZ only, clearing Status.IM[7:0] in the write
  that clears EXL|ERL (a preserved IM1 wedges the machine at the
  exception-level exit).  Both are XC32's crt0 shape.  MX has no EIC and
  keeps its own arm mask, so the MZ mask must NOT appear there.

Usage:  python3 unit-tests/test_pic32_isr_shapes.py [--compiler PATH]
Exit:   0 = all PASS, 1 = failure.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_COMPILER = ROOT / "bin" / "beta" / "jalv3-x86-64"
INCLUDE = ";".join([
    str(ROOT / "demo_lib"),
    str(ROOT / "third_party" / "jallib" / "lib"),
    str(ROOT / "sims"),
])

# SRS markers as emitted in the .asm listing (the disassembler renders
# wrpgpr as `eret`, so match the raw words from the XC32-pinned encoder).
PRISS_LO = "3c09 7654"
PRISS_HI = "3529 3211"
WRPGPR_SP = "41dd e800"
WRPGPR_GP = "41dc e000"
# MZ Status arm: `ori $t1, $t1, 0x00f9` (~EXL, ~ERL, IM[7:0]=0).  MX has
# no EIC and keeps the plain ~(EXL|ERL) mask, so it must not appear there.
ARM_CAUSE_IV = "3c08 0080  lui $t0, $zero, 0x80"   # Cause = IV (arming)
ARM_CAUSE_IV_MM = "41a8 0080  lui $t0, $zero, 0x80"  # same, 16-bit LUI
STUB_CAUSE_IV = "3c1a 0080  lui $k0, $zero, 0x80"    # Cause = IV (handler)
STUB_CAUSE_IV_MM = "41ba 0080  lui $k0, $zero, 0x80" # same, 16-bit LUI
ARM_MZ_IM_CLEAR = "3529 00f9"
# microMIPS compresses the same ORI to the 16-bit form.
ARM_MZ_IM_CLEAR_MM = "5129 00f9"


def build_body(device: str, clock: str, interrupt: str,
               micromips: bool = False, image_pfm: bool = True) -> str:
    lines = [f"include {device}",
             f"pragma target clock {clock}",
             "pragma target OSC FRC_SPLL"]
    if image_pfm:
        lines.append("pragma target image pfm")
    if micromips:
        lines.append("pragma isa micromips")
    lines += [
        "var volatile byte ra = 0",
        "procedure _probe_isr() is",
        f"   pragma interrupt {interrupt}".rstrip(),
        "   ra = ra + 1",
        "end procedure",
        "while ra == 0 loop",
        "end loop",
        "",
    ]
    return "\n".join(lines)


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

    # (name, body, must_compile, required substrings, forbidden substrings,
    #  substrings expected in a FAILING compile's output)
    cases = [
        ("mz_plain_pfm",
         build_body("32mz1024efh064", "200_000_000", ""),
         True, [PRISS_LO, PRISS_HI, WRPGPR_SP, WRPGPR_GP, ARM_CAUSE_IV,
                STUB_CAUSE_IV, ARM_MZ_IM_CLEAR], [], []),
        ("mz_fast_pfm",
         build_body("32mz1024efh064", "200_000_000", "fast"),
         True, [PRISS_LO, PRISS_HI, WRPGPR_SP, WRPGPR_GP, ARM_CAUSE_IV,
                STUB_CAUSE_IV, ARM_MZ_IM_CLEAR], [], []),
        ("mz_raw_pfm",
         build_body("32mz1024efh064", "200_000_000", "raw"),
         True, [ARM_CAUSE_IV, STUB_CAUSE_IV, ARM_MZ_IM_CLEAR],
         [PRISS_LO, PRISS_HI, WRPGPR_SP, WRPGPR_GP], []),
        ("mz_mvec_pfm",
         build_body("32mz1024efh064", "200_000_000", "9"),
         True, [ARM_CAUSE_IV, STUB_CAUSE_IV, ARM_MZ_IM_CLEAR],
         [PRISS_LO, PRISS_HI], []),
        ("mz_plain_micromips",
         build_body("32mz1024efh064", "200_000_000", "", micromips=True),
         False, [], [], ["micromips", "pragma interrupt raw"]),
        ("mz_raw_micromips",
         build_body("32mz1024efh064", "200_000_000", "raw",
                    micromips=True),
         True, [ARM_CAUSE_IV_MM, STUB_CAUSE_IV_MM, ARM_MZ_IM_CLEAR_MM],
         [PRISS_LO, PRISS_HI, WRPGPR_SP, WRPGPR_GP], []),
        ("mx_plain_pfm",
         build_body("32mx270f256b", "40_000_000", ""),
         True, [ARM_CAUSE_IV, STUB_CAUSE_IV],
         [PRISS_LO, PRISS_HI, WRPGPR_SP, WRPGPR_GP, ARM_MZ_IM_CLEAR], []),
        ("mx_fast_pfm",
         build_body("32mx270f256b", "40_000_000", "fast"),
         False, [], [], ["not supported", "pragma interrupt raw"]),
        ("mx_mvec_pfm",
         build_body("32mx270f256b", "40_000_000", "1"),
         True, [ARM_CAUSE_IV, STUB_CAUSE_IV], [], []),
        # boot-flash images cannot dispatch interrupts: refused, not
        # silently dead (fix_pic32_bootflash_isr_refusal.md)
        ("mz_plain_bootflash",
         build_body("32mz1024efh064", "200_000_000", "", image_pfm=False),
         False, [], [], ["boot-flash", "pragma target image pfm"]),
        ("mz_raw_bootflash",
         build_body("32mz1024efh064", "200_000_000", "raw",
                    image_pfm=False),
         False, [], [], ["boot-flash", "pragma target image pfm"]),
        # ... while a boot-flash program with NO interrupt still builds
        ("mz_no_isr_bootflash",
         "include 32mz1024efh064\n"
         "pragma target clock 200_000_000\n"
         "pragma target OSC FRC_SPLL\n"
         "var byte x = 0\n"
         "forever loop\n"
         "   x = x + 1\n"
         "end loop\n",
         True, [], [], []),
    ]

    failures = 0
    with tempfile.TemporaryDirectory(prefix="pisrshapes.") as td:
        tmp = pathlib.Path(td)
        for name, body, must_compile, required, forbidden, err_expected in cases:
            rc, out, listing = compile_case(compiler, tmp, name, body)
            problems: list[str] = []

            # every compile -- passing or failing -- must be leak-clean
            if "label leak" in out:
                problems.append("label leak diagnostic on stderr")

            if must_compile:
                if rc != 0:
                    problems.append(f"expected 0 errors, rc={rc}")
                for marker in required:
                    if marker not in listing:
                        problems.append(f"missing listing marker {marker!r}")
                for marker in forbidden:
                    if marker in listing:
                        problems.append(f"unexpected listing marker {marker!r}")
            else:
                if rc == 0:
                    problems.append("expected a compile error, got 0 errors")
                n_err = sum(1 for ln in out.splitlines() if ": error: " in ln)
                if n_err != 1:
                    problems.append(
                        f"expected exactly 1 error line, got {n_err}")
                for needle in err_expected:
                    if needle.lower() not in out.lower():
                        problems.append(f"diagnostic missing {needle!r}")

            if problems:
                failures += 1
                print(f"FAIL {name}")
                for p in problems:
                    print(f"     - {p}")
                tail = "\n".join(out.splitlines()[-8:])
                if tail:
                    print("     output tail:\n" + "\n".join(
                        "       " + line for line in tail.splitlines()))
            else:
                print(f"PASS {name}")

    total = len(cases)
    print(f"{'RESULT: PASS' if not failures else 'RESULT: FAIL'}"
          f" ({total - failures}/{total})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

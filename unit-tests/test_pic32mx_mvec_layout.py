#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""PIC32MX multi-vector table layout gate (compile-only).

Pins the 2026-10-22 MX MVEC implementation:
  * 64 x 32-byte stubs at EBASE+0x200 (slot N = EBASE+0x200+N*32);
  * slot 0 is the QEMU-compatible dispatcher while assigned slots jump
    to their own handler stubs (both J targets differ from the default);
  * app code/stubs start at +0x1000 (past the 2 KB table);
  * single-vector MX images are unchanged: the boot redirect at
    EBASE+0x200 still jumps to the code+0x400 stub block.

QEMU cannot dispatch a real vector table (VEIC spacing is forced to 0),
so the dispatcher path is exercised by
tests/pic32/32mx270f256b_mvec.jal; this script pins the emitted layout.

Usage: python3 unit-tests/test_pic32mx_mvec_layout.py [--compiler PATH]
Exit 0 = PASS, 1 = failure.
"""
from __future__ import annotations

import argparse
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
# HEX records use the physical KSEG0 address (0x1D...), while J
# targets encode the virtual address (0x9D...).
PHY = 0x1D000000
VIR = 0x9D000000
TABLE_PHY = PHY + 0x200
TABLE = VIR + 0x200
CODE = VIR + 0x1000
SINGLE_STUB = VIR + 0x400
# Single-vector MX keeps EBASE in the top 4 KB of the 256 KB part.
SINGLE_EBASE = VIR + 0x40000 - 0x1000


def parse_hex(path: pathlib.Path) -> dict[int, int]:
    mem: dict[int, int] = {}
    base = 0
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        data = bytes.fromhex(line[1:])
        count, addr, rtype = data[0], (data[1] << 8) | data[2], data[3]
        payload = data[4:4 + count]
        if rtype == 0:
            for i, b in enumerate(payload):
                mem[base + addr + i] = b
        elif rtype == 4:
            base = ((payload[0] << 8) | payload[1]) << 16
    return mem


def word_at(mem: dict[int, int], addr: int) -> int:
    return sum(mem.get(addr + i, 0xFF) << (8 * i) for i in range(4))


def jump_target(pc: int, word: int) -> int | None:
    if (word >> 26) != 2:          # J opcode
        return None
    return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def build(pfm: bool, single: bool) -> str:
    lines = ["include 32mx270f256b",
             "pragma target clock 40_000_000",
             "pragma target OSC FRC_SPLL"]
    if pfm:
        lines.append("pragma target image pfm")
    lines += ["var volatile byte a = 0", "var volatile byte b = 0"]
    if single:
        lines += ["procedure isr() is", "   pragma interrupt",
                  "   IFS0CLR = 1", "   a = a + 1", "end procedure"]
    else:
        lines += [
            "procedure isr0() is", "   pragma interrupt 0",
            "   IFS0CLR = 1", "   a = a + 1", "end procedure",
            "procedure isr4() is", "   pragma interrupt 4",
            "   IFS0CLR = 0x10", "   b = b + 1", "end procedure",
            "IPC0 = 0x1C", "IPC1 = 0x04", "IEC0SET = 0x11",
            "IFS0SET = 0x11",
        ]
    lines += ["forever loop", "end loop", ""]
    return "\n".join(lines)


def build_all_vectors() -> str:
    lines = ["include 32mx270f256b",
             "pragma target clock 40_000_000",
             "pragma target OSC FRC_SPLL",
             "pragma target image pfm",
             "var volatile byte a = 0"]
    for i in range(64):
        lines += [f"procedure isr{i}() is", f"   pragma interrupt {i}",
                  "   IFS0CLR = 0xFF", "   a = a + 1", "end procedure"]
    lines += ["IPC0 = 0x1F", "IEC0SET = 0xFFFFFFFF",
              "IFS0SET = 0xFFFFFFFF",
              "forever loop", "end loop", ""]
    return "\n".join(lines)


def compile_case(compiler: pathlib.Path, tmp: pathlib.Path, name: str,
                 pfm: bool, single: bool) -> tuple[int, str, pathlib.Path]:
    src = tmp / f"{name}.jal"
    hexf = tmp / f"{name}.hex"
    src.write_text(build(pfm, single))
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
    with tempfile.TemporaryDirectory(prefix="mxmvec.") as td:
        tmp = pathlib.Path(td)

        total += 1
        rc, out, hexf = compile_case(compiler, tmp, "mvec", True, False)
        if rc != 0:
            print(f"FAIL mvec: compile failed\n{out}")
            failures += 1
        else:
            mem = parse_hex(hexf)
            slot0 = jump_target(TABLE, word_at(mem, TABLE_PHY))
            slot1 = jump_target(TABLE + 32, word_at(mem, TABLE_PHY + 32))
            slot4 = jump_target(TABLE + 4 * 32, word_at(mem, TABLE_PHY + 4 * 32))
            problems = []
            if slot0 is None:
                problems.append("slot0 is not a J")
            if slot4 is None:
                problems.append("slot4 is not a J")
            if slot1 is None:
                problems.append("unused slot1 is not a J (no default stub?)")
            if slot0 is not None and slot0 != CODE + 0x1D0:
                problems.append(f"slot0 target {slot0:#x} is not the dispatcher")
            if slot4 is not None and slot1 is not None and slot4 == slot1:
                problems.append("slot4 falls through to the default stub")
            if slot0 is not None and slot1 is not None and slot0 == slot1:
                problems.append("slot0 uses the default stub (no dispatcher)")
            if problems:
                print("FAIL mvec: " + "; ".join(problems))
                failures += 1
            else:
                print(f"PASS mvec: slot0={slot0:#x} slot4={slot4:#x} "
                      f"default={slot1:#x}")

        total += 1
        rc, out, hexf = compile_case(compiler, tmp, "single", True, True)
        if rc != 0:
            print(f"FAIL single: compile failed\n{out}")
            failures += 1
        else:
            mem = parse_hex(hexf)
            if word_at(mem, TABLE_PHY) != 0xFFFFFFFF:
                print("FAIL single: table present at app+0x200 "
                      "(single-vector image picked MVEC layout)")
                failures += 1
            else:
                # redirect is lui/ori/jr (the stub is in the app region)
                va = SINGLE_EBASE + 0x200
                pa = va - 0x80000000
                w0 = word_at(mem, pa)
                w1 = word_at(mem, pa + 4)
                w2 = word_at(mem, pa + 8)
                ok = ((w0 >> 16) == 0x3C08 and (w0 & 0xFFFF) == 0x9D00
                      and (w1 >> 16) == 0x3508
                      and (w1 & 0xFFFF) == 0x0400
                      and (w2 >> 26) == 0)
                if not ok:
                    print(f"FAIL single: EBASE {SINGLE_EBASE:#x}+0x200 is "
                          f"{w0:08x} {w1:08x} {w2:08x}, expected "
                          f"lui/ori/jr -> {SINGLE_STUB:#x}")
                    failures += 1
                else:
                    print(f"PASS single: EBASE {SINGLE_EBASE:#x} -> "
                          f"{SINGLE_STUB:#x}")

        # all 64 slots: every assigned vector must have its own target
        total += 1
        src = tmp / "all64.jal"
        hexf = tmp / "all64.hex"
        src.write_text(build_all_vectors())
        proc = subprocess.run(
            [str(compiler), "-s", INCLUDE, "-hex", str(hexf), str(src)],
            cwd=ROOT, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"FAIL all64: compile failed\n{proc.stdout + proc.stderr}")
            failures += 1
        else:
            mem = parse_hex(hexf)
            targets = []
            for i in range(64):
                targets.append(jump_target(TABLE + i * 32,
                    word_at(mem, TABLE_PHY + i * 32)))
            bad = [i for i, tg in enumerate(targets) if tg is None]
            dup = len(targets) != len(set(targets))
            if bad:
                print(f"FAIL all64: slots without a J target: {bad[:8]}")
                failures += 1
            elif dup:
                print("FAIL all64: duplicate slot targets")
                failures += 1
            else:
                print(f"PASS all64: 64 slots, {len(set(targets))} distinct "
                      f"targets (slot63 -> {targets[63]:#x})")

        # vector 64 is outside the 6-bit table index
        total += 1
        src = tmp / "bad64.jal"
        src.write_text(build(True, False).replace(
            "pragma interrupt 4", "pragma interrupt 64"))
        proc = subprocess.run(
            [str(compiler), "-s", INCLUDE, "-no-codfile", str(src)],
            cwd=ROOT, capture_output=True, text=True)
        out = proc.stdout + proc.stderr
        if proc.returncode != 0 and "out of range (0..63)" in out:
            print("PASS bad64: rejected with the 0..63 range diagnostic")
        else:
            print(f"FAIL bad64: rc={proc.returncode} out={out[-300:]}")
            failures += 1

    print(f"{'RESULT: PASS' if not failures else 'RESULT: FAIL'}"
          f" ({total - failures}/{total})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Run sims/uart_txreg/issue_36_record_const_print.jal under gpsim with
UART instrumentation: `log w txreg` records every store to TXREG; parse the log
and verify the expected print_string / print_crlf sequence.

Usage (from repo root):
  python3 scripts/gpsim_uart_validate_issue36.py
  python3 scripts/gpsim_uart_validate_issue36.py --compiler ./bin/jalv3-x86-64 --steps 30000000

Requires: jalv2, gpsim on PATH.


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_JAL = REPO / "sims/uart_txreg/issue_36_record_const_print.jal"
NEEDLE = "a_1\r\na_2\r\na_3\r\nb_001\r\nb_002\r\nb_003\r\n\r\n"

# Cross-architecture UART TXREG tests (same print sequence / needle).
ARCH_TESTS = [
    (DEFAULT_JAL, "p16f1823", 32_000_000),
    (REPO / "sims/uart_txreg/uart_18f4620_print.jal", "p18f4620", 20_000_000),
    (REPO / "sims/uart_txreg/uart_16f877_print.jal", "p16f877", 20_000_000),
]


def parse_txreg_writes(log_text: str) -> bytes:
    out: list[int] = []
    for m in re.finditer(r"Wrote:\s*(0x[0-9a-fA-F]+)\s+to txreg", log_text):
        out.append(int(m.group(1), 16) & 0xFF)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--jal",
        type=Path,
        default=DEFAULT_JAL,
        help="Path to issue_36_record_const_print.jal",
    )
    ap.add_argument(
        "--compiler",
        type=Path,
        default=REPO / "bin/jalv3-x86-64",
        help="jalv2 compiler binary",
    )
    ap.add_argument(
        "-p",
        "--processor",
        default="p16f1823",
        help="gpsim processor (default: p16f1823)",
    )
    ap.add_argument(
        "-f",
        "--frequency",
        type=int,
        default=32_000_000,
        help="gpsim frequency in Hz (default: 32_000_000)",
    )
    ap.add_argument(
        "-n",
        "--steps",
        type=int,
        default=30_000_000,
        help="instruction steps before log off (default: 30_000_000)",
    )
    ap.add_argument("--quiet", action="store_true", help="less stdout")
    ap.add_argument(
        "--all",
        action="store_true",
        help="run every architecture in ARCH_TESTS (16f1823 / 18f4620 / 16f877)",
    )
    args = ap.parse_args()

    if args.all:
        rc = 0
        for jal_p, proc, freq in ARCH_TESTS:
            print(f"=== {jal_p.name} on {proc}")
            rc |= run_one(jal_p, proc, freq, args.steps, args.compiler, args.quiet)
        return rc

    return run_one(args.jal, args.processor, args.frequency, args.steps,
                   args.compiler, args.quiet)


def run_one(jal_arg: Path, processor: str, frequency: int, steps: int,
            compiler: Path, quiet: bool) -> int:
    class _A:
        pass

    args = _A()
    args.steps = steps
    args.quiet = quiet
    args.compiler = compiler
    args.frequency = frequency
    args.processor = processor
    jal = Path(jal_arg).resolve()
    if not jal.is_file():
        print(f"error: missing {jal}", file=sys.stderr)
        return 2
    comp = args.compiler.resolve()
    if not comp.is_file():
        print(f"error: compiler not found: {comp}", file=sys.stderr)
        return 2
    if not shutil.which("gpsim"):
        print("error: gpsim not on PATH", file=sys.stderr)
        return 2

    gp_s = f"{REPO / 'third_party/jallib/lib'};{REPO / 'tests'}"
    with tempfile.TemporaryDirectory(prefix="jal_issue36_uart_") as td:
        tdir = Path(td)
        hex_path = tdir / "issue_36.hex"
        asm_path = tdir / "issue_36.asm"
        log_path = tdir / "txreg.log"
        cmd = [
            str(comp),
            "-s",
            gp_s,
            "-no-codfile",
            "-asm",
            str(asm_path),
            "-hex",
            str(hex_path),
            str(jal),
        ]
        if not args.quiet:
            print("compile:", " ".join(cmd))
        r = subprocess.run(cmd, cwd=str(REPO), capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout, file=sys.stderr)
            print(r.stderr, file=sys.stderr)
            print(f"error: compiler exit {r.returncode}", file=sys.stderr)
            return r.returncode
        if not hex_path.is_file():
            print("error: no .hex produced", file=sys.stderr)
            return 1

        gpsim_in = (
            f"load {hex_path}\n"
            f"frequency {args.frequency}\n"
            f"log on {log_path}\n"
            "log w txreg\n"
            f"step {args.steps}\n"
            "log off\n"
            "quit\n"
        )
        if not args.quiet:
            print(
                f"gpsim: -p {args.processor} … log w txreg … step {args.steps}",
                flush=True,
            )
        extra = os.environ.get("JAL_GPSIM_GPSIM_OPTS", "-S disable").split()
        gr = subprocess.run(
            ["gpsim", "-i", *extra, "-p", args.processor, str(hex_path)],
            input=gpsim_in,
            text=True,
            cwd=str(REPO),
            capture_output=True,
        )
        if gr.returncode != 0:
            print(gr.stderr, file=sys.stderr)
            print(gr.stdout, file=sys.stderr)
            print(f"error: gpsim exit {gr.returncode}", file=sys.stderr)
            return gr.returncode

        log_text = log_path.read_text(errors="replace")
        blob = parse_txreg_writes(log_text)
        s = blob.decode("latin-1", errors="replace")
        if NEEDLE not in s:
            print("error: expected UART fragment not found in TXREG log", file=sys.stderr)
            print(f"  want substring (repr): {NEEDLE!r}", file=sys.stderr)
            print(f"  got {len(blob)} bytes, head repr: {s[:120]!r}", file=sys.stderr)
            return 1
        if not args.quiet:
            i = s.find(NEEDLE)
            print(f"OK: TXREG log has {len(blob)} bytes; needle at offset {i}")
            print(f"    sample: {s[i : i + len(NEEDLE) + 20]!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

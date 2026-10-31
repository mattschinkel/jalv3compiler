#!/usr/bin/env python3
"""One-command gpsim serial-output check: compile a .jal, run it with
`log w txreg`, print the captured serial text, and (optionally) require
that every NEEDLE appears in it.

This is the check that was re-written by hand a dozen times during the
2026-10-30 string/pointer investigations -- keep it one command:

  python3 sims/txreg_check.py demo_sample/16f1825_string_dynamic_1.jal \\
          --needle "ALL STRING CHECKS PASSED" --needle "##DONE##"

  python3 sims/txreg_check.py tmp/probe.jal -s "demo_lib;third_party/jallib/lib" -n 12000000

Defaults follow the Known Pitfalls (gpsim_validate_contract.txt):
  - steps: 8,000,000 (>= the 5M minimum for programs carrying the split-
    sample header's _usec_delay(100_000) boot delay)
  - include path: demo_lib first (matches the sample kit / publish order)
Exit code 0 = all needles found (or no needles given), 1 = missing needle
or compile failure. The full captured text always prints on stdout.
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("jal", type=Path)
    ap.add_argument("--needle", action="append", default=[],
                    help="text that must appear in the captured serial output "
                         "(repeatable; CRLF in the program output is normalised "
                         "to plain newlines before matching)")
    ap.add_argument("--compiler", type=Path, default=REPO / "bin/jalv3-x86-64")
    ap.add_argument("-s", "--libdir", default="demo_lib;third_party/jallib/lib",
                    help="include path passed to -s (default: demo_lib first, "
                         "the sample-kit order)")
    ap.add_argument("-p", "--processor", default=None,
                    help="gpsim processor (default: infer from the device include)")
    ap.add_argument("-f", "--frequency", type=int, default=32_000_000)
    ap.add_argument("-n", "--steps", type=int, default=8_000_000,
                    help="gpsim instruction steps (default 8M; >= 5M for "
                         "split-sample-header programs -- see Known Pitfalls)")
    args = ap.parse_args()

    jal = args.jal.resolve()
    comp = args.compiler.resolve()
    if not jal.is_file():
        print(f"error: not a file: {jal}", file=sys.stderr)
        return 2
    if not comp.is_file():
        print(f"error: compiler not found: {comp}", file=sys.stderr)
        return 2
    if not shutil.which("gpsim"):
        print("error: gpsim not on PATH", file=sys.stderr)
        return 2

    proc = args.processor
    if not proc:
        text = jal.read_text(errors="ignore")
        m = re.search(r"^\s*include\s+([0-9]+f[0-9a-z]+)\b", text, re.I | re.M)
        if not m:
            print("error: could not infer the device include; pass -p p16f1825",
                  file=sys.stderr)
            return 2
        proc = "p" + m.group(1).lower()

    with tempfile.TemporaryDirectory(prefix="txreg_") as td:
        tdir = Path(td)
        hex_path = tdir / "s.hex"
        log_path = tdir / "tx.log"
        c = subprocess.run([str(comp), "-s", args.libdir, "-no-codfile",
                            "-hex", str(hex_path), str(jal)],
                           capture_output=True, text=True)
        if c.returncode != 0 or not hex_path.is_file():
            print(f"error: compile failed (rc={c.returncode})", file=sys.stderr)
            out = (c.stdout or "") + (c.stderr or "")
            print("\n".join(out.splitlines()[-15:]), file=sys.stderr)
            return 1
        g = subprocess.run(["gpsim", "-i", "-S", "disable", "-p", proc,
                            str(hex_path)],
                           input=(f"frequency {args.frequency}\n"
                                  f"log on {log_path}\nlog w txreg\n"
                                  f"step {args.steps}\nlog off\nquit\n"),
                           text=True, capture_output=True)
        if g.returncode != 0:
            print(f"error: gpsim exited {g.returncode}", file=sys.stderr)
            print((g.stderr or "")[-500:], file=sys.stderr)
            return 1
        log = log_path.read_text(errors="ignore")

    data = bytes(int(v, 16) & 0xFF for v in
                 re.findall(r"Wrote:\s*(0x[0-9a-fA-F]+)\s+to txreg", log))
    text = data.decode("latin-1").replace("\r\n", "\n")
    print(text)
    print(f"[txreg_check] {len(data)} bytes captured "
          f"({args.steps} steps @ {args.frequency} Hz, {proc})", file=sys.stderr)

    missing = [n for n in args.needle if n.replace("\r\n", "\n") not in text]
    if missing:
        for n in missing:
            print(f"[txreg_check] MISSING needle: {n!r}", file=sys.stderr)
        return 1
    if args.needle:
        print(f"[txreg_check] all {len(args.needle)} needle(s) found",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

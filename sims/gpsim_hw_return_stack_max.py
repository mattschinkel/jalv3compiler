#!/usr/bin/env python3
"""
Max hardware return-stack depth (STKPTR) from a gpsim batch run.

Uses ``stdbuf -oL`` when available so gpsim’s stdout is line-buffered under a pipe
(large ``step 1`` / ``stkptr`` sequences otherwise block for minutes).

gpsim exposes ``stkptr`` for PIC18 and PIC16 enhanced (e.g. p18f4620, p16f1825).
Classic mid-range PIC14 (e.g. p16f877) does not support ``stkptr`` in the CLI
expression parser — the script exits with an error in that case.

Usage:
  python3 scripts/gpsim_hw_return_stack_max.py -p p18f4620 -f 48000000 -n 50000 \\
      path/to/file.hex
  python3 scripts/gpsim_hw_return_stack_max.py ... --json

Compare to compiler stderr line ``Hardware stack depth X of Y`` (static CFG bound):
  dynamic max STKPTR along one simulated path should satisfy ``max <= X`` when the
  compiler is conservative for that path. ``pic_asm_hw_call_depth.py`` models nested
  CALL depth from asm — related but not identical to STKPTR (interrupts, tail paths).

Requires ``gpsim`` on PATH (same as jal_gpsim_validate.sh).


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from typing import Optional

STKPTR_RE = re.compile(r"^\s*stkptr\s*=\s*0x([0-9a-fA-F]+)\s*$", re.I | re.M)
ERROR_RE = re.compile(r"\*\*\*ERROR", re.I)


def _gpsim_cmd(gpsim: str, proc: str, hex_path: str) -> list[str]:
    """Line-buffer gpsim stdout when stdbuf is available (pipe mode is block-buffered otherwise)."""
    stdbuf = shutil.which("stdbuf")
    if stdbuf:
        return [stdbuf, "-oL", gpsim, "-i", "-p", proc, hex_path]
    return [gpsim, "-i", "-p", proc, hex_path]


def _norm_stkptr(raw: int, processor: str) -> int:
    """
    gpsim reports stkptr = 0x1f on some PIC16 enhanced parts immediately after a
    RETURN that clears the stack (not a real depth of 31). PIC18 uses stkptr
    up to 31 legitimately — do not remap there.
    """
    pl = processor.lower()
    if pl.startswith("p18"):
        return raw
    if raw == 0x1F:
        return 0
    # Mid-range enhanced: 16-entry hardware stack; clamp stray high bits.
    if raw > 16:
        return min(raw & 0x0F, 16)
    return raw


def _stkptr_supported(proc: str, hex_path: str, freq: int) -> tuple[bool, str]:
    """Return (ok, detail) after loading hex and evaluating stkptr once."""
    gpsim = shutil.which("gpsim")
    if not gpsim:
        return False, "gpsim not on PATH"
    script = f"frequency {int(freq)}\nstkptr\nquit\n"
    p = subprocess.run(
        _gpsim_cmd(gpsim, proc, hex_path),
        input=script,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    out = (p.stdout or "") + "\n" + (p.stderr or "")
    if ERROR_RE.search(out) and "stkptr" in out.lower():
        return False, "stkptr not available for this processor in gpsim CLI"
    if STKPTR_RE.search(out):
        return True, ""
    if p.returncode != 0:
        return False, f"gpsim exited {p.returncode}: {out[-800:]}"
    return False, f"unexpected gpsim output (no stkptr line): {out[-800:]}"


def max_stkptr_over_steps(proc: str, hex_path: str, freq: int, steps: int) -> int:
    """
    Single gpsim batch: ``frequency``, then ``step 1`` / ``stkptr`` repeated ``steps``
    times, then ``quit``. One subprocess avoids pipe deadlocks from chatty CLI I/O.
    """
    gpsim = shutil.which("gpsim")
    assert gpsim
    parts = [f"frequency {int(freq)}\n"]
    for _ in range(steps):
        parts.append("step 1\nstkptr\n")
    parts.append("quit\n")
    script = "".join(parts)
    # ~1 ms per step+stkptr pair on typical hosts; allow headroom for CI load.
    timeout = max(300, int(steps * 0.0025) + 120)
    p = subprocess.run(
        _gpsim_cmd(gpsim, proc, hex_path),
        input=script,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    out = (p.stdout or "") + "\n" + (p.stderr or "")
    if p.returncode != 0:
        raise RuntimeError(
            f"gpsim exited {p.returncode}\n{out[-2000:]}"
        )
    mx = 0
    n_readings = 0
    for line in out.splitlines():
        m = STKPTR_RE.match(line)
        if m:
            n_readings += 1
            v = _norm_stkptr(int(m.group(1), 16), proc)
            mx = max(mx, v)
    if steps > 0 and n_readings != steps:
        raise RuntimeError(
            f"expected {steps} stkptr readings, got {n_readings}\n{out[-1500:]}"
        )
    return mx


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Max STKPTR (hardware return stack depth) via gpsim stepping."
    )
    ap.add_argument("-p", "--processor", required=True, help="gpsim processor, e.g. p18f4620")
    ap.add_argument("-f", "--frequency-hz", type=int, default=4_000_000)
    ap.add_argument("-n", "--steps", type=int, default=50_000, help="instruction steps (step 1 each)")
    ap.add_argument("--json", action="store_true", help="print one JSON object to stdout")
    ap.add_argument("--max-le", type=int, metavar="M", help="exit 2 if max STKPTR > M")
    ap.add_argument("hex_path", help="Intel HEX file loaded by gpsim")
    args = ap.parse_args(argv)

    ok, detail = _stkptr_supported(args.processor, args.hex_path, args.frequency_hz)
    if not ok:
        print(f"error: {detail}", file=sys.stderr)
        return 1

    try:
        mx = max_stkptr_over_steps(
            args.processor, args.hex_path, args.frequency_hz, args.steps
        )
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.json:
        print(
            json.dumps(
                {
                    "max_stkptr": mx,
                    "steps": args.steps,
                    "processor": args.processor,
                    "frequency_hz": args.frequency_hz,
                },
                sort_keys=True,
            )
        )
    else:
        print(f"max_stkptr={mx}  (steps={args.steps}, processor={args.processor})")

    if args.max_le is not None and mx > args.max_le:
        print(
            f"error: max_stkptr {mx} exceeds limit {args.max_le}",
            file=sys.stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

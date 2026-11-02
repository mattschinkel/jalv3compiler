#!/usr/bin/env python3
"""
Static contract check for simulator validation blocks.

Why this exists
---------------
A `@gpsim_validate` / `@sim_validate` block that pins only a *failure* flag
(`expect ... 0` on a variable that starts at 0) is satisfied by power-on RAM
and therefore also by a program that never ran.  That is not hypothetical:
`18f4620_float_str_battery.jal` passed `jal_gpsim_validate.sh -n 1` (one
instruction step) until 2026-11-01, and an earlier PIC32 test had the same
shape (`_fb_res` declared at the END, `expect 0`, fix_qemu_test_window_padding).
The runtime harness cannot see the difference; only the source can.

Rule (one per block)
--------------------
Every validation block must contain at least one `expect` line and satisfy at
least one of:

  * an expectation on the checkpoint counter (`..._check_step ...`), which only
    reaches its expected count if the program ran the checkpoints; or
  * at least one expectation whose value is NOT zero.

A block whose every expected value is 0 (and which has no checkpoint counter)
is VACUOUS and fails this check.

Scanned trees
-------------
  sims/gpsim/*.jal    -> @gpsim_validate blocks (208 files)
  sims/pic32/*.jal    -> @sim_validate   blocks
  sims/esp32/*.jal    -> @sim_validate   blocks

Usage:
  python3 unit-tests/check_validate_contract.py [--root DIR] [--quiet]

Exit: 0 = every block satisfies the rule; 1 = at least one vacuous block;
      2 = a file with no validation block at all in a scanned tree.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

TREES = (
    ("sims/gpsim", "@gpsim_validate"),
    ("sims/pic32", "@sim_validate"),
    ("sims/esp32", "@sim_validate"),
)

_BLOCK_RE = re.compile(r"@(?:gpsim|sim)_validate\s+begin(.*?)@(?:gpsim|sim)_validate\s+end",
                       re.S | re.I)
_EXPECT_RE = re.compile(r"^\s*expect\s+(\w+)\s+(\S+)(?:\s+(\S+))?", re.I)
# harness directives that merely start with "expect" (e.g.
# `expect_hw_stkptr_le_compiler_depth: 1`) are not value expectations
_DIRECTIVE_RE = re.compile(r"^\s*expect\w*\s*:", re.I)
_ZERO = {"0", "0x0", "0x00", "0b0", "0o0", "'0'", '"0"'}


def _parse_block(body: str):
    """Return (expects, problems) for one block body.

    expects is a list of (name, value_text); a directive with a single field
    (no value) is recorded with value None and also as a problem.
    """
    expects = []
    problems = []
    for raw in body.splitlines():
        line = re.sub(r"^\s*--\s?", "", raw).strip()
        if not line:
            continue
        low = line.lower()
        if not low.startswith("expect"):
            continue
        if _DIRECTIVE_RE.match(line):
            continue
        m = _EXPECT_RE.match(line)
        if not m:
            problems.append(f"unparseable expect line: {line!r}")
            continue
        kind, name, value = m.group(1).lower(), m.group(2), m.group(3)
        if value is None:
            # `expect byte v_x` — some harnesses accept this as "hide the
            # symbol"?  Treat it as malformed here: it pins nothing.
            problems.append(f"expect without a value: {line!r}")
            continue
        expects.append((kind, name, value))
    return expects, problems


def check_file(path: Path):
    text = path.read_text(errors="replace")
    blocks = list(_BLOCK_RE.finditer(text))
    if not blocks:
        return ["no validation block found"]
    problems = []
    for idx, m in enumerate(blocks, 1):
        expects, per = _parse_block(m.group(1))
        problems.extend(f"block {idx}: {p}" for p in per)
        if not expects:
            problems.append(f"block {idx}: no expect lines")
            continue
        has_check_step = any("check_step" in name.lower() for _k, name, _v in expects)
        has_nonzero = any(value.strip().strip("'\"") not in _ZERO
                          for _k, _n, value in expects)
        if not has_check_step and not has_nonzero:
            problems.append(
                f"block {idx}: VACUOUS — every expected value is 0 and there is no "
                f"checkpoint-counter expectation (a stalled program passes): "
                + ", ".join(f"{n}={v}" for _k, n, v in expects)
            )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=".", help="repository root (default: .)")
    ap.add_argument("--quiet", action="store_true", help="only report failures")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    bad = 0
    checked = 0
    for rel, marker in TREES:
        tree = root / rel
        files = sorted(tree.glob("*.jal"))
        if not files:
            print(f"error: no .jal under {tree}", file=sys.stderr)
            return 2
        for path in files:
            checked += 1
            problems = check_file(path)
            if problems:
                bad += 1
                print(f"FAIL {path.relative_to(root)}", file=sys.stderr)
                for p in problems:
                    print(f"     {p}", file=sys.stderr)

    if bad:
        print(f"validate-contract: FAIL ({bad} of {checked} files)", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"validate-contract: PASS ({checked} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

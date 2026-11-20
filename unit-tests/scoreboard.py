#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
Codegen-parity scoreboard over unittests.py --compare outputs.

Reads unit-tests/Output_Compare/hex_size_comparison.txt and
data_size_comparison.txt (produced by `unittests.py --compare`) and renders a
per-sample parity table: hex-file bytes and data-area bytes, each as
current minus baseline (jalv25r9). A sample is "worse" when either dimension is
positive.

Modes:
  (none)                 render the scoreboard to stdout and write
                         unit-tests/Output_Compare/scoreboard.txt
  --gate                 exit 1 if any sample is worse than baseline ("0 worse")
  --record-golden FILE   write the current per-sample deltas to FILE
  --golden FILE          like --gate, but only a delta that exceeds its golden
                         value (or a worse sample missing from the golden) fails

The golden file is a CSV header plus one row per sample:
  name,hex_delta,data_delta
"""

import os
import sys

COMPARE_DIR = os.path.join("unit-tests", "Output_Compare")
HEX_REPORT = os.path.join(COMPARE_DIR, "hex_size_comparison.txt")
DATA_REPORT = os.path.join(COMPARE_DIR, "data_size_comparison.txt")
SCOREBOARD_OUT = os.path.join(COMPARE_DIR, "scoreboard.txt")


def _load_sizes(path):
    """Return {name: (baseline, current)} for rows with two numeric sizes."""
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            parts = [p.strip() for p in line.split(",")]
            if len(parts) != 4:
                continue
            name, b, c, status = parts
            if status != "different":
                continue
            if not b.isdigit() or not c.isdigit():
                continue
            out[name] = (int(b), int(c))
    return out


def _load_golden(path):
    """Return {name: (hex_delta, data_delta)} from a recorded golden file."""
    out = {}
    if not path or not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            parts = [p.strip() for p in line.split(",")]
            if len(parts) != 3 or parts[0] in ("name", ""):
                continue
            try:
                out[parts[0]] = (int(parts[1]), int(parts[2]))
            except ValueError:
                continue
    return out


def _deltas():
    hex_sizes = _load_sizes(HEX_REPORT)
    data_sizes = _load_sizes(DATA_REPORT)
    names = set(hex_sizes) | set(data_sizes)
    rows = {}
    for name in names:
        hb, hc = hex_sizes.get(name, (0, 0))
        db, dc = data_sizes.get(name, (0, 0))
        hex_delta = hc - hb if name in hex_sizes else 0
        data_delta = dc - db if name in data_sizes else 0
        rows[name] = (hex_delta, data_delta)
    return rows


def _render(rows):
    worse = {n: (h, d) for n, (h, d) in rows.items() if h > 0 or d > 0}
    better = {n: (h, d) for n, (h, d) in rows.items() if h < 0 or d < 0}
    equal = {n for n in rows if n not in worse and n not in better}
    lines = [
        "CODEGEN-PARITY SCOREBOARD",
        "------------------------",
        "deltas are current minus baseline (jalv25r9); positive = worse",
        "",
        "hex worse:  %d" % len([n for n, (h, d) in worse.items() if h > 0]),
        "data worse: %d" % len([n for n, (h, d) in worse.items() if d > 0]),
        "hex better: %d" % len([n for n, (h, d) in better.items() if h < 0]),
        "data better: %d" % len([n for n, (h, d) in better.items() if d < 0]),
        "equal (both dims): %d" % len(equal),
        "",
        "name,hex_delta,data_delta",
    ]
    for name in sorted(worse, key=lambda n: (worse[n][0] + worse[n][1]), reverse=True):
        h, d = worse[name]
        lines.append("%s,%d,%d" % (name, h, d))
    return lines, worse


def main(argv):
    rows = _deltas()
    if not rows:
        sys.stderr.write("scoreboard: no compare data; run unittests.py "
                         "--compare first\n")
        return 2

    golden = None
    record = None
    gate = False
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--gate":
            gate = True
        elif arg == "--golden":
            i += 1
            golden = argv[i] if i < len(argv) else None
        elif arg == "--record-golden":
            i += 1
            record = argv[i] if i < len(argv) else None
        else:
            sys.stderr.write("scoreboard: unknown argument: %s\n" % arg)
            return 2
        i += 1

    if record:
        with open(record, "w", encoding="utf-8") as fh:
            fh.write("name,hex_delta,data_delta\n")
            for name in sorted(rows):
                h, d = rows[name]
                fh.write("%s,%d,%d\n" % (name, h, d))
        sys.stdout.write("scoreboard: recorded %d rows to %s\n"
                         % (len(rows), record))
        return 0

    lines, worse = _render(rows)
    with open(SCOREBOARD_OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")

    if not gate and not golden:
        sys.stdout.write("\n".join(lines) + "\n")
        return 0

    violations = []
    if golden:
        gold = _load_golden(golden)
        for name, (h, d) in worse.items():
            if name not in gold:
                violations.append((name, h, d))
            else:
                gh, gd = gold[name]
                if h > gh or d > gd:
                    violations.append((name, h, d))
    else:
        violations = sorted(
            [(name, h, d) for name, (h, d) in worse.items()],
            key=lambda t: t[1] + t[2], reverse=True)

    if violations:
        sys.stderr.write("scoreboard gate FAILED: %d sample(s) worse\n"
                         % len(violations))
        for name, h, d in violations:
            sys.stderr.write("  %s hex=%+d data=%+d\n"
                             % (name, h, d))
        return 1
    sys.stdout.write("scoreboard gate PASSED: 0 worse\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

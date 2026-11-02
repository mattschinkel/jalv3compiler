#!/usr/bin/env python3
"""Fail when a simulator test adds a NEW validate-block exemption.

The harness accepts three kinds of "this test does not do the usual thing"
declarations in its `@gpsim_validate` / `@sim_validate` block:

    skip_gpsim: 1                  compile/config only, no runtime run
    checkpoint_verify: off         do not pair the checkpoint count statically
    allow_compiler_warnings: 1     compile WITH warnings

Each one removes a signal, and each one was added for a real reason that is
usually documented in the file.  Nothing stopped the list from growing,
though: a new exemption is invisible in a diff that the gate does not read.
This check pins the CURRENT set in `unit-tests/validate_exemptions_baseline.txt`
and fails on any addition, the same way the analyzer gate pins its findings.

    python3 unit-tests/check_validate_exemptions.py           # check
    python3 unit-tests/check_validate_exemptions.py --update  # re-pin
    python3 unit-tests/check_validate_exemptions.py --quiet

Removing an exemption is always fine; it is reported as stale so the baseline
can be re-pinned in the same change.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "unit-tests" / "validate_exemptions_baseline.txt"
TREES = ("sims/gpsim", "sims/pic32", "sims/esp32")

KINDS = {
    "skip_gpsim": ("skip_gpsim", True),
    "checkpoint_verify_off": ("checkpoint_verify", False),
    "allow_compiler_warnings": ("allow_compiler_warnings", True),
}
TRUTHY = {"1", "true", "yes", "on"}
FALSY = {"0", "false", "no", "off"}

BLOCK_RE = re.compile(
    r"@(?:gpsim|sim)_validate\s+begin(.*?)@(?:gpsim|sim)_validate\s+end",
    re.I | re.S,
)


def block_body(text: str) -> str:
    m = BLOCK_RE.search(text)
    return m.group(1) if m else ""


def scan() -> set[tuple[str, str]]:
    found: set[tuple[str, str]] = set()
    for tree in TREES:
        for path in sorted((ROOT / tree).glob("*.jal")):
            body = block_body(path.read_text(encoding="utf-8", errors="replace"))
            if not body:
                continue
            for raw in body.splitlines():
                line = raw.strip()
                if line.startswith("--"):
                    line = line[2:].strip()
                if ":" not in line:
                    continue
                key, _, value = line.partition(":")
                key = key.strip().lower()
                value = value.strip().split()[0].lower() if value.strip() else ""
                for kind, (field, truthy) in KINDS.items():
                    if key != field:
                        continue
                    hit = value in TRUTHY if truthy else value in FALSY
                    if hit:
                        found.add((kind, str(path.relative_to(ROOT))))
    return found


def load_baseline() -> set[tuple[str, str]]:
    if not BASELINE.is_file():
        return set()
    out: set[tuple[str, str]] = set()
    for raw in BASELINE.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2:
            out.add((parts[0], parts[1]))
    return out


def write_baseline(entries: set[tuple[str, str]]) -> None:
    lines = [
        "# Pinned validate-block exemptions (see check_validate_exemptions.py).",
        "# One entry per line: <kind> <path relative to the repo root>.",
        "# Adding a line is a deliberate decision: the test loses a signal.",
        "# Remove a line when the exemption is no longer needed.",
    ]
    for kind, path in sorted(entries):
        lines.append(f"{kind:<26} {path}")
    BASELINE.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--update", action="store_true", help="re-pin the baseline")
    ap.add_argument("--quiet", action="store_true", help="only report problems")
    args = ap.parse_args()

    found = scan()
    if args.update:
        write_baseline(found)
        print(f"validate-exemptions: baseline updated ({len(found)} entries)")
        return 0

    pinned = load_baseline()
    new = found - pinned
    stale = pinned - found

    if not args.quiet:
        print(f"validate-exemptions: {len(found)} exemptions, {len(pinned)} pinned")

    if stale:
        for kind, path in sorted(stale):
            print(f"note: stale exemption (no longer present): {kind} {path}", file=sys.stderr)
        print(
            "note: re-pin with: python3 unit-tests/check_validate_exemptions.py --update",
            file=sys.stderr,
        )

    if new:
        print(
            f"validate-exemptions: FAIL ({len(new)} NEW exemption(s))",
            file=sys.stderr,
        )
        for kind, path in sorted(new):
            print(f"  {kind}  {path}", file=sys.stderr)
        print(
            "A new exemption removes a signal from a runtime test. If it is\n"
            "deliberate, say why in the file and re-pin with:\n"
            "  python3 unit-tests/check_validate_exemptions.py --update",
            file=sys.stderr,
        )
        return 1

    print("validate-exemptions: PASS (no new exemptions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

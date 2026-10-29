#!/usr/bin/env python3
"""
PIC32 QEMU smoke / regression driver (Phase 4).

Runs sims/jal_sim_validate.sh on each .jal under sims/pic32/
matched by the selected suite glob. Does NOT invoke gpsim — keep the
gpsim-first gate in run_regression_suite.py unchanged.

Usage (from repo root):
  python3 sims/run_pic32_regression.py --run
  python3 sims/run_pic32_regression.py --list-suites
  python3 sims/run_pic32_regression.py --suite 32mz1024efh064 --run
  python3 sims/run_pic32_regression.py --jobs 4 --quiet --run

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

import _lowprio
import _runlock
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VALIDATE = ROOT / "sims" / "jal_sim_validate.sh"
DEFAULT_COMPILER = ROOT / "bin" / "jalv3-x86-64"


@dataclass(frozen=True)
class Suite:
    id: str
    glob: str
    description: str


SUITES: list[Suite] = [
    Suite(
        "32mz1024efh064",
        "sims/pic32/32mz1024efh064_*.jal",
        "PIC32MZ1024EFH @sim_validate suite (smoke + type + body ports)",
    ),
    Suite(
        "32mx270f256b",
        "sims/pic32/32mx270f256b_*.jal",
        "PIC32MX270F256B (MX1/2, M4K) @sim_validate suite",
    ),
]


def list_suite_files(suite: Suite) -> list[Path]:
    return sorted(ROOT.glob(suite.glob))


def run_one(jal: Path, compiler: Path, quiet: bool) -> tuple[Path, int, str]:
    cmd = [
        str(VALIDATE),
        "--compiler",
        str(compiler),
        str(jal),
    ]
    if quiet:
        cmd.insert(1, "--quiet")
    env = os.environ.copy()
    env.setdefault("JALV2_NO_REPORT_ARCHIVE", "1")
    env.setdefault("JALV2_NO_CHANGELOG_ENTRY", "1")
    p = subprocess.run(
        cmd,
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True,
    )
    out = (p.stdout or "") + (p.stderr or "")
    return jal, p.returncode, out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list-suites", action="store_true")
    ap.add_argument("--suite", default="all",
                    help="suite id, or 'all' (default) for every suite")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JALV2_TEST_JOBS", "16")))
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    ap.add_argument("--fail-fast", action="store_true")
    args = ap.parse_args()

    if args.list_suites:
        for s in SUITES:
            n = len(list_suite_files(s))
            print(f"{s.id:28s}  {n:3d} files  {s.description}")
        return 0

    if not args.run:
        ap.print_help()
        return 0

    if args.suite == "all":
        suites = SUITES
    else:
        suite = next((s for s in SUITES if s.id == args.suite), None)
        if suite is None:
            print(f"error: unknown suite {args.suite!r}", file=sys.stderr)
            return 2
        suites = [suite]
    for suite in suites:
        if not list_suite_files(suite):
            print(f"error: no files for suite {suite.id} ({suite.glob})",
                  file=sys.stderr)
            return 2
    if not args.compiler.is_file():
        print(f"error: compiler not found: {args.compiler}", file=sys.stderr)
        return 2
    if not VALIDATE.is_file():
        print(f"error: missing {VALIDATE}", file=sys.stderr)
        return 2

    _lowprio.lower_own_priority()
    _runlock.acquire("pic32_suite")
    total_ok = 0
    total = 0
    any_failed = False
    for suite in suites:
        files = list_suite_files(suite)
        print(f"PIC32 suite {suite.id}: {len(files)} file(s), jobs={args.jobs}")
        failed: list[Path] = []
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
            futs = {
                ex.submit(run_one, jal, args.compiler, args.quiet): jal
                for jal in files
            }
            for fut in as_completed(futs):
                jal, rc, out = fut.result()
                status = "PASS" if rc == 0 else "FAIL"
                print(f"  [{status}] {jal.relative_to(ROOT)}")
                if rc != 0 or not args.quiet:
                    if out.strip():
                        for line in out.strip().splitlines()[-40:]:
                            print(f"    {line}")
                if rc != 0:
                    failed.append(jal)
                    if args.fail_fast:
                        break

        n_ok = len(files) - len(failed)
        total_ok += n_ok
        total += len(files)
        any_failed = any_failed or bool(failed)
        print(f"=== {suite.id}: RESULT: {n_ok}/{len(files)} PASS ===")
        if failed and args.fail_fast:
            break
    print(f"=== RESULT: {total_ok}/{total} PASS ===")
    return 0 if not any_failed else 1


if __name__ == "__main__":
    sys.exit(main())

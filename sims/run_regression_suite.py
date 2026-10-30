#!/usr/bin/env python3
"""
Multi-device gpsim compiler regression: checkpoint verification + jal_gpsim_validate.

1) Ensures regression_checkpoint() count matches expect byte v__check_step N in @gpsim_validate
   (unless the block contains ``checkpoint_verify: off``, e.g. when ``_check_step`` storage aliases
   large dynamic-string / compaction paths on PIC18).
2) Runs sims/jal_gpsim_validate.sh on each .jal matched by the selected suite glob.

Usage (from repo root):
  ./sims/run_regression_suite.py              # show help only
  ./sims/run_regression_suite.py --list-suites
  ./sims/run_regression_suite.py --run        # default: all suites (146 .jal: 30+23+30+25 compiler regression + 16f877×27 + 18f4620_gpsim×11)
  ./sims/run_regression_suite.py --suite 18f4620 --run
  ./sims/run_regression_suite.py --suite-all --quiet
  ./sims/run_regression_suite.py --baseline --run
  ./sims/run_regression_suite.py --compiler ./bin/jalv3-x86-64 --run
  ./sims/run_regression_suite.py --jobs 4 --run
  ./sims/run_regression_suite.py --quiet
  ./sims/run_regression_suite.py --fail-fast
  ./sims/run_regression_suite.py --keep-logs /tmp/jal_gpsim_logs --run

Environment:
  JALV2_TEST_JOBS        Default parallel job count (overridden by --jobs). Default: 16.
  REGRESSION_SUITE_ID     Default suite id (overridden by --suite). Default: all (every registered suite).
                          Set to e.g. 18f4620 to run a single suite.
  REGRESSION_SUITE_COMPILER
                          Current compiler path used unless --compiler or --baseline is supplied.
                          Default: bin/jalv3-x86-64 (built from src/).
  JALV2_NO_REPORT_ARCHIVE When set to 1/true/yes, skip post-run snapshots under report_archive/ (suite still
                          clears gpsim_regression_summary.txt before running). JALV2_GPSIM_SUBPROCESS=1 when
                          invoked from unittests.py (skips duplicate snapshot and web copy here; parent copies
                          the summary to WEB_ROOT after writing the combined log).
  JALV2_UNIT_TEST_WEB_ROOT  Destination directory (default /var/www/jalv2_unit_test). After each standalone
                          run this script copies only sims/reports/gpsim_regression_summary.txt
                          into WEB_ROOT/data/ (publish_gpsim_regression_summary_to_web_root); skipped when
                          JALV2_GPSIM_SUBPROCESS=1 (unittests.py copies after writing the combined log). Full
                          dashboard publish remains python3 publish_unit_test_web.py / unittests baseline|current|compare.
  JALV2_NO_CHANGELOG_ENTRY When set to 1/true/yes, skip appending the one-line run summary to CHANGELOG.txt
  (unittests.py and this script call append_changelog_test_automation after snapshots).
  JALV2_GPSIM_CHANGELOG_PARTIAL When set to 1/true/yes, also append CHANGELOG for a partial suite run (default:
  only the full ordered suite list — same as --suite all / --suite-all — gets a changelog line).

See instructions_gpsim_test.txt (repo root) for the full gpsim / @gpsim_validate guide.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

import _lowprio
import _runlock
import tempfile
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

GPSIM_RUN_STATS_JSON = "gpsim_regression_run.json"
DEFAULT_CURRENT_COMPILER = "bin/jalv3-x86-64"

_gather_jal_merged: Callable[[str], str] | None = None


def _merged_jal_for_checkpoints(jal_path: Path) -> str:
    """Main .jal plus same-dir include chain (matches gpsim_validate_engine.gather_jal_with_local_includes)."""
    global _gather_jal_merged
    if _gather_jal_merged is None:
        engine = Path(__file__).resolve().parent / "gpsim_validate_engine.py"
        spec = importlib.util.spec_from_file_location("_jal_gpsim_validate_engine", engine)
        mod = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(mod)
        _gather_jal_merged = mod.gather_jal_with_local_includes
    return _gather_jal_merged(str(jal_path))


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class RegressionSuite:
    """One regression family; each glob is relative to repository root.

    A suite may carry several globs (e.g. the compiler_regression body
    family plus the gpsim witness family) — files are the sorted union.

    libdir overrides the library search order passed to
    jal_gpsim_validate.sh (-s). The default keeps PRISTINE jallib first
    (third_party/jallib/lib;demo_lib) so the classic suites track the
    upstream libraries; a suite whose witnesses pin the FORK-overlay
    resolution (the demo_lib-first order of user programs, samples and
    the unittests corpus) sets libdir="demo_lib;third_party/jallib/lib"."""

    id: str
    description: str
    globs: tuple[str, ...]
    libdir: str | None = None


# Register new architectures here: each suite needs its own regression_<device>_device.jal
# (never include regression_18f4620_device on a non–18F4620 part) plus a matching *.jal glob.
# Device bundles: regression_18f4620_device, regression_18f26k22_device,
# regression_16f1825_device, regression_12f1840_device (see instructions_gpsim_test.txt).
SUITES: dict[str, RegressionSuite] = {
    "12f1840": RegressionSuite(
        id="12f1840",
        description="PIC12F1840 compiler regression (sims/gpsim/12f1840_compiler_regression_*.jal)",
        globs=("sims/gpsim/12f1840_compiler_regression_*.jal",),
    ),
    "16f1825": RegressionSuite(
        id="16f1825",
        description="PIC16F1825 compiler regression (sims/gpsim/16f1825_compiler_regression_*.jal)",
        globs=("sims/gpsim/16f1825_compiler_regression_*.jal",
            "sims/gpsim/16f1825_gpsim_*.jal"),
    ),
    "18f4620": RegressionSuite(
        id="18f4620",
        description="PIC18F4620 compiler regression (sims/gpsim/18f4620_compiler_regression_*.jal)",
        globs=("sims/gpsim/18f4620_compiler_regression_*.jal",),
    ),
    "18f4620_gpsim": RegressionSuite(
        id="18f4620_gpsim",
        description="PIC18F4620 focused gpsim smoke (sims/gpsim/18f4620_gpsim_*.jal"
                    " + the float-renderer battery, whose historical name"
                    " predates the suite glob)",
        globs=("sims/gpsim/18f4620_gpsim_*.jal",
            "sims/gpsim/18f4620_float_str_battery.jal"),
    ),
    "18f26k22": RegressionSuite(
        id="18f26k22",
        description="PIC18F26K22 compiler regression (sims/gpsim/18f26k22_compiler_regression_*.jal)",
        globs=("sims/gpsim/18f26k22_compiler_regression_*.jal",
            "sims/gpsim/18f26k22_gpsim_*.jal"),
    ),
    "16f877": RegressionSuite(
        id="16f877",
        description="PIC16F877 data-pointer gpsim smoke (sims/gpsim/16f877_gpsim_*.jal; split/extended from 16f877_pointer_basic_test.jal)",
        globs=("sims/gpsim/16f877_gpsim_*.jal",),
    ),
    "print_shadow": RegressionSuite(
        id="print_shadow",
        description="print.jal fork shadow under the demo_lib-first include order "
                    "(sims/gpsim/18f4620_print_shadow*.jal; user/corpus resolution, not the "
                    "pristine-first default of the classic suites)",
        globs=("sims/gpsim/18f4620_print_shadow*.jal",),
        libdir="demo_lib;third_party/jallib/lib",
    ),
    "string_cmp_bank_merge": RegressionSuite(
        id="string_cmp_bank_merge",
        description="BSR-worklist round-truncation witness under the demo_lib-first "
                    "include order (sims/gpsim/16f1825_string_cmp_bank_merge.jal -- the "
                    "layout that puts the string-compare result bit in a non-zero bank "
                    "only resolves demo_lib-first; pin: pre-fix builds read the compare "
                    "as false with byte-exact content)",
        globs=("sims/gpsim/16f1825_gpsim_string_cmp_bank_merge.jal",),
        libdir="demo_lib;third_party/jallib/lib",
    ),
}

# Order for --suite all / default (not alphabetical: 4620 first, then other PIC18, PIC14 enhanced, classic 877).
SUITE_RUN_ORDER: tuple[str, ...] = ("18f4620", "18f4620_gpsim", "18f26k22", "16f1825", "12f1840", "16f877", "print_shadow", "string_cmp_bank_merge")


def registered_suite_ids_ordered() -> list[str]:
    """Suites run for --suite all / --suite-all (order matches SUITE_RUN_ORDER ∩ SUITES)."""
    return [s for s in SUITE_RUN_ORDER if s in SUITES]


def _env_forces_gpsim_changelog_on_partial() -> bool:
    """True when JALV2_GPSIM_CHANGELOG_PARTIAL is set — append CHANGELOG even for a single-suite (etc.) run."""
    v = os.environ.get("JALV2_GPSIM_CHANGELOG_PARTIAL", "").strip().lower()
    return v in ("1", "true", "yes", "on")


# --- checkpoint verification (was verify_regression_checkpoint_counts.py) ---


def _strip_jal_comment(line: str) -> str:
    if "--" in line:
        idx = line.find("--")
        if idx >= 0:
            before = line[:idx]
            if not before.strip():
                return ""
            return before.rstrip()
    return line


def _is_only_comment_line(line: str) -> bool:
    return line.lstrip().startswith("--")


def count_checkpoints(text: str) -> int:
    n = 0
    for raw in text.splitlines():
        if _is_only_comment_line(raw):
            continue
        line = _strip_jal_comment(raw)
        if re.match(r"^\s*procedure\s+regression_checkpoint\s*\(", line, re.I):
            continue
        n += len(re.findall(r"\bregression_checkpoint\s*\(\s*\)", line))
    return n


def parse_checkpoint_verify_disabled(text: str) -> bool:
    m = re.search(
        r"@gpsim_validate\s+begin(.*?)@gpsim_validate\s+end",
        text,
        re.I | re.S,
    )
    if not m:
        return False
    block = m.group(1)
    for raw in block.splitlines():
        if not raw.strip().startswith("--"):
            continue
        rest = raw.strip()[2:].strip().lower()
        if rest.startswith("checkpoint_verify:"):
            tail = rest.split(":", 1)[1].strip()
            if tail in ("0", "off", "false", "no"):
                return True
    return False


def parse_expected_check_step(text: str) -> int | None:
    m = re.search(
        r"@gpsim_validate\s+begin(.*?)@gpsim_validate\s+end",
        text,
        re.I | re.S,
    )
    if not m:
        return None
    block = m.group(1)
    for raw in block.splitlines():
        if not raw.strip().startswith("--"):
            continue
        rest = raw.strip()[2:].strip()
        if re.match(r"expect\s+byte\s+v__check_step\s+", rest, re.I):
            parts = rest.split()
            try:
                return int(parts[3], 0)
            except (IndexError, ValueError):
                pass
    return None


def list_suite_jals(root: Path, suite_id: str) -> list[Path]:
    spec = SUITES[suite_id]
    seen: set[Path] = set()
    for pattern in spec.globs:
        seen.update(root.glob(pattern))
    return sorted(seen)


def verify_checkpoint_counts(root: Path, suite_id: str) -> int:
    """Print per-file OK/FAIL for one suite. Return 0 if all OK, else 1; 2 if no files."""
    files = list_suite_jals(root, suite_id)
    if not files:
        print(
            f"error: suite {suite_id!r}: no files matching {SUITES[suite_id].globs!r}",
            file=sys.stderr,
        )
        return 2

    err = 0
    label = f"[{suite_id}] "
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        merged = _merged_jal_for_checkpoints(path)
        got = count_checkpoints(merged)
        exp = parse_expected_check_step(text)
        name = path.name
        if parse_checkpoint_verify_disabled(text):
            print(
                f"OK   {label}{name}: checkpoints=skip (checkpoint_verify: off in @gpsim_validate)"
            )
            continue
        if exp is None:
            print(
                f"FAIL {label}{name}: no expect byte v__check_step in @gpsim_validate",
                file=sys.stderr,
            )
            err = 1
            continue
        if got != exp:
            print(
                f"FAIL {label}{name}: regression_checkpoint() count {got} != expect {exp}",
                file=sys.stderr,
            )
            err = 1
        else:
            print(f"OK   {label}{name}: checkpoints={got} expect={exp}")
    return err


# --- jal_gpsim_validate runner ---


def _run_validate(
    root: Path,
    jal: Path,
    validate_args: list[str],
    quiet: bool,
) -> tuple[int, str]:
    script = root / "sims" / "jal_gpsim_validate.sh"
    cmd = [str(script)] + validate_args + (["--quiet"] if quiet else []) + [str(jal)]
    p = subprocess.run(
        cmd,
        cwd=root,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    log = (p.stdout or "") + (p.stderr or "")
    hdr = f"\n========================================\n{jal}\n========================================\n"
    return p.returncode, hdr + log


def _copy_keep_logs(
    keep_dir: Path,
    workdir: Path,
    jals: list[Path],
    *,
    subdir: str | None = None,
) -> None:
    dest = (keep_dir / subdir) if subdir else keep_dir
    dest.mkdir(parents=True, exist_ok=True)
    for k, jal in enumerate(jals, start=1):
        logf = workdir / f"job-{k}.log"
        if logf.is_file():
            base = jal.stem
            shutil.copy2(logf, dest / f"{k:02d}_{base}.log")
    print(f"run_regression_suite: per-job logs copied to {dest}")


def _run_validate_phase(
    root: Path,
    jals: list[Path],
    suite_id: str,
    args: argparse.Namespace,
    validate_args: list[str],
    workdir: Path | None,
    *,
    keep_log_subdir: str | None = None,
) -> tuple[int, int, int, int]:
    """Run jal_gpsim_validate on jals.

    Returns (exit_code, passed, failed, tested) where passed+failed==tested for this suite.
    """
    suite = SUITES.get(suite_id)
    if suite is not None and suite.libdir:
        # fork-overlay resolution order (see RegressionSuite.libdir)
        validate_args = ["-s", suite.libdir] + list(validate_args)
    extra_desc = " ".join(validate_args) if validate_args else "(none)"
    prefix = f"[{suite_id}] "

    if args.fail_fast:
        print(
            f"run_regression_suite: suite {suite_id!r}, {len(jals)} file(s), "
            f"fail-fast (sequential), compiler extra: {extra_desc}"
        )
        print()
        passed = 0
        for jal in jals:
            rc, log = _run_validate(root, jal, validate_args, args.quiet)
            if args.quiet:
                if rc == 0:
                    print(f"OK   {prefix}{jal.name}")
                else:
                    print(f"FAIL {prefix}{jal.name}", file=sys.stderr)
            else:
                sys.stdout.write(log)
            if rc == 0:
                passed += 1
            else:
                failed = 1
                tested = passed + failed
                print(
                    f"run_regression_suite: fail-fast stopping after {prefix}{jal.name}",
                    file=sys.stderr,
                )
                return 1, passed, failed, tested
        print()
        print(f"run_regression_suite: suite {suite_id!r} all OK ({len(jals)} files)")
        return 0, len(jals), 0, len(jals)

    max_jobs = args.jobs
    print(
        f"run_regression_suite: suite {suite_id!r}, {len(jals)} file(s), "
        f"up to {max_jobs} parallel job(s), compiler extra: {extra_desc}"
    )
    if args.quiet:
        print("run_regression_suite: --quiet (summary at end; tail of failed jobs on error)")
    print()

    assert workdir is not None
    results: list[tuple[int, str]] = [(-1, "")] * len(jals)

    def _work(idx: int, jal: Path) -> tuple[int, int, str]:
        rc, log = _run_validate(root, jal, validate_args, args.quiet)
        return idx, rc, log

    with ThreadPoolExecutor(max_workers=max_jobs) as ex:
        futs = [ex.submit(_work, i, j) for i, j in enumerate(jals)]
        for fut in as_completed(futs):
            idx, rc, log = fut.result()
            results[idx] = (rc, log)

    for k, ((rc, log), jal) in enumerate(zip(results, jals), start=1):
        (workdir / f"job-{k}.log").write_text(log, encoding="utf-8")
        (workdir / f"job-{k}.rc").write_text("0" if rc == 0 else "1", encoding="utf-8")

    failed = any(rc != 0 for rc, _ in results)
    npass = sum(1 for rc, _ in results if rc == 0)
    nfail = sum(1 for rc, _ in results if rc != 0)
    keep = Path(args.keep_logs).resolve() if args.keep_logs else None

    if args.quiet:
        print()
        print(f"run_regression_suite (quiet summary) [{suite_id}]:")
        failed_names: list[str] = []
        for jal, (rc, _) in zip(jals, results):
            if rc == 0:
                print(f"OK   {prefix}{jal.name}")
            else:
                print(f"FAIL {prefix}{jal.name}", file=sys.stderr)
                failed_names.append(jal.name)
        if keep:
            _copy_keep_logs(keep, workdir, jals, subdir=keep_log_subdir)
        if failed:
            print(
                f"run_regression_suite: suite {suite_id!r} failed ({len(failed_names)}): "
                f"{' '.join(failed_names)}",
                file=sys.stderr,
            )
            print("Hint: re-run without --quiet for full per-test logs.", file=sys.stderr)
            for k, (jal, (rc, _)) in enumerate(zip(jals, results), start=1):
                if rc != 0:
                    logf = workdir / f"job-{k}.log"
                    print(f"--- tail {prefix}{jal.name} ---", file=sys.stderr)
                    if logf.is_file():
                        lines = logf.read_text(encoding="utf-8", errors="replace").splitlines()
                        print("\n".join(lines[-60:]), file=sys.stderr)
            print("run_regression_suite: one or more tests failed", file=sys.stderr)
            return 1, npass, nfail, len(jals)
        print()
        print(f"run_regression_suite: suite {suite_id!r} all OK ({len(jals)} files)")
        return 0, npass, nfail, len(jals)

    for _, log in results:
        sys.stdout.write(log)
    if keep:
        _copy_keep_logs(keep, workdir, jals, subdir=keep_log_subdir)
    print()
    if failed:
        print(f"run_regression_suite: suite {suite_id!r} failed files:", file=sys.stderr)
        for jal, (rc, _) in zip(jals, results):
            if rc != 0:
                print(f"  {jal}", file=sys.stderr)
        print("run_regression_suite: one or more tests failed", file=sys.stderr)
        return 1, npass, nfail, len(jals)
    print(f"run_regression_suite: suite {suite_id!r} all OK ({len(jals)} files)")
    return 0, npass, nfail, len(jals)


def _write_gpsim_regression_summary_file(
    root: Path,
    args: argparse.Namespace,
    suite_ids: list[str],
    exit_code: int,
    *,
    passed: int,
    failed: int,
    tested: int,
    checkpoint_failed_suite: str | None,
) -> None:
    """Write a short summary under Comparison/reports/ (full console log via unittests --gpsim-regression)."""
    path = root / "sims" / "reports" / "gpsim_regression_summary.txt"
    stats_path = root / "sims" / "reports" / GPSIM_RUN_STATS_JSON
    path.parent.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    extra = []
    if args.baseline:
        extra.append("--baseline")
    elif args.compiler:
        extra.extend(["--compiler", args.compiler])
    if args.quiet:
        extra.append("--quiet")
    if args.fail_fast:
        extra.append("--fail-fast")
    if args.suite_all:
        extra.append("--suite-all")
    else:
        extra.extend(["--suite", args.suite])
    if checkpoint_failed_suite is not None:
        results_line = (
            f"Results: checkpoint verification failed for suite {checkpoint_failed_suite!r} "
            f"before any .jal simulation (0 files tested).\n\n"
        )
    else:
        results_line = f"Results: {passed} passed, {failed} failed ({tested} .jal files tested).\n\n"
    lines = [
        f"{ts}\n\n",
        "GPSIM REGRESSION SUITE (run_regression_suite.py)\n",
        "=" * 50 + "\n",
        f"Suites: {', '.join(suite_ids)}\n",
        f"Compiler mode: {'baseline' if args.baseline else 'current'}\n",
        f"Compiler: {'baseline' if args.baseline else args.compiler}\n",
        f"Jobs: {args.jobs}\n",
        f"Args: {' '.join(extra)}\n",
        f"Exit code: {exit_code}\n",
        results_line,
        "For a captured stdout/stderr log, run: python3 unittests.py --gpsim-regression …\n",
    ]
    path.write_text("".join(lines), encoding="utf-8")
    meta = {
        "passed": passed,
        "failed": failed,
        "tested": tested,
        "checkpoint_failed_suite": checkpoint_failed_suite,
        "suite_ids": list(suite_ids),
        "exit_code": exit_code,
        "changelog_eligible": (
            list(suite_ids) == registered_suite_ids_ordered()
            and checkpoint_failed_suite is None
        ),
    }
    stats_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    if not args.quiet:
        print(f"run_regression_suite: wrote {path.relative_to(root)}")


def _build_argparser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Verify regression checkpoints then run jal_gpsim_validate per suite (see SUITES in script).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "With no command-line arguments, only this help is shown (exit 0).\n"
            "To run the suite: pass --run, or pass any other option (e.g. --quiet, --baseline).\n"
            "\n"
            "Suites are registered in run_regression_suite.py (SUITES). Use --list-suites to show ids.\n"
            "\n"
            "Examples:\n"
            "  ./sims/run_regression_suite.py --list-suites\n"
            "  ./sims/run_regression_suite.py --run\n"
            "  ./sims/run_regression_suite.py --suite 18f4620 --quiet\n"
            "  ./sims/run_regression_suite.py --suite all --quiet\n"
            "  ./sims/run_regression_suite.py --suite-all --quiet\n"
            "  ./sims/run_regression_suite.py --compiler ./bin/jalv3-x86-64 --run\n"
            "  ./sims/run_regression_suite.py --baseline --jobs 4 --run"
        ),
    )
    ap.add_argument(
        "--run",
        action="store_true",
        help="run the suite (optional if you pass another flag such as --quiet or --baseline)",
    )
    ap.add_argument(
        "--suite",
        default=os.environ.get("REGRESSION_SUITE_ID", "all"),
        metavar="ID",
        help=(
            "regression suite id, or 'all' for every suite (default: REGRESSION_SUITE_ID or all). "
            f"Registered: {', '.join(sorted(SUITES))}"
        ),
    )
    ap.add_argument(
        "--suite-all",
        action="store_true",
        help="run every registered suite in SUITE_RUN_ORDER (--suite ignored)",
    )
    ap.add_argument(
        "--list-suites",
        action="store_true",
        help="print registered suite ids and descriptions, then exit 0",
    )
    ap.add_argument("--baseline", action="store_true", help="pass --baseline to jal_gpsim_validate")
    ap.add_argument(
        "--compiler",
        default=os.environ.get("REGRESSION_SUITE_COMPILER", DEFAULT_CURRENT_COMPILER),
        metavar="PATH",
        help=(
            "current compiler path passed to jal_gpsim_validate "
            f"(default: REGRESSION_SUITE_COMPILER or {DEFAULT_CURRENT_COMPILER}; ignored with --baseline)"
        ),
    )
    ap.add_argument(
        "--jobs",
        "-j",
        type=int,
        metavar="N",
        default=int(os.environ.get("JALV2_TEST_JOBS", "16")),
        help="parallel jobs (default: JALV2_TEST_JOBS or 16)",
    )
    ap.add_argument("--quiet", "-q", action="store_true", help="one-line OK/FAIL per test; tails on failure")
    ap.add_argument("--no-uart", action="store_true",
                    help="skip the cross-architecture UART TXREG check appended after the suites")
    ap.add_argument("--fail-fast", action="store_true", help="sequential; stop on first failure")
    ap.add_argument(
        "--keep-logs",
        metavar="DIR",
        help="copy each job log to DIR (01_name.jal.log, …); with --suite-all, uses DIR/<suite_id>/ per suite",
    )
    return ap


def main() -> int:
    ap = _build_argparser()
    if len(sys.argv) <= 1:
        ap.print_help()
        return 0
    args = ap.parse_args()

    if args.list_suites:
        for sid in sorted(SUITES):
            print(f"{sid}\t{SUITES[sid].description}")
        return 0

    _lowprio.lower_own_priority()
    _runlock.acquire("gpsim_suite")

    if args.jobs < 1:
        print("error: --jobs requires a positive integer", file=sys.stderr)
        return 2

    if not args.suite_all and args.suite not in SUITES and args.suite != "all":
        print(
            f"error: unknown suite {args.suite!r} — registered: {', '.join(sorted(SUITES))} (or 'all')",
            file=sys.stderr,
        )
        print("hint: ./sims/run_regression_suite.py --list-suites", file=sys.stderr)
        return 2

    if args.suite_all or args.suite == "all":
        suite_ids = registered_suite_ids_ordered()
    else:
        suite_ids = [args.suite]

    root = _repo_root()
    os.chdir(root)
    if not args.baseline:
        compiler_path = Path(args.compiler)
        compiler_file = compiler_path if compiler_path.is_absolute() else root / compiler_path
        if not compiler_file.is_file():
            print(f"error: compiler not found: {compiler_file}", file=sys.stderr)
            return 2

    # archive_test_reports lives beside this runner in sims/.
    _rr = str(root / "sims")
    if _rr not in sys.path:
        sys.path.insert(0, _rr)

    from archive_test_reports import (
        append_changelog_test_automation,
        clear_gpsim_regression_summary,
        snapshot_test_report_trees,
    )

    clear_gpsim_regression_summary(root)

    validate_args = ["--baseline"] if args.baseline else ["--compiler", args.compiler]

    exit_code = 0
    suites_started = False
    gpsim_passed = 0
    gpsim_failed = 0
    gpsim_tested = 0
    checkpoint_failed_suite: str | None = None
    try:
        for suite_id in suite_ids:
            suites_started = True
            keep_sub = suite_id if (args.suite_all and args.keep_logs) else None
            vrc = verify_checkpoint_counts(root, suite_id)
            if vrc != 0:
                print(
                    "error: regression_checkpoint() count vs expect byte v__check_step mismatch "
                    f"(suite {suite_id!r}; see above)",
                    file=sys.stderr,
                )
                checkpoint_failed_suite = suite_id
                exit_code = 1 if vrc == 1 else vrc
                return exit_code
            print()

            jals = list_suite_jals(root, suite_id)
            if args.fail_fast:
                rc, p, f, t = _run_validate_phase(
                    root, jals, suite_id, args, validate_args, None, keep_log_subdir=keep_sub
                )
            else:
                workdir = Path(
                    tempfile.mkdtemp(prefix=f"jal_regression_suite.{suite_id}.", dir=os.environ.get("TMPDIR") or None)
                )
                try:
                    rc, p, f, t = _run_validate_phase(
                        root, jals, suite_id, args, validate_args, workdir, keep_log_subdir=keep_sub
                    )
                finally:
                    shutil.rmtree(workdir, ignore_errors=True)
            gpsim_passed += p
            gpsim_failed += f
            gpsim_tested += t
            if rc != 0:
                exit_code = rc
                return exit_code
            if len(suite_ids) > 1 and suite_id != suite_ids[-1]:
                print()

        if len(suite_ids) > 1:
            print()
            print(f"run_regression_suite: all suites OK ({', '.join(suite_ids)})")
        # Cross-architecture UART TXREG runtime check (serial output byte-exact
        # on 16f1823 / 18f4620 / 16f877); skip via --no-uart or JALV2_GPSIM_NO_UART=1
        if (not getattr(args, "no_uart", False)
              and os.environ.get("JALV2_GPSIM_NO_UART", "").strip().lower()
                not in ("1", "true", "yes")
              and shutil.which("gpsim")):
            uart_script = os.path.join(
                os.path.dirname(os.path.abspath(__file__)),
                "gpsim_uart_validate_issue36.py")
            uart_script = os.path.abspath(uart_script)
            if os.path.isfile(uart_script):
                if not args.quiet:
                    print("run_regression_suite: UART TXREG cross-arch check ...")
                r = subprocess.run(
                    [sys.executable, uart_script,
                     "--compiler", str(args.compiler), "--all"],
                    capture_output=True, text=True)
                tail = (r.stdout or "").strip().splitlines()
                for line in tail[-6:]:
                    if line.strip():
                        print(f"  {line}")
                if r.returncode != 0:
                    print("run_regression_suite: UART TXREG check FAILED",
                          file=sys.stderr)
                    if r.stderr:
                        print(r.stderr, file=sys.stderr)
                    return 2
                print("run_regression_suite: UART TXREG check OK")
        exit_code = 0
        return 0
    finally:
        if suites_started:
            _write_gpsim_regression_summary_file(
                root,
                args,
                suite_ids,
                exit_code,
                passed=gpsim_passed,
                failed=gpsim_failed,
                tested=gpsim_tested,
                checkpoint_failed_suite=checkpoint_failed_suite,
            )
            if os.environ.get("JALV2_GPSIM_SUBPROCESS", "").strip().lower() not in ("1", "true", "yes"):
                _suite_snap = snapshot_test_report_trees(
                    root, label="gpsim_regression_suite", quiet=args.quiet
                )
                _web_snap: Path | None = None
                try:
                    from publish_unit_test_web import publish_gpsim_regression_summary_to_web_root

                    _web_snap = publish_gpsim_regression_summary_to_web_root(
                        str(root), quiet=args.quiet
                    )
                except Exception as e:
                    if not args.quiet:
                        print(
                            f"run_regression_suite: warning: gpsim web copy failed: {e}",
                            file=sys.stderr,
                        )
                _bl = " --baseline" if args.baseline else ""
                _suites = ",".join(suite_ids)
                _full = registered_suite_ids_ordered()
                _changelog_ok = (
                    list(suite_ids) == _full
                    and checkpoint_failed_suite is None
                ) or _env_forces_gpsim_changelog_on_partial()
                if _changelog_ok:
                    append_changelog_test_automation(
                        root,
                        summary=(
                            f"Tests (`sims/run_regression_suite.py`{_bl}; suites {_suites}): exit {exit_code}; "
                            f"{gpsim_passed} passed, {gpsim_failed} failed / {gpsim_tested} tested"
                        ),
                        report_snapshot=_suite_snap,
                        web_snapshot=_web_snap,
                        quiet=args.quiet,
                    )
                elif not args.quiet:
                    print(
                        "run_regression_suite: CHANGELOG.txt unchanged (partial suite run; "
                        "full ordered suite list matches --suite all / --suite-all). "
                        "Override: JALV2_GPSIM_CHANGELOG_PARTIAL=1. "
                        "JALV2_NO_CHANGELOG_ENTRY=1 still skips any append when eligible."
                    )


if __name__ == "__main__":
    raise SystemExit(main())

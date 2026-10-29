#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
Comprehensive Unit Test Script for JALv2 Compiler
Tests all samples in the third_party/jallib/samples directory
Saves all outputs to organized directory structure

Writes unit-tests/Output_Compare/current_error_messages.txt and current_warning_messages.txt
on --current and --compare (parsed compiler diagnostics for the unit-test web dashboard). Compare mode runs
validate_hex_static with a stderr progress bar; static validation uses up to 8 threads for scan/diff.
Sample compiles use ThreadPoolExecutor; set --parallel N, -j N, or JALV2_TEST_JOBS (default 16).
The runner drops itself to the lowest CPU (nice 19) and I/O (ionice idle) priority at startup via
unit-tests/_lowprio.py (shared with the gpsim/PIC32/ESP32 suite runners); every compiler / regression
subprocess and later-created worker thread inherits it.

After baseline/current/compare runs, report files are published to JALV2_UNIT_TEST_WEB_ROOT
(default /var/www/jalv2_unit_test) via publish_unit_test_web.publish_dashboard() (CLI: publish_unit_test_web.py).
Set JALV2_UNIT_TEST_WEB_ROOT to the private dashboard path; publish failures are warned but do not abort the run.
After --gpsim-regression, only gpsim_regression_summary.txt is copied into WEB_ROOT/data/ via
publish_unit_test_web.publish_gpsim_regression_summary_to_web_root().

`--pic32` compiles the PIC32 `@sim_validate` suite (`sims/pic32/32mz1024efh064_*.jal`)
with `-s demo_lib;third_party/jallib/lib;tests` (requires `--compiler PATH`; baseline jalv25r9 cannot emit MIPS).
`--pic32-regression` runs `sims/run_pic32_regression.py` (QEMU). Both may be combined.

IMPORTANT: When this script is updated, UNITTESTS.md must also be updated
to reflect any changes in functionality, command line options, or output structure.

`--current` requires `--compiler PATH` (no implicit choice between `bin/jalv2-original` and `bin/jalv3-x86-64`).

**Ephemeral shadow:** Each run copies **`third_party/jallib/samples/`** and **`demo_lib/`** into **`unit-tests/.unittests_shadow/{baseline,current}/<run_id>/`** (unique **`run_id`** per process — **`fixes/fix_unittests_shadow_concurrent_run_rmtree.md`**). No automatic **`COUNT(`**↔**`_LENGTH(`** text rewriting; the repo sources are compiled as-is. The shadow tree is removed in **`finally`**.

With no CLI arguments, the script prints help and exits. Each baseline/current run clears unit-tests/Comparison/reports and Output_Compare first, then
after the run a snapshot copy is saved under unit-tests/Comparison/report_archive/<timestamp>_<label>/.
Compare mode clears Output_Compare at start and snapshots after. Gpsim suite clears only
gpsim_regression_summary.txt before running; unittests --gpsim-regression snapshots after the full log
is written, then copies only that summary into WEB_ROOT/data/. Set JALV2_NO_REPORT_ARCHIVE=1 to skip snapshots (clearing still runs). Set JALV2_NO_CHANGELOG_ENTRY=1 to skip CHANGELOG append. Full unit-test web publish clears
data/ then, after copying files, snapshots the new data/ under WEB_ROOT/archive/… Completed runs append
one line to CHANGELOG.txt (append_changelog_test_automation); JALV2_NO_CHANGELOG_ENTRY=1 skips.


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""

import json
import os
import subprocess
import glob
import shutil
import sys
import re
import time
import shlex
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from typing import Optional

# unittests.py lives in tests/: script dir = tests/, repo root one level up.
_tests_dir = os.path.dirname(os.path.abspath(__file__))
_repo_root = os.path.dirname(_tests_dir)
_scripts_dir = os.path.join(_repo_root, "scripts")
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)

from archive_test_reports import (
    append_changelog_test_automation,
    clear_comparison_reports_and_output_compare,
    clear_output_compare_only,
    snapshot_test_report_trees,
)

INTERNAL_ERROR_PATTERNS = [
    r"ERROR:\s+Too many variables in block",
    # libc / compiler abort signatures — these can appear even when a
    # summary line was printed, so match them independently.
    r"Assertion `[^']*' failed",
    r"Segmentation fault",
    r"Aborted \(core dumped\)",
    r"stack smashing detected",
    r"internal compiler error",
]

# Ephemeral copies of Sample/ + third_party/jallib/lib/ + demo_lib/ (see _unit_test_prepare_shadow_sources).
_UNITTEST_SHADOW_SUBDIR = os.path.join("unit-tests", ".unittests_shadow")


def _unit_test_prepare_shadow_sources(repo_root: str, for_baseline_compiler: bool) -> tuple[str, str, str, str]:
    """
    Copy third_party/jallib/samples, third_party/jallib/lib/ and demo_lib/ into a fresh tree (ephemeral; unique per run).
    Return (shadow_root, abs_sample_dir, lib_search_path, note).

    ``note`` is a short line for the summary (for_baseline_compiler is unused but kept
    for call-site clarity).
    """
    label = "baseline" if for_baseline_compiler else "current"
    run_id = f"run_{os.getpid()}_{time.time_ns()}"
    shadow_root = os.path.join(repo_root, _UNITTEST_SHADOW_SUBDIR, label, run_id)
    os.makedirs(shadow_root, exist_ok=True)
    src_sample = os.path.join(repo_root, "third_party", "jallib", "samples")
    src_lib = os.path.join(repo_root, "third_party/jallib/lib")
    src_lib_new = os.path.join(repo_root, "demo_lib")
    dst_sample = os.path.join(shadow_root, "Sample")
    dst_lib = os.path.join(shadow_root, "third_party/jallib/lib")
    dst_lib_new = os.path.join(shadow_root, "demo_lib")
    shutil.copytree(src_sample, dst_sample)
    shutil.copytree(src_lib, dst_lib)
    shutil.copytree(src_lib_new, dst_lib_new)
    note = "copy only (no COUNT/_LENGTH text rewrite)"
    return (
        shadow_root,
        os.path.abspath(dst_sample),
        os.path.abspath(dst_lib_new) + ";" + os.path.abspath(dst_lib),
        note,
    )


# Default matches server dashboard at http://<host>:8000/ when served from this root.
_DEFAULT_UNIT_TEST_WEB_ROOT = "/var/www/jalv2_unit_test"
_GPSIM_SUMMARY_FILE = os.path.join(
    "unit-tests", "Comparison", "reports", "gpsim_regression_summary.txt"
)
_GPSIM_RUN_STATS_JSON = os.path.join(
    "unit-tests", "Comparison", "reports", "gpsim_regression_run.json"
)
_PIC32_SUITE_GLOB = os.path.join("sims", "pic32", "32mz1024efh064_*.jal")
_PIC32_COMPILE_SUMMARY = os.path.join(
    "unit-tests", "Comparison", "reports", "pic32_compile_summary.txt"
)
_PIC32_REGRESSION_SUMMARY = os.path.join(
    "unit-tests", "Comparison", "reports", "pic32_regression_summary.txt"
)


def _report_timestamp_first_line() -> str:
    """Local date and time for line 1 of generated text reports (YYYY-MM-DD HH:MM:SS)."""
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def publish_unit_test_dashboard():
    """
    Copy compare/summary .txt and dashboard static files to JALV2_UNIT_TEST_WEB_ROOT
    (default /var/www/jalv2_unit_test). Safe to call after --baseline, --current, or --compare.

    Returns the path to WEB_ROOT/archive/<timestamp>_publish/ when snapshotted, else None.
    """
    repo_root = _repo_root
    pub = os.path.join(repo_root, "publish_unit_test_web.py")
    if not os.path.isfile(pub):
        print("Warning: publish_unit_test_web.py not found; skipping web publish")
        return None
    web_root = os.environ.get("JALV2_UNIT_TEST_WEB_ROOT", _DEFAULT_UNIT_TEST_WEB_ROOT)
    print("\n=== Publishing unit test web dashboard ===")
    print(f"    JALV2_UNIT_TEST_WEB_ROOT={web_root}")
    try:
        from publish_unit_test_web import publish_dashboard
    except ImportError as e:
        print(f"Warning: could not import publish_dashboard: {e}")
        return None
    try:
        snap = publish_dashboard(repo_root, web_root=web_root, quiet=False)
    except Exception as e:
        print(f"Warning: publish_dashboard failed: {e}")
        return None
    print(f"    Published: {web_root}/ (data in {web_root}/data/)")
    return snap

def parse_error_warning_counts(text: str):
    """
    Parse compiler error/warning counts and treat configured internal ERROR lines
    as real errors that add to the error total.
    """
    # Find the final error/warning count line first
    # Use the *last* "N errors, M warnings" line (compiler may print more than once;
    # the final line matches jal_main.c pfile_log summary).
    m_all = list(re.finditer(r"(\d+) errors?,?\s*(\d+) warnings?", text))
    if m_all:
        m = m_all[-1]
        errors = int(m.group(1))
        warnings = int(m.group(2))
    else:
        # No "N errors, M warnings" line at all: the compiler aborted before the
        # summary (assertion/signal) or produced malformed output. That is a
        # failure, not 0 errors — the `alias X is <procedure>` abort hid
        # exactly this way (PASSED via the missing-summary fallback).
        errors = text.count(" error")
        warnings = text.count(" warning")
        internal_errors = 1
        errors += internal_errors
        return errors, warnings, internal_errors

    # Add internal diagnostics that should be considered compiler errors
    internal_errors = 0
    for pattern in INTERNAL_ERROR_PATTERNS:
        internal_errors += len(re.findall(pattern, text))

    errors += internal_errors
    return errors, warnings, internal_errors


def _extract_body_from_saved_full_output(text: str) -> str:
    """
    Strip the wrapper written around compiler runs; return STDOUT + STDERR body.
    """
    if "STDOUT:" not in text:
        return text
    i = text.find("STDOUT:")
    chunk = text[i + len("STDOUT:") :].lstrip("\n")
    if "STDERR:" in chunk:
        out_part, rest = chunk.split("STDERR:", 1)
        err_part = rest.lstrip("\n")
        return (out_part.rstrip() + "\n" + err_part.rstrip()).strip()
    return chunk.strip()


def _compiler_output_contains_error_tag(text: str) -> bool:
    """
    True if log likely contains a compiler error diagnostic (not just the word 'error'
    inside unrelated text). Matches legacy Error:/ERROR: and jalv2 path:line: error:
    """
    if "Error:" in text or "ERROR:" in text:
        return True
    if re.search(r"\.jal\s*:\s*\d+\s*:\s*error\s*:", text, re.I):
        return True
    return False


def extract_compiler_error_messages(text: str, max_lines: int = 200) -> list[str]:
    """
    Pull human-readable compiler diagnostics from a saved full_output file.
    Keeps typical `path.jal:line: message` lines, Error:/ERROR: lines, and drops
    pass summaries (e.g. 'branches checked, 0 errors') and the final count line.
    """
    body = _extract_body_from_saved_full_output(text)
    summary_re = re.compile(r"^\s*\d+\s+errors?,\s*\d+\s+warnings?\s*$", re.I)
    check_re = re.compile(r"\schecked,\s*\d+\s+errors?\s*$", re.I)
    kept: list[str] = []
    for line in body.splitlines():
        s = line.rstrip()
        if not s:
            continue
        if summary_re.match(s):
            continue
        if check_re.search(s):
            continue
        low = s.lower()
        if low.startswith("jal jalv2") or low.startswith("generating "):
            continue
        if re.match(r"^\s*\d+\s+tokens?,\s*", s):
            continue
        if re.match(r"^writing result\s*$", s, re.I):
            continue
        if any(
            s.startswith(p)
            for p in (
                "Code area:",
                "Data area:",
                "Software stack",
                "Hardware stack",
            )
        ):
            continue
        if re.search(r"\.jal:\d+:", s, re.I):
            kept.append(s)
            continue
        if "ERROR:" in s or s.startswith("Error:") or ": Error:" in s:
            kept.append(s)
    return kept[:max_lines]


def extract_compiler_warning_messages(text: str, max_lines: int = 200) -> list[str]:
    """
    Pull warning lines from a saved full_output file (path.jal:line: warning: …).
    """
    body = _extract_body_from_saved_full_output(text)
    summary_re = re.compile(r"^\s*\d+\s+errors?,\s*\d+\s+warnings?\s*$", re.I)
    check_re = re.compile(r"\schecked,\s*\d+\s+(errors?|warnings?)\s*$", re.I)
    kept: list[str] = []
    for line in body.splitlines():
        s = line.rstrip()
        if not s:
            continue
        if summary_re.match(s):
            continue
        if check_re.search(s):
            continue
        low = s.lower()
        if low.startswith("jal jalv2") or low.startswith("generating "):
            continue
        if re.match(r"^\s*\d+\s+tokens?,\s*", s):
            continue
        if re.match(r"^writing result\s*$", s, re.I):
            continue
        if any(
            s.startswith(p)
            for p in (
                "Code area:",
                "Data area:",
                "Software stack",
                "Hardware stack",
            )
        ):
            continue
        if re.search(r"\.jal:\d+:", s, re.I) and "warning" in low:
            kept.append(s)
            continue
        if "warning:" in low and ".jal" in s:
            kept.append(s)
    return kept[:max_lines]


def selftest_warning_extraction_stack_overflow_extended() -> None:
    """
    Guard: extended hardware-stack overflow text must still be extracted as a
    warning line for current_warning_messages.txt (see
    fixes/fix_hardware_stack_overflow_unit_samples.md).
    """
    sample = (
        "third_party/jallib/samples/x.jal:42: warning: Hardware stack overflow! "
        "(main+tasks depth 16, total with ISR 17, limit 8)\n"
    )
    msgs = extract_compiler_warning_messages(sample)
    if not msgs:
        raise AssertionError("expected at least one warning line from sample text")
    line0 = msgs[0]
    if "main+tasks depth" not in line0 or "limit 8" not in line0:
        raise AssertionError(f"unexpected extraction: {msgs!r}")


def write_current_error_messages_report(full_output_dir: str, out_path: str) -> None:
    """
    Write unit-tests/Output_Compare/current_error_messages.txt for the web dashboard.
    """
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as out:
        out.write(f"{_report_timestamp_first_line()}\n\n")
        out.write("COMPILER ERROR MESSAGES (CURRENT)\n")
        out.write("==================================\n")
        out.write("Source: Output_Current/full_output/*_output.txt (samples with error count > 0)\n\n")
        if not os.path.isdir(full_output_dir):
            out.write("(no full_output directory)\n")
            return
        any_written = False
        for name in sorted(fn for fn in os.listdir(full_output_dir) if fn.endswith("_output.txt")):
            path = os.path.join(full_output_dir, name)
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as inf:
                    text = inf.read()
                err_count, _w, _int = parse_error_warning_counts(text)
                if err_count <= 0:
                    continue
                any_written = True
                base = name.replace("_output.txt", "")
                msgs = extract_compiler_error_messages(text)
                out.write("---\n")
                out.write(f"Sample: {base}.jal\n")
                out.write(f"Parsed error count: {err_count}\n")
                out.write("---\n")
                if msgs:
                    for m in msgs:
                        out.write(m + "\n")
                else:
                    out.write(
                        "(no lines matched diagnostic patterns; see full log under "
                        "Output_Compare/samples_with_errors/ or Output_Current/full_output/)\n"
                    )
                out.write("\n")
            except OSError:
                continue
        if not any_written:
            out.write("(no samples with errors in this run)\n")


def write_current_warning_messages_report(full_output_dir: str, out_path: str) -> None:
    """
    Write unit-tests/Output_Compare/current_warning_messages.txt for the web dashboard.
    """
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as out:
        out.write(f"{_report_timestamp_first_line()}\n\n")
        out.write("COMPILER WARNING MESSAGES (CURRENT)\n")
        out.write("====================================\n")
        out.write("Source: Output_Current/full_output/*_output.txt (samples with warning count > 0)\n\n")
        if not os.path.isdir(full_output_dir):
            out.write("(no full_output directory)\n")
            return
        any_written = False
        for name in sorted(fn for fn in os.listdir(full_output_dir) if fn.endswith("_output.txt")):
            path = os.path.join(full_output_dir, name)
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as inf:
                    text = inf.read()
                _e, w, _int = parse_error_warning_counts(text)
                if w <= 0:
                    continue
                any_written = True
                base = name.replace("_output.txt", "")
                msgs = extract_compiler_warning_messages(text)
                out.write("---\n")
                out.write(f"Sample: {base}.jal\n")
                out.write(f"Parsed warning count: {w}\n")
                out.write("---\n")
                if msgs:
                    for m in msgs:
                        out.write(m + "\n")
                else:
                    out.write(
                        "(no lines matched warning patterns; see full log under "
                        "Output_Compare/samples_with_warnings/ or Output_Current/full_output/)\n"
                    )
                out.write("\n")
            except OSError:
                continue
        if not any_written:
            out.write("(no samples with warnings in this run)\n")


def clean_old_reports():
    """Clean/delete old test reports and output directories"""
    print("=== CLEANING OLD REPORTS ===")
    print("Removing old test outputs and reports...")
    
    # Directories to clean
    dirs_to_clean = [
        "unit-tests/Output_Baseline",
        "unit-tests/Output_Current", 
        "unit-tests/Output_Compare",
        "unit-tests/Output_PIC32",
        "unit-tests/Comparison/reports"
    ]
    
    cleaned_count = 0
    for dir_path in dirs_to_clean:
        if os.path.exists(dir_path):
            try:
                shutil.rmtree(dir_path)
                print(f"✅ Removed: {dir_path}")
                cleaned_count += 1
            except Exception as e:
                print(f"❌ Failed to remove {dir_path}: {e}")
        else:
            print(f"ℹ️  Directory not found: {dir_path}")
    
    print(f"\nCleaned {cleaned_count} directories")
    print("✅ Old reports cleaned successfully!")
    print("")

def _sample_compile_worker_count(parallel_workers=None):
    """
    Thread count for compiling samples in test_all_samples.
    CLI --parallel / -j overrides env JALV2_TEST_JOBS (default 16).
    """
    if parallel_workers is not None:
        w = parallel_workers
    else:
        try:
            w = int(os.environ.get("JALV2_TEST_JOBS", "16"))
        except ValueError:
            w = 16
    return max(1, min(64, w))


def _sample_compile_timeout_sec():
    """
    Per-sample subprocess timeout for jalv2 in test_all_samples.
    Env JALV2_UNIT_TEST_COMPILE_TIMEOUT overrides (seconds; default 180).
    """
    try:
        t = int(os.environ.get("JALV2_UNIT_TEST_COMPILE_TIMEOUT", "180"))
    except ValueError:
        t = 180
    return max(1, t)


def _cli_compiler_path(args):
    """Return --compiler PATH / --compiler=PATH from a argv slice, else None."""
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--compiler" and i + 1 < len(args):
            return args[i + 1]
        if a.startswith("--compiler="):
            return a.split("=", 1)[1]
        i += 1
    return None


def _cli_sample_limit(args):
    """Optional first-N integer from a argv slice (skips --compiler PATH)."""
    n = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--compiler" and i + 1 < len(args):
            i += 2
            continue
        if a.startswith("--compiler="):
            i += 1
            continue
        try:
            n = int(a)
        except ValueError:
            pass
        i += 1
    return n


def _pic32_compiler_args(jal_path):
    """Tokens from `-- compiler_args:` lines (same contract as jal_sim_validate)."""
    extra = []
    try:
        with open(jal_path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return extra
    for line in text.splitlines():
        s = line.strip()
        if s.lower().startswith("-- compiler_args:"):
            rest = s.split(":", 1)[1].strip()
            if rest:
                extra.extend(rest.split())
    return extra


def test_pic32_compile(compiler_path, parallel_workers=None, max_samples=None):
    """
    Compile sims/pic32/32mz1024efh064_*.jal (no QEMU).

    Same compile contract as jal_sim_validate.sh: -s demo_lib;third_party/jallib/lib;tests,
    require the summary line, 0 errors, 0 warnings, non-empty HEX.
    Writes unit-tests/Output_PIC32/ and pic32_compile_summary.txt.
    Returns a stats dict including ``failed``.
    """
    repo = _repo_root
    jal_files = sorted(glob.glob(os.path.join(repo, _PIC32_SUITE_GLOB)))
    if max_samples and max_samples > 0:
        jal_files = jal_files[:max_samples]
    if not jal_files:
        print(f"error: no PIC32 suite files matching {_PIC32_SUITE_GLOB}", file=sys.stderr)
        return {"failed": 1, "passed": 0, "samples": 0, "report_snapshot": None}

    out_root = os.path.join(repo, "unit-tests", "Output_PIC32")
    for sub in ("hex", "asm", "full_output"):
        os.makedirs(os.path.join(out_root, sub), exist_ok=True)
    os.makedirs(os.path.join(repo, "unit-tests", "Comparison", "reports"), exist_ok=True)

    # lib first: 32-bit variants + fork chipdef shadow the pristine
    # jallib files for the PIC32 suite (see fixes/fix_lib_new_shadow.md)
    lib_s = os.path.join(repo, "demo_lib") + ";" + os.path.join(repo, "third_party/jallib/lib")
    gp_s = os.path.join(repo, "sims", "shared")
    search = lib_s + ";" + gp_s
    timeout_sec = _sample_compile_timeout_sec()
    workers = _sample_compile_worker_count(parallel_workers)

    print("=== PIC32 COMPILE PATH ===")
    print(f"Compiler: {compiler_path}")
    print(f"Search:   {search}")
    print(f"Files:    {len(jal_files)} ({_PIC32_SUITE_GLOB})")
    print(f"Workers:  {workers}")
    print(f"Timeout:  {timeout_sec}s")
    print("")

    def run_one(jal_file):
        filename = os.path.basename(jal_file)
        base = filename[:-4] if filename.endswith(".jal") else filename
        hex_path = os.path.join(out_root, "hex", base + ".hex")
        asm_path = os.path.join(out_root, "asm", base + ".asm")
        extra = _pic32_compiler_args(jal_file)
        cmd = [
            compiler_path,
            "-s",
            search,
            "-no-codfile",
            "-asm",
            asm_path,
            "-hex",
            hex_path,
        ] + extra + [os.path.abspath(jal_file)]
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout_sec,
                cwd=repo,
            )
            output = (result.stdout or "") + (result.stderr or "")
            out_file = os.path.join(out_root, "full_output", base + "_output.txt")
            with open(out_file, "w", encoding="utf-8") as fh:
                fh.write(f"{_report_timestamp_first_line()}\n\n")
                fh.write(f"Compiler Output for: {filename}\n")
                fh.write("=" * 50 + "\n\n")
                fh.write("CMD: " + " ".join(shlex.quote(str(a)) for a in cmd) + "\n\n")
                fh.write("STDOUT:\n")
                fh.write(result.stdout or "")
                fh.write("\n\nSTDERR:\n")
                fh.write(result.stderr or "")
            errors, warnings, internal_errors = parse_error_warning_counts(output)
            hex_ok = os.path.isfile(hex_path) and os.path.getsize(hex_path) > 0
            # Match jal_sim_validate.sh: warnings-as-errors + required HEX.
            passed = (errors == 0 and warnings == 0 and hex_ok)
            return {
                "filename": filename,
                "errors": errors,
                "warnings": warnings,
                "internal_errors": internal_errors,
                "hex_ok": hex_ok,
                "passed": passed,
                "timeout": False,
                "exception": None,
            }
        except subprocess.TimeoutExpired:
            return {
                "filename": filename,
                "errors": 1,
                "warnings": 0,
                "internal_errors": 0,
                "hex_ok": False,
                "passed": False,
                "timeout": True,
                "exception": None,
            }
        except Exception as e:
            return {
                "filename": filename,
                "errors": 1,
                "warnings": 0,
                "internal_errors": 0,
                "hex_ok": False,
                "passed": False,
                "timeout": False,
                "exception": str(e),
            }

    passed = 0
    failed = 0
    failed_files = []
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(run_one, jal): jal for jal in jal_files}
        for fut in as_completed(futs):
            r = fut.result()
            name = r["filename"]
            if r["timeout"]:
                print(f"  [TIMEOUT] {name}")
                failed += 1
                failed_files.append(name)
            elif r["exception"] is not None:
                print(f"  [EXCEPTION] {name}: {r['exception']}")
                failed += 1
                failed_files.append(name)
            elif r["passed"]:
                print(f"  [PASS] {name}")
                passed += 1
            else:
                why = []
                if r["errors"]:
                    why.append(f"{r['errors']} errors")
                if r["warnings"]:
                    why.append(f"{r['warnings']} warnings")
                if not r["hex_ok"]:
                    why.append("missing HEX")
                print(f"  [FAIL] {name} ({', '.join(why) or 'failed'})")
                failed += 1
                failed_files.append(name)

    n = len(jal_files)
    print("")
    print(f"=== PIC32 COMPILE RESULT: {passed}/{n} PASS ===")
    if failed_files:
        print("Failed:")
        for name in failed_files:
            print(f"  {name}")

    summary_path = os.path.join(repo, _PIC32_COMPILE_SUMMARY)
    with open(summary_path, "w", encoding="utf-8") as fh:
        fh.write(f"{_report_timestamp_first_line()}\n\n")
        fh.write("PIC32 COMPILE PATH\n")
        fh.write("=" * 50 + "\n")
        fh.write(f"Compiler: {compiler_path}\n")
        fh.write(f"Search: {search}\n")
        fh.write(f"Glob: {_PIC32_SUITE_GLOB}\n")
        fh.write(f"Results: {passed} passed, {failed} failed ({n} .jal files tested).\n")
        if failed_files:
            fh.write("\nFAILED:\n")
            for name in failed_files:
                fh.write(f"  {name}\n")
    print(f"Summary: {_PIC32_COMPILE_SUMMARY}")
    snap = snapshot_test_report_trees(repo, label="pic32_compile", quiet=False)
    return {
        "failed": failed,
        "passed": passed,
        "samples": n,
        "report_snapshot": snap,
    }


def _print_unittests_usage() -> None:
    """Print full CLI help (same as ``--help`` and when no arguments are passed)."""
    print("Usage: python3 unittests.py [--baseline|--current|--compare|--clean|--gpsim-regression|--pic32|--pic32-regression] [number_of_samples]")
    print("")
    print("  With no arguments, this help is shown. Run sample tests with --baseline or")
    print("  --current --compiler PATH (and optional sample limit).")
    print("")
    print("  --baseline, -b    Use baseline compiler (bin/old_versions/jalv25r9-x86-64)")
    print("  --current, -c     Use a compiler you choose (requires --compiler PATH)")
    print("  --compiler PATH   Required with --current and --pic32: jalv2 binary (e.g. ./bin/jalv3-x86-64)")
    print("  --compare         Compare baseline vs current; writes Output_Compare/ including static_validation/")
    print("  --clean           Clean/delete old test reports and output directories")
    print("  --gpsim-regression  Run sims/run_regression_suite.py (needs gpsim on PATH);")
    print("                      saves log to unit-tests/Comparison/reports/gpsim_regression_summary.txt")
    print("                      Passes --run when no suite flags (default: all device suites);")
    print("                      also --baseline/-b, --quiet/-q, --fail-fast, --suite ID, --suite-all")
    print("  --pic32           Compile PIC32 suite (sims/pic32/32mz1024efh064_*.jal);")
    print("                      requires --compiler PATH; HEX/ASM under unit-tests/Output_PIC32/")
    print("  --pic32-regression  Run sims/run_pic32_regression.py (QEMU); optional --compiler")
    print("                      --quiet --fail-fast; --parallel N maps to --jobs N")
    print("  --help, -h        Show this help message and exit")
    print("  --parallel N, -j N   Sample compile thread pool (default: env JALV2_TEST_JOBS or 16)")
    print("  Env JALV2_UNIT_TEST_COMPILE_TIMEOUT   Per-sample jalv2 compile timeout in seconds (default 180)")
    print("  number            Limit to first N samples")
    print("")
    print("  The runner starts at the lowest CPU/IO priority (nice 19 + ionice idle class);")
    print("  all compiler and regression subprocesses inherit it, so runs yield to foreground")
    print("  work and still run at full speed on an otherwise idle machine.")
    print("  Sample / lib sources: each --baseline / --current run copies to")
    print("  unit-tests/.unittests_shadow/{baseline,current}/run_<pid>_<ns>/. Per-file rewrites use third_party/jallib/lib/strings.jal as")
    print("  a probe: baseline rewrites all .jal only if strings.jal contains _LENGTH(; current only if")
    print("  it contains COUNT(; if demo_lib/strings.jal is missing, all .jal are rewritten. Repo trees unchanged.")
    print("")
    print("  After --baseline, --current, or --compare, the full dashboard is published automatically")
    print("  (publish_unit_test_web.publish_dashboard). Set JALV2_UNIT_TEST_WEB_ROOT to your private dashboard path.")
    print("  After --gpsim-regression, only gpsim_regression_summary.txt is copied into WEB_ROOT/data/")
    print("  (publish_gpsim_regression_summary_to_web_root). --pic32 writes pic32_compile_summary.txt;")
    print("  --pic32-regression writes pic32_regression_summary.txt (no dashboard publish).")
    print(f"  Default web root if unset: {_DEFAULT_UNIT_TEST_WEB_ROOT}; override with JALV2_UNIT_TEST_WEB_ROOT.")
    print("")
    print("  Report trees are cleared before each --baseline/--current run; after the run a snapshot")
    print("  copy is saved under unit-tests/Comparison/report_archive/. Compare clears Output_Compare")
    print("  first. Full publish clears WEB_ROOT/data/ then snapshots the new data/ after copy.")
    print("  Set JALV2_NO_REPORT_ARCHIVE=1 to skip snapshots only (clears still run).")
    print("  Set JALV2_NO_CHANGELOG_ENTRY=1 to skip appending the post-run line to CHANGELOG.txt.")
    print("")


def test_all_samples(
    max_samples=None,
    use_baseline=True,
    parallel_workers=None,
    current_compiler_path=None,
):
    _repo = _repo_root
    _lbl = "unit_baseline" if use_baseline else "unit_current"
    # Keep the other run's summary in Comparison/reports/ so publish + dashboard still have both.
    if use_baseline:
        clear_comparison_reports_and_output_compare(
            _repo,
            quiet=False,
            preserve_reports=("current_test_summary.txt",),
        )
    else:
        clear_comparison_reports_and_output_compare(
            _repo,
            quiet=False,
            preserve_reports=("baseline_test_summary.txt",),
        )

    # Determine compiler and output directories
    if use_baseline:
        compiler_path = os.path.join(_repo, "bin", "old_versions", "jalv25r9-x86-64")
        if not os.path.isfile(compiler_path):
            print(
                f"error: baseline compiler not found: {compiler_path}",
                file=sys.stderr,
            )
            print(
                "  Expected bin/old_versions/jalv25r9-x86-64 (e.g. jalv25r9).",
                file=sys.stderr,
            )
            raise SystemExit(2)
        output_type = "Baseline"
        compiler_desc = "baseline compiler (bin/old_versions/jalv25r9-x86-64)"
    else:
        if not current_compiler_path:
            raise ValueError("current_compiler_path is required when use_baseline is False")
        compiler_path = current_compiler_path
        output_type = "Current"
        compiler_desc = f"current compiler ({compiler_path})"
    
    print("=== COMPREHENSIVE UNIT TEST - ALL SAMPLES ===")
    print(f"Testing all samples with {compiler_desc}")
    print("Date:", subprocess.check_output(["date"], text=True).strip())
    print(f"Compiler: {compiler_path}")
    print("")
    
    # Create output directories
    output_dirs = [
        f"unit-tests/Output_{output_type}/hex",
        f"unit-tests/Output_{output_type}/asm", 
        f"unit-tests/Output_{output_type}/cod",
        f"unit-tests/Output_{output_type}/full_output",
        "unit-tests/Comparison/reports"
    ]
    
    for dir_path in output_dirs:
        os.makedirs(dir_path, exist_ok=True)
        print(f"✅ Created directory: {dir_path}")
    
    print("")

    (
        shadow_root,
        sample_dir,
        lib_path,
        _shadow_note,
    ) = _unit_test_prepare_shadow_sources(
        _repo, for_baseline_compiler=use_baseline
    )
    print("✅ Shadow tree (third_party/jallib/samples + third_party/jallib/lib/ + demo_lib/): " + _shadow_note)
    print(f"   (ephemeral: {shadow_root})")
    print("")

    try:
        # Get all JAL files (paths point into the shadow tree, not the repo working copies)
        jal_files = glob.glob(os.path.join(sample_dir, "*.jal"))

        # Limit samples if specified
        if max_samples and max_samples > 0:
            jal_files = jal_files[:max_samples]
            print(f"Found {len(jal_files)} sample files (limited to first {max_samples})")
        else:
            print(f"Found {len(jal_files)} sample files")
        print("")

        compile_timeout_sec = _sample_compile_timeout_sec()

        # Test results
        passed = 0
        failed = 0
        total_errors = 0
        total_warnings = 0
        max_warnings = 0
        samples_with_warnings = 0
        failed_files = []
        missing_hex_names = []
        missing_hex_when_passed_names = []
    
        def run_single_sample(jal_file):
            filename = os.path.basename(jal_file)
            base_name = filename.replace('.jal', '')
            try:
                # Run compiler
                result = subprocess.run(
                    [compiler_path, "-s", lib_path, os.path.abspath(jal_file)],
                    capture_output=True,
                    text=True,
                    timeout=compile_timeout_sec,
                )
            
                # Check for errors and warnings
                output = result.stdout + result.stderr
            
                # Save full compiler output
                output_file = f"unit-tests/Output_{output_type}/full_output/{base_name}_output.txt"
                with open(output_file, 'w') as f:
                    f.write(f"{_report_timestamp_first_line()}\n\n")
                    f.write(f"Compiler Output for: {filename}\n")
                    f.write("=" * 50 + "\n\n")
                    f.write("STDOUT:\n")
                    f.write(result.stdout)
                    f.write("\n\nSTDERR:\n")
                    f.write(result.stderr)
            
                # Save generated files (.hex, .asm, .cod)
                local_sample_dir = os.path.dirname(jal_file)
                saved_exts = []
                for ext in ['hex', 'asm', 'cod']:
                    source_file = os.path.join(local_sample_dir, f"{base_name}.{ext}")
                    if os.path.exists(source_file):
                        dest_file = f"unit-tests/Output_{output_type}/{ext}/{base_name}.{ext}"
                        shutil.copy2(source_file, dest_file)
                        saved_exts.append(ext.upper())
                hex_path = os.path.join(local_sample_dir, f"{base_name}.hex")
                hex_present = os.path.isfile(hex_path)
            
                errors, warnings, internal_errors = parse_error_warning_counts(output)
            
                return {
                    "filename": filename,
                    "base_name": base_name,
                    "errors": errors,
                    "warnings": warnings,
                    "passed": (errors == 0),
                    "internal_errors": internal_errors,
                    "saved_exts": saved_exts,
                    "hex_present": hex_present,
                    "output": output,
                    "timeout": False,
                    "exception": None,
                }
            except subprocess.TimeoutExpired:
                output_file = (
                    f"unit-tests/Output_{output_type}/full_output/{base_name}_output.txt"
                )
                try:
                    with open(output_file, "w") as tf:
                        tf.write(f"{_report_timestamp_first_line()}\n\n")
                        tf.write(f"Compiler Output for: {filename}\n")
                        tf.write("=" * 50 + "\n\n")
                        tf.write(
                            "TIMEOUT: subprocess.run exceeded "
                            f"{compile_timeout_sec}s "
                            "(override with JALV2_UNIT_TEST_COMPILE_TIMEOUT).\n"
                        )
                except OSError:
                    pass
                return {
                    "filename": filename,
                    "base_name": base_name,
                    "errors": 0,
                    "warnings": 0,
                    "passed": False,
                    "internal_errors": 0,
                    "saved_exts": [],
                    "hex_present": False,
                    "output": "",
                    "timeout": True,
                    "exception": None,
                }
            except Exception as e:
                return {
                    "filename": filename,
                    "base_name": base_name,
                    "errors": 0,
                    "warnings": 0,
                    "passed": False,
                    "internal_errors": 0,
                    "saved_exts": [],
                    "hex_present": False,
                    "output": "",
                    "timeout": False,
                    "exception": str(e),
                }

        _workers = _sample_compile_worker_count(parallel_workers)
        print(f"Running tests with {_workers} parallel workers")
        print(
            f"Per-sample compile timeout: {compile_timeout_sec}s "
            "(env JALV2_UNIT_TEST_COMPILE_TIMEOUT)"
        )
        print("")

        with ThreadPoolExecutor(max_workers=_workers) as executor:
            future_to_index = {
                executor.submit(run_single_sample, jal_file): i
                for i, jal_file in enumerate(jal_files, 1)
            }

            for future in as_completed(future_to_index):
                i = future_to_index[future]
                result = future.result()
                filename = result["filename"]
                print(f"=== TEST {i}: {filename} ===")

                for ext in result["saved_exts"]:
                    print(f"   📁 Saved: {ext} file")

                if result["timeout"]:
                    print("⏰ TIMEOUT - Test took too long")
                    failed += 1
                    failed_files.append(filename)
                elif result["exception"] is not None:
                    print(f"💥 EXCEPTION - {result['exception']}")
                    failed += 1
                    failed_files.append(filename)
                else:
                    if result["passed"]:
                        print(f"✅ PASSED - 0 errors, {result['warnings']} warnings")
                        passed += 1
                    else:
                        print(f"❌ FAILED - {result['errors']} errors, {result['warnings']} warnings")
                        if result["internal_errors"] > 0:
                            print(f"   Counted internal errors: {result['internal_errors']}")
                        failed += 1
                        failed_files.append(filename)
                        error_lines = [line for line in result["output"].split('\n') if 'error' in line.lower()][:3]
                        for line in error_lines:
                            print(f"   {line}")

                    total_errors += result["errors"]
                    wct = result["warnings"]
                    total_warnings += wct
                    if wct > 0:
                        samples_with_warnings += 1
                    if wct > max_warnings:
                        max_warnings = wct
                    if not result.get("hex_present", True):
                        missing_hex_names.append(result.get("base_name", filename.replace(".jal", "")))
                        if result.get("passed"):
                            missing_hex_when_passed_names.append(
                                result.get("base_name", filename.replace(".jal", ""))
                            )

                print("")
    
        # Summary
        print("=== COMPREHENSIVE TEST SUMMARY ===")
        print(f"Total samples tested: {len(jal_files)}")
        print(f"Passed: {passed}")
        print(f"Failed: {failed}")
        success_rate = (passed / len(jal_files) * 100.0) if jal_files else 0.0
        print(f"Success rate: {success_rate:.1f}%")
        print(f"Total errors across all tests: {total_errors}")
        n_samples = len(jal_files)
        print(
            "Warnings (each .jal is a separate compile — not one combined build):"
        )
        print(f"  Max warnings in any single sample: {max_warnings}")
        print(
            f"  Samples with >0 warnings: {samples_with_warnings} / {n_samples}"
        )
        print(
            f"  Summed warning counts (Σ per-sample final summary): {total_warnings}"
        )
    
        if failed > 0:
            print(f"\n=== FAILED FILES ({failed} total) ===")
            for i, filename in enumerate(failed_files, 1):
                print(f"{i:2d}. {filename}")
    
        if failed == 0:
            print("🎉 ALL TESTS PASSED!")
        else:
            print(f"⚠️  {failed} tests failed")
    
        # Save comprehensive summary report
        summary_file = f"unit-tests/Comparison/reports/{output_type.lower()}_test_summary.txt"
        with open(summary_file, 'w') as f:
            f.write(f"{_report_timestamp_first_line()}\n\n")
            f.write(f"{output_type.upper()} COMPILER TEST SUMMARY\n")
            f.write("=" * 50 + "\n")
            f.write(f"Compiler: {compiler_path} ({output_type.upper()})\n")
            f.write(f"Parallel workers: {_sample_compile_worker_count(parallel_workers)}\n")
            f.write(f"Sample directory (ephemeral): {sample_dir}\n")
            f.write(f"Lib include path (ephemeral): {lib_path}\n")
            f.write("Shadow copy: " + _shadow_note + "\n")
            f.write(f"Canonical (repo) files: not modified (third_party/jallib/samples/, third_party/jallib/lib/, demo_lib/)\n")
            f.write("\n")
            f.write("RESULTS:\n")
            f.write(f"Total samples tested: {len(jal_files)}\n")
            f.write(f"Passed: {passed}\n")
            f.write(f"Failed: {failed}\n")
            f.write(f"Success rate: {success_rate:.1f}%\n")
            f.write(f"Total errors across all tests: {total_errors}\n")
            f.write(
                "Warnings (each sample is a separate compile; "
                "see max / counts below — summed line is not one build):\n"
            )
            f.write(f"  Max warnings in any single sample: {max_warnings}\n")
            f.write(
                f"  Samples with >0 warnings: {samples_with_warnings} / {n_samples}\n"
            )
            f.write(
                f"  Summed warning counts (Σ per-sample): {total_warnings}\n"
            )
            f.write("\n")
            f.write("OUTPUT LOCATIONS:\n")
            f.write(f"HEX files: unit-tests/Output_{output_type}/hex/\n")
            f.write(f"ASM files: unit-tests/Output_{output_type}/asm/\n")
            f.write(f"COD files: unit-tests/Output_{output_type}/cod/\n")
            f.write(f"Full output: unit-tests/Output_{output_type}/full_output/\n")
            f.write(f"Reports: unit-tests/Comparison/reports/\n")
        
            if failed > 0:
                f.write(f"\nFAILED FILES ({failed} total):\n")
                for i, filename in enumerate(failed_files, 1):
                    f.write(f"{i:2d}. {filename}\n")
            f.write("\n")
            f.write("MISSING HEX (third_party/jallib/samples/<name>.hex not present after compile):\n")
            f.write(f"Total with no .hex file: {len(missing_hex_names)}\n")
            f.write(
                f"Subset with 0 compiler errors (unexpected): {len(missing_hex_when_passed_names)}\n"
            )
    
        print(f"\n📊 Summary report saved to: {summary_file}")
        print(f"📁 All outputs saved to: unit-tests/Output_{output_type}/")

        if not use_baseline:
            compare_dir = "unit-tests/Output_Compare"
            os.makedirs(compare_dir, exist_ok=True)
            err_msg_path = os.path.join(compare_dir, "current_error_messages.txt")
            warn_msg_path = os.path.join(compare_dir, "current_warning_messages.txt")
            write_current_error_messages_report(
                f"unit-tests/Output_{output_type}/full_output",
                err_msg_path,
            )
            write_current_warning_messages_report(
                f"unit-tests/Output_{output_type}/full_output",
                warn_msg_path,
            )
            print(f"📋 Compiler error messages (for web dashboard): {err_msg_path}")
            print(f"📋 Compiler warning messages (for web dashboard): {warn_msg_path}")
            miss_path = os.path.join(compare_dir, "current_missing_hex.txt")
            with open(miss_path, "w", encoding="utf-8") as mf:
                mf.write(f"{_report_timestamp_first_line()}\n\n")
                mf.write("MISSING HEX AFTER COMPILE (current)\n")
                mf.write("--------------------------------------\n")
                mf.write(
                    "Checked: third_party/jallib/samples/<name>.hex exists after each compile.\n\n"
                )
                mf.write(f"Total .jal samples: {len(jal_files)}\n")
                mf.write(f"No .hex file present: {len(missing_hex_names)}\n")
                mf.write(
                    f"No .hex but compiler reported 0 errors: {len(missing_hex_when_passed_names)}\n\n"
                )
                mf.write("Samples with no .hex file (name):\n")
                for n in sorted(missing_hex_names):
                    mf.write(f"{n}\n")
                if not missing_hex_names:
                    mf.write("(none)\n")
                mf.write("\nSubset: 0 errors listed in summary but no .hex:\n")
                for n in sorted(missing_hex_when_passed_names):
                    mf.write(f"{n}\n")
                if not missing_hex_when_passed_names:
                    mf.write("(none)\n")
            print(f"📋 Missing HEX report (web dashboard): {miss_path}")

        report_snap = snapshot_test_report_trees(_repo, label=_lbl, quiet=False)
        return {
            "report_snapshot": report_snap,
            "passed": passed,
            "failed": failed,
            "samples": len(jal_files),
            "total_errors": total_errors,
            "total_warnings": total_warnings,
            "max_warnings": max_warnings,
            "samples_with_warnings": samples_with_warnings,
            "label": _lbl,
            "parallel_workers": _sample_compile_worker_count(parallel_workers),
            "missing_hex": len(missing_hex_names),
            "missing_hex_when_passed": len(missing_hex_when_passed_names),
            "compiler_path": compiler_path,
        }


    finally:
        if os.path.isdir(shadow_root):
            shutil.rmtree(shadow_root, ignore_errors=True)


if __name__ == "__main__":
    # Single-instance guard: a second concurrent run double-drives the same
    # output dirs and corrupts report archives (see _runlock.py).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import _lowprio
    import _runlock
    _lowprio.lower_own_priority()
    _runlock.acquire("unittests")

    # Parse command line arguments
    max_samples = None
    use_baseline = True  # Default to baseline compiler
    parallel_workers_cli = None

    # Strip --parallel N / -j N / --parallel=N so N is not parsed as max_samples
    _na = [sys.argv[0]]
    _i = 1
    while _i < len(sys.argv):
        _a = sys.argv[_i]
        if _a in ("--parallel", "-j") and _i + 1 < len(sys.argv):
            try:
                parallel_workers_cli = int(sys.argv[_i + 1])
            except ValueError:
                print("error: --parallel/-j requires an integer", file=sys.stderr)
                sys.exit(2)
            _i += 2
            continue
        if _a.startswith("--parallel="):
            try:
                parallel_workers_cli = int(_a.split("=", 1)[1])
            except ValueError:
                print("error: --parallel=N requires an integer", file=sys.stderr)
                sys.exit(2)
            _i += 1
            continue
        _na.append(_a)
        _i += 1
    sys.argv = _na

    # Handle help option early
    if len(sys.argv) > 1 and ("--help" in sys.argv[1:] or "-h" in sys.argv[1:]):
        _print_unittests_usage()
        sys.exit(0)

    if len(sys.argv) > 1 and "--gpsim-regression" in sys.argv[1:]:
        _root = os.path.join(_repo_root, "sims")
        _suite = os.path.join(_root, "run_regression_suite.py")
        if not os.path.isfile(_suite):
            print(f"error: missing {_suite}", file=sys.stderr)
            sys.exit(2)
        _extra = []
        if "--baseline" in sys.argv[1:] or "-b" in sys.argv[1:]:
            _extra.append("--baseline")
        if "--quiet" in sys.argv[1:] or "-q" in sys.argv[1:]:
            _extra.append("--quiet")
        if "--fail-fast" in sys.argv[1:]:
            _extra.append("--fail-fast")
        if "--suite-all" in sys.argv[1:]:
            _extra.append("--suite-all")
        _av = sys.argv[1:]
        for _i, _a in enumerate(_av):
            if _a == "--suite" and _i + 1 < len(_av):
                _extra.extend(["--suite", _av[_i + 1]])
                break
        _cmd = [sys.executable, _suite]
        if _extra:
            _cmd.extend(_extra)
        else:
            _cmd.append("--run")
        print("Running gpsim regression (output captured for web dashboard)…")
        _gpsim_env = os.environ.copy()
        _gpsim_env["JALV2_GPSIM_SUBPROCESS"] = "1"
        _completed = subprocess.run(
            _cmd,
            cwd=_root,
            env=_gpsim_env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        _summary_path = os.path.join(_root, _GPSIM_SUMMARY_FILE)
        os.makedirs(os.path.dirname(_summary_path), exist_ok=True)
        _stats_path = os.path.join(_root, _GPSIM_RUN_STATS_JSON)
        _meta = None
        if os.path.isfile(_stats_path):
            try:
                with open(_stats_path, encoding="utf-8") as _mf:
                    _meta = json.load(_mf)
            except (OSError, json.JSONDecodeError):
                _meta = None
        with open(_summary_path, "w", encoding="utf-8") as _sf:
            _sf.write(f"{_report_timestamp_first_line()}\n\n")
            if _meta is not None:
                _cfs = _meta.get("checkpoint_failed_suite")
                if _cfs is not None:
                    _sf.write(
                        f"Results: checkpoint verification failed for suite {_cfs!r} "
                        f"before any .jal simulation (0 files tested).\n\n"
                    )
                else:
                    _sf.write(
                        f"Results: {_meta['passed']} passed, {_meta['failed']} failed "
                        f"({_meta['tested']} .jal files tested).\n\n"
                    )
            _sf.write("GPSIM REGRESSION SUITE\n")
            _sf.write("=" * 50 + "\n")
            _sf.write(f"Command: {' '.join(shlex.quote(str(a)) for a in _cmd)}\n")
            _sf.write(f"Exit code: {_completed.returncode}\n")
            _sf.write(f"Summary path (for publish): {_GPSIM_SUMMARY_FILE}\n")
            _sf.write("\n--- combined log (stdout + stderr) ---\n\n")
            _sf.write(_completed.stdout or "")
            if _completed.stderr:
                _sf.write(_completed.stderr)
        print(f"Gpsim log written to {_GPSIM_SUMMARY_FILE}")
        _gpsim_snap = snapshot_test_report_trees(_root, label="gpsim_regression", quiet=False)
        _gpsim_web = None
        try:
            from publish_unit_test_web import publish_gpsim_regression_summary_to_web_root

            _gpsim_web = publish_gpsim_regression_summary_to_web_root(_root, quiet=False)
        except Exception as e:
            print(f"Warning: gpsim web copy failed: {e}", file=sys.stderr)
            _gpsim_web = None
        _flags = " ".join(shlex.quote(x) for x in _extra) if _extra else "--run"
        _sum_extra = ""
        if _meta is not None and _meta.get("checkpoint_failed_suite") is None:
            _sum_extra = (
                f"; {_meta['passed']} passed, {_meta['failed']} failed / {_meta['tested']} tested"
            )
        _gpsim_changelog_partial = os.environ.get(
            "JALV2_GPSIM_CHANGELOG_PARTIAL", ""
        ).strip().lower() in ("1", "true", "yes", "on")
        _gpsim_changelog_ok = (
            _meta is None
            or _meta.get("changelog_eligible", True)
            or _gpsim_changelog_partial
        )
        if _gpsim_changelog_ok:
            append_changelog_test_automation(
                _root,
                summary=(
                    f"Tests (`unittests.py` `--gpsim-regression` {_flags}): exit {_completed.returncode}{_sum_extra}"
                ),
                report_snapshot=_gpsim_snap,
                web_snapshot=_gpsim_web,
                quiet=False,
            )
        else:
            print(
                "unittests: CHANGELOG.txt unchanged (gpsim run was not the full ordered suite list). "
                "Override: JALV2_GPSIM_CHANGELOG_PARTIAL=1.",
            )
        sys.exit(_completed.returncode)

    if len(sys.argv) > 1 and (
        "--pic32" in sys.argv[1:] or "--pic32-regression" in sys.argv[1:]
    ):
        _root = _tests_dir
        _pic32_args = sys.argv[1:]
        _pic32_compiler = _cli_compiler_path(_pic32_args)
        _pic32_limit = _cli_sample_limit(_pic32_args)
        _want_compile = "--pic32" in _pic32_args
        _want_qemu = "--pic32-regression" in _pic32_args
        _exit = 0

        if _want_compile:
            if not _pic32_compiler:
                print(
                    "error: --pic32 requires --compiler PATH (e.g. ./bin/jalv3-x86-64)",
                    file=sys.stderr,
                )
                sys.exit(2)
            if not os.path.isfile(_pic32_compiler):
                print(
                    f"error: compiler not found or not a file: {_pic32_compiler}",
                    file=sys.stderr,
                )
                sys.exit(2)
            _stats = test_pic32_compile(
                _pic32_compiler,
                parallel_workers=parallel_workers_cli,
                max_samples=_pic32_limit,
            )
            append_changelog_test_automation(
                _root,
                summary=(
                    f"Tests (`unittests.py` `--pic32`): {_stats['passed']}/{_stats['samples']} "
                    f"compiled, failed {_stats['failed']}"
                ),
                report_snapshot=_stats.get("report_snapshot"),
                web_snapshot=None,
                quiet=False,
            )
            if _stats.get("failed", 0) > 0:
                _exit = 1
                if not _want_qemu:
                    sys.exit(_exit)

        if _want_qemu:
            _suite = os.path.join(_root, "sims", "run_pic32_regression.py")
            if not os.path.isfile(_suite):
                print(f"error: missing {_suite}", file=sys.stderr)
                sys.exit(2)
            _cmd = [sys.executable, _suite, "--run"]
            if _pic32_compiler:
                if not os.path.isfile(_pic32_compiler):
                    print(
                        f"error: compiler not found or not a file: {_pic32_compiler}",
                        file=sys.stderr,
                    )
                    sys.exit(2)
                _cmd.extend(["--compiler", _pic32_compiler])
            if "--quiet" in _pic32_args or "-q" in _pic32_args:
                _cmd.append("--quiet")
            if "--fail-fast" in _pic32_args:
                _cmd.append("--fail-fast")
            if parallel_workers_cli is not None:
                _cmd.extend(["--jobs", str(parallel_workers_cli)])
            print("Running PIC32 QEMU regression (output captured)…")
            _completed = subprocess.run(
                _cmd,
                cwd=_root,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            _summary_path = os.path.join(_root, _PIC32_REGRESSION_SUMMARY)
            os.makedirs(os.path.dirname(_summary_path), exist_ok=True)
            with open(_summary_path, "w", encoding="utf-8") as _sf:
                _sf.write(f"{_report_timestamp_first_line()}\n\n")
                _sf.write("PIC32 QEMU REGRESSION\n")
                _sf.write("=" * 50 + "\n")
                _sf.write(f"Command: {' '.join(shlex.quote(str(a)) for a in _cmd)}\n")
                _sf.write(f"Exit code: {_completed.returncode}\n")
                _sf.write("\n--- combined log (stdout + stderr) ---\n\n")
                _sf.write(_completed.stdout or "")
                if _completed.stderr:
                    _sf.write(_completed.stderr)
            print(f"PIC32 QEMU log written to {_PIC32_REGRESSION_SUMMARY}")
            if _completed.stdout:
                for _line in _completed.stdout.strip().splitlines()[-8:]:
                    print(_line)
            _qemu_snap = snapshot_test_report_trees(
                _root, label="pic32_regression", quiet=False
            )
            append_changelog_test_automation(
                _root,
                summary=(
                    f"Tests (`unittests.py` `--pic32-regression`): exit {_completed.returncode}"
                ),
                report_snapshot=_qemu_snap,
                web_snapshot=None,
                quiet=False,
            )
            if _completed.returncode != 0:
                _exit = _completed.returncode
        sys.exit(_exit)

    # Handle --compare option (compute counts for both directories and exit)
    if len(sys.argv) > 1 and ("--compare" in sys.argv[1:]):
        _repo = _repo_root
        clear_output_compare_only(
            _repo,
            quiet=False,
            preserve_files=("current_missing_hex.txt",),
        )

        import re

        def compute_counts(root_dir: str):
            full_out = os.path.join(root_dir, "full_output")
            files_with_error = 0
            total_errors = 0
            total_warnings = 0
            files_without_summary = 0
            files_with_errors = 0  # Files with >0 errors
            files_with_warnings = 0  # Files with >0 warnings
            if not os.path.isdir(full_out):
                return files_with_error, total_errors, total_warnings, files_without_summary, files_with_errors, files_with_warnings
            for name in os.listdir(full_out):
                if not name.endswith("_output.txt"):
                    continue
                path = os.path.join(full_out, name)
                try:
                    with open(path, "r", encoding="utf-8", errors="ignore") as f:
                        text = f.read()
                    if "ERROR:" in text or "Error:" in text:
                        files_with_error += 1
                    e, w, _internal = parse_error_warning_counts(text)
                    total_errors += e
                    total_warnings += w
                    if e > 0:
                        files_with_errors += 1
                    if w > 0:
                        files_with_warnings += 1
                    if not re.search(r"(\d+) errors?,?\s*(\d+) warnings?", text):
                        files_without_summary += 1
                except Exception:
                    # Ignore unreadable files
                    files_without_summary += 1
            return files_with_error, total_errors, total_warnings, files_without_summary, files_with_errors, files_with_warnings

        # Prepare paths
        base_dir = os.path.join("unit-tests", "Output_Baseline")
        curr_dir = os.path.join("unit-tests", "Output_Current")
        compare_dir = os.path.join("unit-tests", "Output_Compare")
        samples_dir = os.path.join("unit-tests", "Sample")
        os.makedirs(compare_dir, exist_ok=True)
        compare_report_ts = _report_timestamp_first_line()

        # Compute error/warning counts
        b_err_files, b_err_sum, b_warn_sum, b_no_summary, b_files_with_errors, b_files_with_warnings = compute_counts(base_dir)
        c_err_files, c_err_sum, c_warn_sum, c_no_summary, c_files_with_errors, c_files_with_warnings = compute_counts(curr_dir)

        # Compare HEX sizes first (needed for summary)
        base_hex = os.path.join(base_dir, "hex")
        curr_hex = os.path.join(curr_dir, "hex")
        names = set()
        if os.path.isdir(base_hex):
            names.update([os.path.splitext(n)[0] for n in os.listdir(base_hex) if n.lower().endswith(".hex")])
        if os.path.isdir(curr_hex):
            names.update([os.path.splitext(n)[0] for n in os.listdir(curr_hex) if n.lower().endswith(".hex")])

        equal_sizes = 0
        different_sizes = 0
        missing_in_baseline = 0
        missing_in_current = 0
        hex_sizes = {}  # Store hex sizes for later use

        samples_diff_hex_dir = os.path.join(compare_dir, "samples_with_diff_hex")
        os.makedirs(samples_diff_hex_dir, exist_ok=True)

        hex_report_path = os.path.join(compare_dir, "hex_size_comparison.txt")
        with open(hex_report_path, "w", encoding="utf-8") as hf:
            hf.write(f"{compare_report_ts}\n\n")
            hf.write("HEX SIZE COMPARISON\n")
            hf.write("--------------------\n")
            hf.write("name, baseline_size, current_size, status\n")
            for name in sorted(names):
                b_path = os.path.join(base_hex, name + ".hex")
                c_path = os.path.join(curr_hex, name + ".hex")
                b_size = os.path.getsize(b_path) if os.path.isfile(b_path) else None
                c_size = os.path.getsize(c_path) if os.path.isfile(c_path) else None
                
                # Store sizes in dictionary for later use
                if b_size is not None and c_size is not None:
                    hex_sizes[name] = (b_size, c_size)
                
                if b_size is None and c_size is None:
                    status = "missing-both"
                elif b_size is None:
                    status = "missing-baseline"
                    missing_in_baseline += 1
                elif c_size is None:
                    status = "missing-current"
                    missing_in_current += 1
                else:
                    if b_size == c_size:
                        status = "equal"
                        equal_sizes += 1
                    else:
                        status = "different"
                        different_sizes += 1
                        # copy the .jal sample into samples_with_diff_hex
                        sample_src = os.path.join(samples_dir, name + ".jal")
                        if os.path.isfile(sample_src):
                            shutil.copy2(sample_src, os.path.join(samples_diff_hex_dir, name + ".jal"))
                hf.write(f"{name}, {b_size if b_size is not None else '-'}, {c_size if c_size is not None else '-'}, {status}\n")

        total_compared = equal_sizes + different_sizes + missing_in_baseline + missing_in_current
        eq_pct = (equal_sizes / total_compared * 100.0) if total_compared else 0.0
        diff_pct = (different_sizes / total_compared * 100.0) if total_compared else 0.0

        # Write hex comparison summary
        hex_summary_path = os.path.join(compare_dir, "hex_size_summary.txt")
        with open(hex_summary_path, "w", encoding="utf-8") as hs:
            hs.write(f"{compare_report_ts}\n\n")
            hs.write("HEX SIZE SUMMARY\n")
            hs.write("----------------\n")
            hs.write(f"Equal sizes: {equal_sizes}\n")
            hs.write(f"Different sizes: {different_sizes}\n")
            hs.write(f"Missing in baseline: {missing_in_baseline}\n")
            hs.write(f"Missing in current: {missing_in_current}\n")
            hs.write(f"Totals compared: {total_compared}\n")
            hs.write(f"Equal %: {eq_pct:.1f}%\n")
            hs.write(f"Different %: {diff_pct:.1f}%\n")

        # Data (RAM) usage comparison: "Data area: N of M used" from the
        # saved full compiler outputs of both runs
        def _data_area_used(full_dir, name):
            path = os.path.join(full_dir, name + "_output.txt")
            used = None
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as f:
                    for line in f:
                        m = re.search(r"Data area:\s*(\d+) of \d+ used", line)
                        if m:
                            used = int(m.group(1))
            except OSError:
                pass
            return used

        base_full_dir = os.path.join(base_dir, "full_output")
        curr_full_dir = os.path.join(curr_dir, "full_output")
        data_equal = data_different = 0
        data_missing_baseline = data_missing_current = data_no_data = 0
        data_report_path = os.path.join(compare_dir, "data_size_comparison.txt")
        with open(data_report_path, "w", encoding="utf-8") as df:
            df.write(f"{compare_report_ts}\n\n")
            df.write("DATA (RAM) USAGE COMPARISON\n")
            df.write("-----------------------------\n")
            df.write("name, baseline_data, current_data, status\n")
            for name in sorted(names):
                b_used = _data_area_used(base_full_dir, name) if os.path.isdir(base_full_dir) else None
                c_used = _data_area_used(curr_full_dir, name) if os.path.isdir(curr_full_dir) else None
                if b_used is None and c_used is None:
                    status = "no-data"
                    data_no_data += 1
                elif b_used is None:
                    status = "missing-baseline"
                    data_missing_baseline += 1
                elif c_used is None:
                    status = "missing-current"
                    data_missing_current += 1
                elif b_used == c_used:
                    status = "equal"
                    data_equal += 1
                else:
                    status = "different"
                    data_different += 1
                df.write(
                    f"{name}, {b_used if b_used is not None else '-'}, "
                    f"{c_used if c_used is not None else '-'}, {status}\n"
                )

        # Missing HEX output copies vs every Sample/*.jal (unit test copies to Output_*/hex/)
        all_jal_bases = []
        if os.path.isdir(samples_dir):
            all_jal_bases = sorted(
                os.path.splitext(f)[0]
                for f in os.listdir(samples_dir)
                if f.endswith(".jal")
            )
        missing_hex_out_bases_b = []
        missing_hex_out_bases_c = []
        for bn in all_jal_bases:
            bp = os.path.join(base_hex, bn + ".hex")
            cp = os.path.join(curr_hex, bn + ".hex")
            if not os.path.isfile(bp):
                missing_hex_out_bases_b.append(bn)
            if not os.path.isfile(cp):
                missing_hex_out_bases_c.append(bn)
        missing_hex_compare_report = os.path.join(compare_dir, "compare_missing_hex.txt")
        with open(missing_hex_compare_report, "w", encoding="utf-8") as mf:
            mf.write(f"{compare_report_ts}\n\n")
            mf.write("MISSING HEX OUTPUT COPIES\n")
            mf.write("-------------------------\n")
            mf.write(
                "Per sample, expected: unit-tests/Output_<run>/hex/<name>.hex "
                "(copied from third_party/jallib/samples/ after compile).\n\n"
            )
            mf.write(f"Total .jal samples: {len(all_jal_bases)}\n")
            mf.write(f"Missing in baseline output copy: {len(missing_hex_out_bases_b)}\n")
            mf.write(f"Missing in current output copy: {len(missing_hex_out_bases_c)}\n\n")
            mf.write("Missing baseline (names):\n")
            for n in missing_hex_out_bases_b:
                mf.write(f"{n}\n")
            if not missing_hex_out_bases_b:
                mf.write("(none)\n")
            mf.write("\nMissing current (names):\n")
            for n in missing_hex_out_bases_c:
                mf.write(f"{n}\n")
            if not missing_hex_out_bases_c:
                mf.write("(none)\n")

        # Get samples with warnings for filtering
        warning_samples = set()
        curr_full = os.path.join(curr_dir, "full_output")
        if os.path.isdir(curr_full):
            for name in os.listdir(curr_full):
                if not name.endswith("_output.txt"):
                    continue
                path = os.path.join(curr_full, name)
                try:
                    with open(path, "r", encoding="utf-8", errors="ignore") as f:
                        text = f.read()
                    base = name.replace("_output.txt", "")
                    m = re.search(r"(\d+) errors?,?\s*(\d+) warnings?", text)
                    if m:
                        w = int(m.group(2))
                        if w > 0:
                            warning_samples.add(base)
                    else:
                        # Fallback to simple counting if pattern not found
                        w = text.count(" warning")
                        if w > 0:
                            warning_samples.add(base)
                except Exception:
                    pass

        # Different hex sizes, no warnings, and current side non-empty (size 0 = failed/empty output)
        different_clean_samples = []
        for name in names:
            if name in hex_sizes:
                base_size, curr_size = hex_sizes[name]
                if base_size != curr_size:  # Different sizes
                    has_warnings = name in warning_samples
                    has_empty_curr_hex = curr_size == 0
                    if not has_warnings and not has_empty_curr_hex:
                        different_clean_samples.append((name, base_size, curr_size))

        # Write comprehensive compare summary with hex stats and missing-hex counts
        summary_path = os.path.join(compare_dir, "compare_summary.txt")
        with open(summary_path, "w", encoding="utf-8") as sf:
            sf.write(f"{compare_report_ts}\n\n")
            sf.write("COMPARE SUMMARY\n")
            sf.write("---------------\n")
            sf.write(f"Baseline: files containing 'ERROR:': {b_err_files}\n")
            sf.write(f"Baseline: summed errors: {b_err_sum}\n")
            sf.write(f"Baseline: summed warnings: {b_warn_sum}\n")
            sf.write(f"Baseline: unique files with errors: {b_files_with_errors}\n")
            sf.write(f"Baseline: unique files with warnings: {b_files_with_warnings}\n")
            sf.write(f"Current:  files containing 'ERROR:': {c_err_files}\n")
            sf.write(f"Current:  summed errors: {c_err_sum}\n")
            sf.write(f"Current:  summed warnings: {c_warn_sum}\n")
            sf.write(f"Current:  unique files with errors: {c_files_with_errors}\n")
            sf.write(f"Current:  unique files with warnings: {c_files_with_warnings}\n")
            sf.write(f"\nHEX SIZE COMPARISON\n")
            sf.write(f"Equal sizes: {equal_sizes} ({eq_pct:.1f}%)\n")
            sf.write(f"Different sizes: {different_sizes} ({diff_pct:.1f}%)\n")
            sf.write(f"Missing in baseline: {missing_in_baseline}\n")
            sf.write(f"Missing in current: {missing_in_current}\n")
            sf.write(f"Total samples compared: {total_compared}\n")
            sf.write(f"\nDATA (RAM) USAGE COMPARISON\n")
            sf.write(f"Equal usage: {data_equal}\n")
            sf.write(f"Different usage: {data_different}\n")
            sf.write(f"Missing in baseline: {data_missing_baseline}\n")
            sf.write(f"Missing in current: {data_missing_current}\n")
            sf.write(f"No data-area line: {data_no_data}\n")
            sf.write(f"\nMISSING HEX OUTPUT COPIES (all Sample/*.jal)\n")
            sf.write(f"Missing in baseline output dir: {len(missing_hex_out_bases_b)}\n")
            sf.write(f"Missing in current output dir: {len(missing_hex_out_bases_c)}\n")
            sf.write(f"\nFILES WITHOUT ERRORS & WARNINGS SUMMARY\n")
            sf.write(f"Baseline: {b_no_summary} files without summary line\n")
            sf.write(f"Current:  {c_no_summary} files without summary line\n")
            
            # Add clean different samples section
            sf.write(f"\nCLEAN DIFFERENT SIZES (NO WARNINGS, CURRENT HEX NON-EMPTY)\n")
            sf.write(f"Total: {len(different_clean_samples)} samples\n")
            sf.write(f"{'Sample Name':<45} {'Baseline':>8} {'Current':>8} {'Difference':>10}\n")
            sf.write(f"{'-' * 80}\n")
            for name, base_size, curr_size in sorted(different_clean_samples):
                diff = curr_size - base_size
                diff_str = f"{diff:+d}" if diff != 0 else f"{diff:d}"
                sf.write(f"{name:<45} {base_size:>8} {curr_size:>8} {diff_str:>10}\n")

        # Create directory for clean different samples and copy files
        if different_clean_samples:
            clean_diff_dir = os.path.join(compare_dir, "samples_clean_different_sizes")
            os.makedirs(clean_diff_dir, exist_ok=True)
            
            for name, base_size, curr_size in different_clean_samples:
                # Copy .jal sample file
                sample_src = os.path.join(samples_dir, name + ".jal")
                if os.path.isfile(sample_src):
                    shutil.copy2(sample_src, os.path.join(clean_diff_dir, name + ".jal"))
                
                # Copy current compiler output
                curr_output_src = os.path.join(curr_dir, "full_output", name + "_output.txt")
                if os.path.isfile(curr_output_src):
                    shutil.copy2(curr_output_src, os.path.join(clean_diff_dir, name + "_output.txt"))
                
                # Copy baseline compiler output
                base_output_src = os.path.join(base_dir, "full_output", name + "_output.txt")
                if os.path.isfile(base_output_src):
                    shutil.copy2(base_output_src, os.path.join(clean_diff_dir, name + "_baseline_output.txt"))
                
                # Copy current hex file
                curr_hex_src = os.path.join(curr_dir, "hex", name + ".hex")
                if os.path.isfile(curr_hex_src):
                    shutil.copy2(curr_hex_src, os.path.join(clean_diff_dir, name + "_current.hex"))
                
                # Copy baseline hex file
                base_hex_src = os.path.join(base_dir, "hex", name + ".hex")
                if os.path.isfile(base_hex_src):
                    shutil.copy2(base_hex_src, os.path.join(clean_diff_dir, name + "_baseline.hex"))
                
                # Copy current .asm file
                curr_asm_src = os.path.join(curr_dir, "asm", name + ".asm")
                if os.path.isfile(curr_asm_src):
                    shutil.copy2(curr_asm_src, os.path.join(clean_diff_dir, name + "_current.asm"))
                
                # Copy baseline .asm file
                base_asm_src = os.path.join(base_dir, "asm", name + ".asm")
                if os.path.isfile(base_asm_src):
                    shutil.copy2(base_asm_src, os.path.join(clean_diff_dir, name + "_baseline.asm"))
                
                # Copy current .cod file
                curr_cod_src = os.path.join(curr_dir, "cod", name + ".cod")
                if os.path.isfile(curr_cod_src):
                    shutil.copy2(curr_cod_src, os.path.join(clean_diff_dir, name + "_current.cod"))
                
                # Copy baseline .cod file
                base_cod_src = os.path.join(base_dir, "cod", name + ".cod")
                if os.path.isfile(base_cod_src):
                    shutil.copy2(base_cod_src, os.path.join(clean_diff_dir, name + "_baseline.cod"))

        # Additional current-only reports and sample collection
        curr_full = os.path.join(curr_dir, "full_output")
        error_tag_report = os.path.join(compare_dir, "current_contains_error_tag.txt")
        warnings_report = os.path.join(compare_dir, "current_with_warnings.txt")
        errors_report = os.path.join(compare_dir, "current_with_errors.txt")

        samples_err_tag_dir = os.path.join(compare_dir, "samples_contains_error_tag")
        samples_warn_dir = os.path.join(compare_dir, "samples_with_warnings")
        samples_errs_dir = os.path.join(compare_dir, "samples_with_errors")
        os.makedirs(samples_err_tag_dir, exist_ok=True)
        os.makedirs(samples_warn_dir, exist_ok=True)
        os.makedirs(samples_errs_dir, exist_ok=True)

        with open(error_tag_report, "w", encoding="utf-8") as erpt, \
             open(warnings_report, "w", encoding="utf-8") as wrpt, \
             open(errors_report, "w", encoding="utf-8") as rrpt:
            erpt.write(f"{compare_report_ts}\n\n")
            erpt.write("FILES MATCHING ERROR DIAGNOSTICS (Error:, ERROR:, or *.jal:line: error:)\n")
            erpt.write("---------------------------------------------------------------------\n")
            wrpt.write(f"{compare_report_ts}\n\n")
            wrpt.write("FILES WITH WARNINGS (>0)\n")
            wrpt.write("------------------------\n")
            wrpt.write("name, warnings\n")
            rrpt.write(f"{compare_report_ts}\n\n")
            rrpt.write("FILES WITH ERRORS (>0)\n")
            rrpt.write("----------------------\n")
            rrpt.write("name, errors\n")

            error_tag_hits = 0
            if os.path.isdir(curr_full):
                for name in sorted(fn for fn in os.listdir(curr_full) if fn.endswith("_output.txt")):
                    path = os.path.join(curr_full, name)
                    try:
                        with open(path, "r", encoding="utf-8", errors="ignore") as f:
                            text = f.read()
                        base = name.replace("_output.txt", "")
                        sample_src = os.path.join(samples_dir, base + ".jal")
                        # destination log filename
                        dest_log_name = base + "_output.txt"
                        if _compiler_output_contains_error_tag(text):
                            error_tag_hits += 1
                            erpt.write(f"{base}\n")
                            if os.path.isfile(sample_src):
                                shutil.copy2(sample_src, os.path.join(samples_err_tag_dir, base + ".jal"))
                            # copy full output log
                            shutil.copy2(path, os.path.join(samples_err_tag_dir, dest_log_name))
                        e, w, _internal = parse_error_warning_counts(text)
                        
                        if w > 0:
                            wrpt.write(f"{base}, {w}\n")
                            if os.path.isfile(sample_src):
                                shutil.copy2(sample_src, os.path.join(samples_warn_dir, base + ".jal"))
                            shutil.copy2(path, os.path.join(samples_warn_dir, dest_log_name))
                        if e > 0:
                            rrpt.write(f"{base}, {e}\n")
                            if os.path.isfile(sample_src):
                                shutil.copy2(sample_src, os.path.join(samples_errs_dir, base + ".jal"))
                            shutil.copy2(path, os.path.join(samples_errs_dir, dest_log_name))
                    except Exception:
                        pass

            if error_tag_hits == 0:
                erpt.write(
                    "(none — no compiler output matched error diagnostic patterns)\n"
                )

        write_current_error_messages_report(
            curr_full,
            os.path.join(compare_dir, "current_error_messages.txt"),
        )
        write_current_warning_messages_report(
            curr_full,
            os.path.join(compare_dir, "current_warning_messages.txt"),
        )

        print(f"Compare reports written to: {compare_dir}")
        print(f" - {os.path.basename(summary_path)}")
        print(f" - {os.path.basename(hex_report_path)}")
        print(f" - {os.path.basename(hex_summary_path)}")
        print(" - compare_missing_hex.txt")
        print(" - current_error_messages.txt")
        print(" - current_warning_messages.txt")
        if different_clean_samples:
            print(
                " - samples_clean_different_sizes/ "
                "(different sizes, no warnings, current HEX non-empty)"
            )
        try:
            from validate_hex_static import run_static_validation

            _sv_workers = _sample_compile_worker_count(parallel_workers_cli)
            print(f"Static validation: up to {_sv_workers} worker threads")
            run_static_validation(
                base_dir, curr_dir, compare_dir, quiet=False, max_workers=_sv_workers
            )
        except Exception as e:
            print(f"Warning: static validation (validate_hex_static) failed: {e}")
        _cmp_snap = snapshot_test_report_trees(_repo, label="compare", quiet=False)
        _cmp_web = publish_unit_test_dashboard()
        append_changelog_test_automation(
            _repo,
            summary=(
                f"Tests (`unittests.py` `--compare`): HEX pairs {total_compared}, equal {equal_sizes}, "
                f"different {different_sizes}, missing current HEX output {len(missing_hex_out_bases_c)}, "
                f"current summed warnings {c_warn_sum}"
            ),
            report_snapshot=_cmp_snap,
            web_snapshot=_cmp_web,
            quiet=False,
        )
        sys.exit(0)

    # Handle --clean option
    if len(sys.argv) > 1 and ("--clean" in sys.argv[1:]):
        clean_old_reports()
        sys.exit(0)

    current_compiler_path = None
    saw_current = False
    if len(sys.argv) > 1:
        _opts = sys.argv[1:]
        _i = 0
        while _i < len(_opts):
            arg = _opts[_i]
            if arg in ("--current", "-c"):
                saw_current = True
                use_baseline = False
                _i += 1
            elif arg in ("--baseline", "-b"):
                use_baseline = True
                print("Using BASELINE compiler")
                _i += 1
            elif arg == "--compiler" and _i + 1 < len(_opts):
                current_compiler_path = _opts[_i + 1]
                _i += 2
            elif arg.startswith("--compiler="):
                current_compiler_path = arg.split("=", 1)[1]
                _i += 1
            elif arg == "--compare":
                _i += 1
            else:
                try:
                    max_samples = int(arg)
                    print(f"Limiting tests to first {max_samples} samples")
                except ValueError:
                    print(f"Unknown argument: {arg}")
                _i += 1

    if saw_current:
        if not current_compiler_path:
            print(
                "error: --current requires --compiler PATH (e.g. ./bin/jalv3-x86-64)",
                file=sys.stderr,
            )
            sys.exit(2)
        if not os.path.isfile(current_compiler_path):
            print(
                f"error: compiler not found or not a file: {current_compiler_path}",
                file=sys.stderr,
            )
            sys.exit(2)
        print(f"Using CURRENT compiler: {current_compiler_path}")
    elif current_compiler_path:
        print(
            "error: --compiler is only valid with --current",
            file=sys.stderr,
        )
        sys.exit(2)

    if len(sys.argv) == 1:
        _print_unittests_usage()
        sys.exit(0)

    selftest_warning_extraction_stack_overflow_extended()

    _repo = _repo_root
    _stats = test_all_samples(
        max_samples,
        use_baseline,
        parallel_workers_cli,
        current_compiler_path=current_compiler_path if saw_current else None,
    )
    _web = publish_unit_test_dashboard()
    _mode = "--baseline" if _stats["label"] == "unit_baseline" else "--current"
    _cmp = _stats.get("compiler_path", "")
    _mode_detail = f"`{_mode}`" if _mode == "--baseline" else f"`{_mode}` (`{_cmp}`)"
    append_changelog_test_automation(
        _repo,
        summary=(
            f"Tests (`unittests.py` {_mode_detail}): {_stats['samples']} samples, passed {_stats['passed']}, "
            f"failed {_stats['failed']}, summed errors {_stats['total_errors']}, "
            f"warnings max {_stats.get('max_warnings', _stats['total_warnings'])} "
            f"(Σ {_stats['total_warnings']}), parallel {_stats['parallel_workers']}, "
            f"missing_hex {_stats.get('missing_hex', 0)}"
        ),
        report_snapshot=_stats["report_snapshot"],
        web_snapshot=_web,
        quiet=False,
    )
    sys.exit(1 if _stats.get("failed", 0) > 0 else 0)

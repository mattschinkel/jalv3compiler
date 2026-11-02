#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""Self-tests for the test harness itself (no compiler needed).

WHY THIS EXISTS
    The recurring failure mode in `fixes/fix_unittests_*.md` and
    `fixes/fix_unit_*.md` is not a compiler bug: it is the harness reporting a
    green result while measuring the wrong thing.  Those fixes have no
    regression test because a broken harness is invisible to the gates it
    runs -- the gate still says PASS.

    This pin exercises the pure decision points directly, and runs
    `unittests.py --compare` in a throw-away mini-repo to prove the
    missing-baseline hole stays closed:

      * crashed compile with no `N errors, M warnings` line is a FAILURE
        (fix_unittests_crash_fails_asm_normalize)
      * a summary plus a signal/assert signature is still a FAILURE
      * an un-runnable compiler is a setup error, not N sample failures
        (fix_unit_spawn_error_misclassified)
      * listing-only asm noise normalizes away, real operand differences do not
        (fix_unittests_crash_fails_asm_normalize)
      * `--compare` with no baseline exits 2 instead of "comparing nothing"
        (fix_compare_missing_baseline_fails)
      * `--compare` with a usable baseline still exits 0 (no false alarm)

    Usage: python3 unit-tests/test_harness_contract.py
"""
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import unittests  # noqa: E402
import validate_hex_static  # noqa: E402

# Modules unittests.py imports at run time; copied into the mini-repo so the
# test exercises the real entry point (not a re-implementation).
MINI_MODULES = [
    "unittests.py",
    "archive_test_reports.py",
    "_lowprio.py",
    "_runlock.py",
    "validate_hex_static.py",
]

failures = []
checks = 0


def check(name, fn):
    global checks
    checks += 1
    try:
        fn()
    except Exception:
        failures.append((name, traceback.format_exc()))
        print("FAIL %s" % name)
    else:
        print("ok   %s" % name)


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


# --------------------------------------------------------------- diagnostics
def t_summary_counts():
    got = unittests.parse_error_warning_counts(
        "jal 3.0a1-beta\ngenerating p-code\n0 errors, 0 warnings\n")
    expect(got == (0, 0, 0), "clean summary parsed as %r" % (got,))


def t_error_counts():
    got = unittests.parse_error_warning_counts("2 errors, 3 warnings\n")
    expect(got == (2, 3, 0), "2/3 summary parsed as %r" % (got,))


def t_crash_without_summary_is_failure():
    e, _w, internal = unittests.parse_error_warning_counts(
        "jal: generating p-code\nAssertion `val1 != VALUE_NONE' failed\n")
    expect(internal >= 1, "no-summary abort was not flagged internal")
    expect(e >= 1, "no-summary abort counted %d errors" % e)


def t_signal_after_summary_is_failure():
    e, _w, internal = unittests.parse_error_warning_counts(
        "0 errors, 0 warnings\nSegmentation fault\n")
    expect(internal >= 1, "signal after summary was not flagged internal")
    expect(e >= 1, "signal after summary counted %d errors" % e)


def t_abort_signature_is_failure():
    e, _w, internal = unittests.parse_error_warning_counts(
        "0 errors, 0 warnings\nAborted (core dumped)\n")
    expect(internal >= 1, "abort after summary was not flagged internal")
    expect(e >= 1, "abort after summary counted %d errors" % e)


# ------------------------------------------------------------ spawn contract
def _expect_exit2(path):
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            unittests._require_runnable_compiler(path)
    except SystemExit as exc:
        expect(exc.code == 2, "SystemExit code %r, expected 2" % (exc.code,))
    else:
        raise AssertionError("accepted un-runnable compiler %s" % path)


def t_missing_compiler_is_setup_error():
    _expect_exit2(os.path.join(HERE, "does-not-exist-binary"))


def t_non_executable_compiler_is_setup_error():
    fd, path = tempfile.mkstemp(prefix="jal_nonexec_")
    os.close(fd)
    try:
        os.chmod(path, 0o644)
        _expect_exit2(path)
    finally:
        os.unlink(path)


def t_executable_compiler_resolves():
    got = unittests._require_runnable_compiler("/bin/sh")
    expect(os.path.isabs(got), "compiler path not absolute: %r" % (got,))


def t_fuzzer_spawn_error_is_setup_error():
    fd, path = tempfile.mkstemp(prefix="jal_fuzz_nonexec_")
    os.close(fd)
    outdir = tempfile.mkdtemp(prefix="jal_fuzz_out_")
    try:
        os.chmod(path, 0o644)
        proc = subprocess.run(
            [sys.executable, os.path.join(HERE, "fuzz_jal_inputs.py"),
             "--compiler", path, "--inputs", "1", "--out-dir", outdir],
            cwd=os.path.dirname(HERE), capture_output=True, text=True,
            timeout=60)
        out = proc.stdout + proc.stderr
        expect(proc.returncode == 2,
               "fuzzer with non-executable compiler exited %d (expected 2)"
               % proc.returncode)
        expect("not executable" in out, "setup diagnostic absent")
        expect("Traceback" not in out, "fuzzer leaked a traceback")
    finally:
        os.unlink(path)
        shutil.rmtree(outdir, ignore_errors=True)


# ------------------------------------------------------------- asm normalize
def t_normalizer_label_renumbering():
    a = validate_hex_static.normalize_asm_text("\tl__l61: movlw 1\n")
    b = validate_hex_static.normalize_asm_text("\tl__l62: movlw 1\n")
    expect(a == b, "label renumbering not normalized: %r vs %r" % (a, b))


def t_normalizer_movlb_symbolic():
    a = validate_hex_static.normalize_asm_text("movlb v_osccon, 1\n")
    b = validate_hex_static.normalize_asm_text("movlb 1\n")
    expect(a == b, "symbolic MOVLB not normalized: %r vs %r" % (a, b))


def t_normalizer_drops_listing_noise():
    for noise in ("; banner\n", "foo EQU 0x10\n",
                  "= 2881[i-V----2]:{x}\n", "usec_delay(100000)\n"):
        got = validate_hex_static.normalize_asm_text(noise)
        expect(got == "", "listing noise survived: %r -> %r" % (noise, got))


def t_normalizer_keeps_real_diff():
    a = validate_hex_static.normalize_asm_text("movlw 1\n")
    b = validate_hex_static.normalize_asm_text("movlw 2\n")
    expect(a != b, "real operand difference was normalized away")


# --------------------------------------------------- compare baseline contract
def make_mini_repo(with_baseline):
    root = tempfile.mkdtemp(prefix="jal_harness_")
    ut = os.path.join(root, "unit-tests")
    os.makedirs(os.path.join(ut, "Output_Current", "hex"))
    os.makedirs(os.path.join(ut, "Output_Current", "full_output"))
    os.makedirs(os.path.join(ut, "Sample"))
    for mod in MINI_MODULES:
        shutil.copy2(os.path.join(HERE, mod), os.path.join(ut, mod))
    hex_text = ":00000001FF\n"
    with open(os.path.join(ut, "Output_Current", "hex", "foo.hex"), "w") as f:
        f.write(hex_text)
    with open(os.path.join(ut, "Sample", "foo.jal"), "w") as f:
        f.write("include 16f877\n")
    if with_baseline:
        os.makedirs(os.path.join(ut, "Output_Baseline", "hex"))
        with open(os.path.join(ut, "Output_Baseline", "hex", "foo.hex"), "w") as f:
            f.write(hex_text)
    return root


def run_compare(root):
    return subprocess.run(
        [sys.executable, os.path.join("unit-tests", "unittests.py"), "--compare"],
        cwd=root, capture_output=True, text=True, timeout=180)


def t_compare_without_baseline_fails():
    root = make_mini_repo(False)
    try:
        proc = run_compare(root)
        out = proc.stdout + proc.stderr
        expect(proc.returncode == 2,
               "--compare without baseline exited %d (expected 2)" % proc.returncode)
        expect("baseline HEX tree missing or empty" in out,
               "missing-baseline diagnostic absent")
        summary = os.path.join(root, "unit-tests", "Output_Compare",
                               "hex_size_summary.txt")
        expect(os.path.isfile(summary), "hex_size_summary.txt not written")
        text = open(summary, encoding="utf-8", errors="replace").read()
        expect("FAILED:" in text, "summary does not record FAILED")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def t_compare_with_baseline_passes():
    root = make_mini_repo(True)
    try:
        proc = run_compare(root)
        expect(proc.returncode == 0,
               "--compare with a usable baseline exited %d (expected 0): %s"
               % (proc.returncode, (proc.stdout + proc.stderr)[-800:]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


CHECKS = [
    ("diagnostics: clean summary", t_summary_counts),
    ("diagnostics: error/warning counts", t_error_counts),
    ("diagnostics: crash without summary is a failure",
     t_crash_without_summary_is_failure),
    ("diagnostics: signal after summary is a failure",
     t_signal_after_summary_is_failure),
    ("diagnostics: abort after summary is a failure",
     t_abort_signature_is_failure),
    ("spawn: missing compiler is a setup error",
     t_missing_compiler_is_setup_error),
    ("spawn: non-executable compiler is a setup error",
     t_non_executable_compiler_is_setup_error),
    ("spawn: executable compiler resolves",
     t_executable_compiler_resolves),
    ("spawn: fuzzer rejects an un-runnable compiler",
     t_fuzzer_spawn_error_is_setup_error),
    ("asm normalize: label renumbering", t_normalizer_label_renumbering),
    ("asm normalize: symbolic MOVLB", t_normalizer_movlb_symbolic),
    ("asm normalize: listing noise dropped", t_normalizer_drops_listing_noise),
    ("asm normalize: real diff preserved", t_normalizer_keeps_real_diff),
    ("compare: missing baseline exits 2", t_compare_without_baseline_fails),
    ("compare: usable baseline exits 0", t_compare_with_baseline_passes),
]


def main():
    for name, fn in CHECKS:
        check(name, fn)
    print("----")
    if failures:
        for name, tb in failures:
            print("=== %s" % name)
            print(tb)
        print("harness contract: %d FAILURE(S) of %d checks"
              % (len(failures), checks))
        return 1
    print("harness contract: PASS (%d checks)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())

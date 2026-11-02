#!/usr/bin/env python3
"""Corpus line-coverage report for the jalv3 compiler.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.

WHY THIS EXISTS
    Every other gate answers "does it still work?".  None answered "which
    code has no test at all?", so a defect could live in a straight-line
    function that no sample reaches and no reviewer notices.  This builds
    the gcov flavor (make COV=1), compiles a spread of the corpus with it,
    and reports executed/total source lines per module against a committed
    baseline, so a drop is a reviewable diff instead of a surprise.

WHAT IT COMPILES
    * demo_sample/            (fork demos; every target family)
    * sims/gpsim/             (classic runtime witnesses)
    * sims/pic32/, sims/esp32/(the two 32-bit back ends)
    * third_party/jallib/samples, every --stride-th file (--all for every one)
    The 32-bit and gpsim suites matter: without them the MIPS/ESP32 back
    ends read ~0% and the report is actively misleading.

USAGE
    python3 unit-tests/coverage_report.py                 # default stride
    python3 unit-tests/coverage_report.py --all           # full corpus (~slow)
    python3 unit-tests/coverage_report.py --update-baseline
    python3 unit-tests/coverage_report.py --no-build      # reuse the binary

EXIT
    0 when overall coverage is at or above the baseline (minus tolerance),
    1 when it regressed, 2 on a setup error.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import tempfile
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SRC = os.path.join(REPO, "src")
BIN = os.path.join(REPO, "bin", "beta", "jalv3-x86-64-cov")
BASELINE = os.path.join(HERE, "coverage_baseline.txt")
REPORT = os.path.join(HERE, "Comparison", "coverage_report.txt")

INCLUDES = ["-s", os.path.join(REPO, "demo_lib"),
            "-s", os.path.join(REPO, "third_party", "jallib", "lib"),
            "-s", os.path.join(REPO, "sims", "shared"),
            "-s", os.path.join(REPO, "sims")]

# module dirs under src/ that carry .c files, in report order
MODULES = ["jal", "libcore", "libpic12", "libpic32", "libesp32", "libutils"]

GCOV_RE = re.compile(r"Lines executed:([0-9.]+)% of (\d+)")


def log(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def build_cov():
    log("building the gcov flavor (make COV=1) ...")
    r = subprocess.run(["make", "-C", SRC, "ARCH=x86-64", "COV=1"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(BIN):
        sys.stderr.write(r.stdout.decode("utf-8", "replace")[-4000:])
        log("error: coverage build failed")
        return False
    return True


def clear_gcda():
    n = 0
    for mod in MODULES:
        d = os.path.join(SRC, mod)
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            for f in files:
                if f.endswith(".gcda"):
                    os.remove(os.path.join(root, f))
                    n += 1
    log("cleared %d counters" % n)


def corpus(args):
    """Deterministic input list: explicit suites in full, corpus by stride."""
    files = []
    for rel in ("demo_sample", os.path.join("sims", "gpsim"),
                os.path.join("sims", "pic32"), os.path.join("sims", "esp32")):
        d = os.path.join(REPO, rel)
        if not os.path.isdir(d):
            continue
        files += sorted(os.path.join(d, f) for f in os.listdir(d)
                        if f.endswith(".jal"))
    samples = os.path.join(REPO, "third_party", "jallib", "samples")
    if os.path.isdir(samples):
        pool = []
        for root, _dirs, fs in os.walk(samples):
            pool += [os.path.join(root, f) for f in fs if f.endswith(".jal")]
        pool.sort()
        step = 1 if (args.all or args.stride <= 1) else args.stride
        files += pool[::step]
    return files


def compile_one(path):
    """Compile one file; failures are expected and irrelevant to coverage."""
    try:
        subprocess.run([BIN] + INCLUDES + ["-no-codfile", path],
                       cwd=REPO, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=120)
    except subprocess.TimeoutExpired:
        pass


COD_PROBES = (
    "demo_sample/16f887_blink.jal",
    "demo_sample/32mz1024efh064_blink_intosc_8mhz.jal",
    "demo_sample/esp32_xtensa_lx6_blink_240mhz.jal",
)


def run_cod_probe():
    """Compile a few samples WITH debug information enabled.

    Everything else (the corpus and every pin) passes `-no-codfile`, which
    left src/libutils/cod_file.c at ~3% -- the COD writer had no coverage at
    all.  The files are written to a temp dir and dropped; only the executed
    lines are being measured.
    """
    for rel in COD_PROBES:
        path = os.path.join(REPO, rel)
        if not os.path.exists(path):
            continue
        with tempfile.TemporaryDirectory() as d:
            try:
                subprocess.run([BIN] + INCLUDES +
                               ["-codfile", os.path.join(d, "probe.cod"), path],
                               cwd=REPO, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=120)
            except subprocess.TimeoutExpired:
                pass


def run_corpus(files, jobs):
    log("compiling %d inputs under gcov ..." % len(files))
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for _ in ex.map(compile_one, files):
            done += 1
            if done % 100 == 0:
                log("  %d/%d" % (done, len(files)))


def gcov_module(mod):
    """Run gcov over one module dir; return (executed, total, per-file rows)."""
    d = os.path.join(SRC, mod)
    objdir = os.path.join(d, ".obj-x86-64-cov")
    if not os.path.isdir(objdir):
        return 0, 0, []
    rows = []
    executed = total = 0
    for f in sorted(os.listdir(d)):
        if not f.endswith(".c"):
            continue
        base = f[:-2]
        if not os.path.exists(os.path.join(objdir, base + ".gcno")):
            continue
        subprocess.run(["gcov", "-o", ".obj-x86-64-cov", f],
                       cwd=d, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        # Take the counts from the annotated source, not from the percentage:
        # `round(pct * lines)` is off by up to one per file, and the executed
        # floor below must not wobble with that rounding.
        exe = n = 0
        gc = os.path.join(d, f + ".gcov")
        if os.path.exists(gc):
            for line in open(gc, errors="replace"):
                m = re.match(r"^\s*([0-9]+\*?|#####|=====|-):\s*[0-9]+:", line)
                if not m or m.group(1) == "-":
                    continue
                n += 1
                if m.group(1)[0].isdigit():
                    exe += 1
        if not n:
            continue
        pct = 100.0 * exe / n
        executed += exe
        total += n
        rows.append((f, pct, n, exe))
    # gcov leaves .gcov files next to the sources; do not pollute the tree
    for f in os.listdir(d):
        if f.endswith(".gcov"):
            os.remove(os.path.join(d, f))
    return executed, total, rows


def read_baseline():
    """Return {mode, overall:(pct,lines,exe|None), modules:{name:(pct,lines,exe|None)}}."""
    if not os.path.exists(BASELINE):
        return None
    out = {"mode": None, "overall": None, "modules": {}}
    for line in open(BASELINE):
        m = re.match(r"overall\s+([0-9.]+)\s+([0-9]+)(?:\s+([0-9]+))?", line)
        if m:
            out["overall"] = (float(m.group(1)), int(m.group(2)),
                              int(m.group(3)) if m.group(3) else None)
        m = re.match(r"module\s+(\S+)\s+([0-9.]+)\s+([0-9]+)(?:\s+([0-9]+))?",
                     line)
        if m:
            out["modules"][m.group(1)] = (float(m.group(2)),
                                          int(m.group(3)),
                                          int(m.group(4)) if m.group(4) else None)
        m = re.match(r"mode\s+(\S+)", line)
        if m:
            out["mode"] = m.group(1)
    return out if out["overall"] else None


def write_baseline(overall_pct, overall_lines, overall_exe, per_mod, mode):
    with open(BASELINE, "w") as fh:
        fh.write("# coverage baseline -- regenerate with\n")
        fh.write("#   python3 unit-tests/coverage_report.py --update-baseline\n")
        fh.write("# overall <percent> <executable lines> <executed lines>\n")
        fh.write("# The executed count is a FLOOR as well as the percentage:\n")
        fh.write("# deleting tested code must not be able to buy coverage.\n")
        fh.write("# mode <all|strideN>  -- the input set this number describes;\n")
        fh.write("#   a run with a different mode is reported but not compared\n")
        fh.write("mode %s\n" % mode)
        fh.write("overall %.2f %d %d\n" % (overall_pct, overall_lines, overall_exe))
        for mod, pct, n, exe in per_mod:
            fh.write("module  %-10s %.2f %d %d\n" % (mod, pct, n, exe))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="compile every jallib sample (slow)")
    ap.add_argument("--stride", type=int, default=0,
                    help="compile every Nth jallib sample (0 = every one)")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--tolerance", type=float, default=0.5,
                    help="allowed drop in overall percent before failing")
    ap.add_argument("--exec-tolerance", type=int, default=10,
                    help="allowed drop in EXECUTED lines before failing "
                         "(the floor that stops deleting tested code from "
                         "buying coverage)")
    args = ap.parse_args()

    if not args.no_build:
        if not build_cov():
            return 2
    elif not os.path.exists(BIN):
        log("error: %s missing (run without --no-build)" % BIN)
        return 2

    clear_gcda()
    files = corpus(args)
    if not files:
        log("error: no inputs found")
        return 2
    run_corpus(files, args.jobs)
    run_cod_probe()

    log("collecting coverage ...")
    per_mod = []
    overall_exe = overall_tot = 0
    detail = []
    for mod in MODULES:
        exe, tot, rows = gcov_module(mod)
        if not tot:
            continue
        pct = 100.0 * exe / tot
        per_mod.append((mod, pct, tot, exe))
        detail += [(mod, r) for r in rows]
        overall_exe += exe
        overall_tot += tot
    if not overall_tot:
        log("error: no counters were produced (instrumented binary not used?)")
        return 2
    overall = 100.0 * overall_exe / overall_tot

    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, "w") as fh:
        fh.write("jalv3 corpus line coverage\n")
        fh.write("inputs: %d   executable lines: %d\n\n" % (len(files), overall_tot))
        fh.write("%-10s %8s %8s %10s\n" % ("module", "lines", "exe", "pct"))
        for mod, pct, tot, exe in per_mod:
            fh.write("%-10s %8d %8d %9.2f%%\n" % (mod, tot, exe, pct))
        fh.write("%-10s %8d %8d %9.2f%%\n"
                 % ("OVERALL", overall_tot, overall_exe, overall))
        fh.write("\nper file:\n")
        for mod, (f, pct, n, exe) in sorted(detail):
            fh.write("  %-10s %-26s %6.2f%% of %d (%d executed)\n"
                     % (mod, f, pct, n, exe))

    print("module     lines     exe       pct")
    for mod, pct, tot, exe in per_mod:
        print("%-10s %6d %7d %8.2f%%" % (mod, tot, exe, pct))
    print("%-10s %6d %7d %8.2f%%" % ("OVERALL", overall_tot, overall_exe, overall))
    print("report: %s" % os.path.relpath(REPORT, REPO))

    mode = "all" if (args.all or args.stride <= 1) else "stride%d" % args.stride

    if args.update_baseline:
        write_baseline(overall, overall_tot, overall_exe, per_mod, mode)
        print("baseline updated: %s" % os.path.relpath(BASELINE, REPO))
        return 0

    return compare_baseline(read_baseline(), overall, overall_exe, mode, args)


def compare_baseline(base, overall, overall_exe, mode, args):
    """Verdict for one measurement against the baseline (0 ok, 1 regression)."""
    if base is None:
        print("no baseline yet; run with --update-baseline")
        return 0
    if base["mode"] and base["mode"] != mode:
        # Different input set: the numbers are not comparable, so report
        # but do not fail.  (A stride-8 run legitimately covers slightly
        # less than --all and is not a regression.)
        print("note: baseline was measured with mode %s, this run is %s -- "
              "not comparing (overall %.2f%% vs %.2f%%)"
              % (base["mode"], mode, overall, base["overall"][0]))
        return 0
    base_pct, _base_lines, base_exe = base["overall"]
    floor = base_pct - args.tolerance
    fails = []
    if overall + 1e-9 < floor:
        fails.append("overall %.2f%% < baseline %.2f%% (floor %.2f%%)"
                     % (overall, base_pct, floor))
    # The executed-line floor: a percentage can be raised by deleting code
    # that was never executed (legitimate) or code that WAS executed (lost
    # test coverage).  Only the second is a regression, and only this floor
    # can tell them apart.
    if base_exe is not None and overall_exe < base_exe - args.exec_tolerance:
        fails.append("overall EXECUTED lines %d < baseline %d (tolerance %d)"
                     % (overall_exe, base_exe, args.exec_tolerance))
    if fails:
        print("REGRESSION:")
        for f in fails:
            print("  " + f)
        return 1
    print("ok: overall %.2f%% vs baseline %.2f%%; executed %d vs baseline %d"
          % (overall, base_pct, overall_exe,
             base_exe if base_exe is not None else overall_exe))
    return 0


if __name__ == "__main__":
    sys.exit(main())

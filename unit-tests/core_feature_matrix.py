#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""Core x feature matrix for the shared p-code optimiser.

Every codegen feature is one of three things, and this gate makes the
distinction explicit for each supported core:

  policy    -- the fold exists and is target-independent
  mechanism -- the back end can lower the result
  capability-- the core can do it at all

The pass that performs the byref tag / masked-predicate / constant-bit
logical folds is registered once in the pfile vectors (jal_main.c:671,
called from pfile.c:4190), so it runs for every target.  The gate proves
that claim instead of trusting file placement:

  fold   compile a folded witness and a hand-written twin; the normalised
         instruction streams must be IDENTICAL (the fold removed the
         predicate and nothing else changed)
  accept the lowering must exist; the back end's code-size number is
         pinned against the baseline so silent growth is visible
  refuse the compile must fail; the diagnostic text is pinned
  na     explicitly not applicable on that core, with the reason

Usage
  python3 unit-tests/core_feature_matrix.py [--compiler PATH]
  python3 unit-tests/core_feature_matrix.py --update   # rewrites the
      baseline and unit-tests/core-feature-matrix.md after a reviewed change

Exit: 0 every cell matches, 1 a cell drifted, 2 the compiler is unusable.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_COMPILER = ROOT / "bin" / "beta" / "jalv3-x86-64"
BASELINE = ROOT / "unit-tests" / "core-feature-baseline.txt"
MATRIX_MD = ROOT / "unit-tests" / "core-feature-matrix.md"

# Gate mode: the matrix is the engine-off baseline (see run_gates.sh T3).
os.environ["JALV2_FAT_POINTERS"] = "0"

CORES = [
    ("pic12", "10f200", "third_party/jallib/lib"),
    ("pic14", "16f887", "third_party/jallib/lib"),
    ("pic18", "18f4620", "third_party/jallib/lib"),
    ("pic32", "regression_32mx270f256b_device",
     "demo_lib;third_party/jallib/lib;sims/shared"),
    ("esp32", "regression_esp32_device",
     "demo_lib;third_party/jallib/lib;sims/shared"),
]

TAG_FOLD = """
procedure sink(byref in a[]) is
   if _is_array() then
      out = 1
   else
      out = 2
   end if
end procedure
"""

TAG_FOLD_HAND = """
procedure sink(byref in a[]) is
   out = 1
end procedure
"""

MASKED_FOLD = """
procedure sink(byref in a[], byte in x) is
   if _is_array() & (x == 0) then
      out = 1
   else
      out = 2
   end if
end procedure
"""

MASKED_HAND = """
procedure sink(byref in a[], byte in x) is
   if x == 0 then
      out = 1
   else
      out = 2
   end if
end procedure
"""

LOGICAL_FOLD = """
procedure sink(byref in a[], bit in flag) is
   if _is_array() & flag then
      out = 1
   else
      out = 2
   end if
end procedure
"""

LOGICAL_HAND = """
procedure sink(byref in a[], bit in flag) is
   if flag then
      out = 1
   else
      out = 2
   end if
end procedure
"""

VIEW_CONST = """
procedure sink(byref in a[]) is
   out = a[0]
end procedure
"""

VIEW_RUNTIME = """
var byte i

procedure sink(byref in a[]) is
   out = a[i]
end procedure
"""

INDIRECT = """
var byte i
var byte a[4]

i = 2
a[i] = 7
out = a[i]
"""

CONST_ARRAY = """
const byte m1[] = {0, 66}
"""

# The diagnostic is the contract: name the limit and the way out once.
REFUSE_INDIRECT = (
    "12-bit core: no indirect addressing (no FSR/IRP); a pointer or a "
    "runtime index needs a 14-bit part -- use a constant index here")

# feature -> (kind, ...)
FEATURES = [
    ("tag_fold", "fold", TAG_FOLD, TAG_FOLD_HAND, "sink(m1)", CONST_ARRAY,
     "uniform `_is_array()` folds away"),
    ("masked_predicate", "fold", MASKED_FOLD, MASKED_HAND, "sink(m1, 0)",
     CONST_ARRAY, "`_is_array() & (x == 0)` folds to `x == 0`"),
    ("logical_fold", "fold", LOGICAL_FOLD, LOGICAL_HAND, "sink(m1, 0)",
     CONST_ARRAY, "`_is_array() & flag` folds to `flag`"),
    ("byref_view_const", "capability", VIEW_CONST, None, "sink(m1)",
     CONST_ARRAY, "constant-index view through a byref formal"),
    ("byref_view_runtime", "accept", VIEW_RUNTIME, None,
     "i = 1\nsink(m1)", CONST_ARRAY, "runtime-index view through a byref formal"),
    ("indirect_addressing", "capability", INDIRECT, None, "", "",
     "general pointer / runtime index"),
]

# 12-bit has no FSR/IRP: neither view can be lowered.  The constant-index
# view refuses through the capability check in pic_assign_from_lookup.
NA = {
    ("pic12", "byref_view_runtime"):
        "no general indirect addressing; the constant-index cell records "
        "the refusal",
}

FOLD_TAIL = """
out = 0x11
{main}
forever loop
end loop
"""


def libs_for(spec: str) -> str:
    return ";".join(str(ROOT / p) if not p.startswith("/") else p
                    for p in spec.split(";"))


def source(device: str, body: str, main: str) -> str:
    text = "include %s\n\nvar volatile byte out\n\n%s\n" % (device, body)
    if main:
        text += FOLD_TAIL.format(main=main)
    return text


def normalise_asm(path: Path) -> list[str]:
    """Instruction stream only: drop comments, blanks and generated labels."""
    out = []
    for line in path.read_text(errors="replace").splitlines():
        if ";" in line:
            line = line.split(";", 1)[0]
        line = line.rstrip()
        if not line:
            continue
        line = re.sub(r"l__l\d+", "l__l#", line)
        out.append(line)
    return out


def instruction_count(lines: list[str]) -> int:
    count = 0
    for line in lines:
        if not re.match(r"^[0-9a-fA-F]+:\s", line):
            continue
        if line.rstrip().endswith("label"):
            continue
        if " .org" in line:
            continue
        count += 1
    return count


def run(compiler: Path, core: tuple, src: str, work: Path) -> tuple:
    jal = work / "probe.jal"
    jal.write_text(src)
    proc = subprocess.run(
        [str(compiler), "-s", libs_for(core[2]), "-asm", "probe.asm",
         "-hex", "probe.hex", "probe.jal"],
        cwd=work, capture_output=True, text=True, timeout=300)
    text = proc.stdout + proc.stderr
    asm = work / "probe.asm"
    lines = normalise_asm(asm) if asm.exists() else []
    return proc.returncode, text, lines


def metric(text: str, lines: list[str], cid: str) -> str:
    """The back end's own code-size number: `Code area` where it prints one,
    the listing's instruction count for PIC32 (it prints none)."""
    if cid == "pic32":
        return str(instruction_count(lines))
    found = re.search(r"^Code area: (\d+) of", text, re.M)
    if found:
        return found.group(1)
    return str(instruction_count(lines))


def errors(text: str) -> str | None:
    for line in text.splitlines():
        if "error:" not in line:
            continue
        line = re.sub(r"^[^:]*\.jal:\d+:\s*", "", line)
        line = line.strip()
        if line.startswith("error:"):
            line = line[len("error:"):].strip()
        return line
    return None


def evaluate(compiler: Path, core, feature) -> tuple[str, str]:
    """Return (status, value) for one cell; raise on a matrix mismatch."""
    cid, device, _libs = core
    name, kind, body, hand, main, prelude, _desc = feature
    work = Path(tempfile.mkdtemp(prefix="core-matrix-"))
    try:
        if (cid, name) in NA:
            return "na", NA[(cid, name)]
        if kind == "fold":
            src = source(device, prelude + "\n" + body, main)
            rc, text, lines = run(compiler, core, src, work)
            if rc != 0 or "0 errors, 0 warnings" not in text:
                raise SystemExit(
                    "FAIL %s/%s: folded witness did not compile\n%s"
                    % (cid, name, text[-800:]))
            hand_src = source(device, prelude + "\n" + hand, main)
            hand_dir = work / "hand"
            hand_dir.mkdir()
            rc2, text2, lines2 = run(compiler, core, hand_src, hand_dir)
            if rc2 != 0 or "0 errors, 0 warnings" not in text2:
                raise SystemExit(
                    "FAIL %s/%s: hand witness did not compile\n%s"
                    % (cid, name, text2[-800:]))
            if lines != lines2:
                import difflib
                diff = "\n".join(list(difflib.unified_diff(
                    lines, lines2, "folded", "hand", lineterm=""))[:24])
                raise SystemExit(
                    "FAIL %s/%s: folded body differs from the hand twin\n%s"
                    % (cid, name, diff))
            return "fold", "-"
        if kind == "accept":
            src = source(device, prelude + "\n" + body, main)
            rc, text, lines = run(compiler, core, src, work)
            if rc != 0 or "0 errors, 0 warnings" not in text:
                raise SystemExit(
                    "FAIL %s/%s: lowering missing\n%s"
                    % (cid, name, text[-800:]))
            return "accept", metric(text, lines, cid)
        if kind == "capability":
            src = source(device, prelude + "\n" + body, main)
            rc, text, lines = run(compiler, core, src, work)
            if rc == 0:
                return "accept", metric(text, lines, cid)
            got = errors(text)
            return "refuse", got or "(no error line)"
        raise SystemExit("FAIL: unknown cell kind %r for %s/%s"
                         % (kind, cid, name))
    finally:
        _cleanup(work)


def _cleanup(work: Path) -> None:
    import shutil
    shutil.rmtree(work, ignore_errors=True)


def load_baseline(path: Path) -> dict:
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 4:
            raise SystemExit("FAIL: malformed baseline row: %r" % line)
        rows[(parts[0], parts[1])] = (parts[2], parts[3])
    return rows


def write_baseline(path: Path, rows: dict) -> None:
    lines = ["# core\tfeature\tstatus\tvalue",
             "# value: '-' for a fold (self-checking twin), an instruction",
             "# code-size number for accept, the pinned diagnostic for refuse,",
             "# the reason for na.  Refresh with --update after a reviewed change."]
    for core in CORES:
        for feature in FEATURES:
            key = (core[0], feature[0])
            status, value = rows[key]
            lines.append("\t".join([key[0], key[1], status, value]))
    path.write_text("\n".join(lines) + "\n")


def write_matrix(path: Path, rows: dict) -> None:
    names = [f[0] for f in FEATURES]
    labels = [c[0] for c in CORES]
    out = ["| feature | " + " | ".join(labels) + " |",
           "|---" * (len(labels) + 1) + "|"]
    for name in names:
        cells = []
        for cid in labels:
            status, value = rows[(cid, name)]
            if status == "fold":
                cells.append("fold")
            elif status == "accept":
                cells.append("yes (%s)" % value)
            elif status == "refuse":
                cells.append("refused")
            else:
                cells.append("n/a")
        out.append("| %s | %s |" % (name, " | ".join(cells)))
    out += ["",
            "N is the back end's code-size unit: words on the 12/14-bit",
            "cores, bytes on the 18F and ESP32, instructions on PIC32.",
            "`fold` means the folded witness emits exactly the hand twin's",
            "instruction stream; `refused` means the compile fails with the",
            "diagnostic pinned in core-feature-baseline.txt."]
    path.write_text("\n".join(out) + "\n")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    parser.add_argument("--update", action="store_true",
                        help="rewrite the baseline and matrix from this build")
    args = parser.parse_args(argv)

    compiler = Path(args.compiler)
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        print("error: compiler not executable: %s" % compiler, file=sys.stderr)
        return 2

    baseline = load_baseline(BASELINE)
    rows = {}
    failures = []
    for core in CORES:
        for feature in FEATURES:
            key = (core[0], feature[0])
            status, value = evaluate(compiler, core, feature)
            rows[key] = (status, value)
            if args.update:
                continue
            if key not in baseline:
                failures.append("%s/%s: no baseline row" % key)
                continue
            was = baseline[key]
            if was != (status, value):
                failures.append(
                    "%s/%s: expected %s %r, got %s %r"
                    % (key[0], key[1], was[0], was[1], status, value))

    if args.update:
        write_baseline(BASELINE, rows)
        write_matrix(MATRIX_MD, rows)
        print("updated %s and %s" % (BASELINE.name, MATRIX_MD.name))
        return 0

    if failures:
        for line in failures:
            print("FAIL " + line, file=sys.stderr)
        print("core feature matrix: %d cell(s) drifted" % len(failures),
              file=sys.stderr)
        return 1

    print("core feature matrix: %d cells ok" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Measure ROM (A) literal-lowering size impact over the string-using programs.

Compiles every demo_sample/ + sims/ program that includes `strings_dynamic`
with two compilers (ROM (A) off and on) and reports the per-file code size,
the delta, and the win/loss distribution.  Metric: the compiler's own
`Code area:` count when present (words on classic/14-bit, bytes on PIC18);
32-bit back ends print no code-area line, so fall back to the emitted
instruction count from the .asm listing.

Used by `fixes/fix_rom_a_default_decision.md` (and its 2026-11-04 addendum /
`fixes/fix_ptr_read_call_fsr_clobber.md`) to re-decide
`JALV2_ROM_A_STRING_LITERAL_CALL`:

    python3 unit-tests/rom_a_measure.py --off ./bin/beta/jalv3-x86-64 \
        --on /path/to/rom-a-on/jalv3-x86-64

Two body fragments under `sims/shared/` are not standalone programs; they
report `error/error` and are excluded from the counts.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = "demo_lib;third_party/jallib/lib;sims/shared"


def collect():
    files = []
    for root in ("demo_sample", "sims"):
        for dirpath, _dirnames, names in os.walk(os.path.join(REPO, root)):
            for name in sorted(names):
                if not name.endswith(".jal"):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    text = open(path, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                if "strings_dynamic" in text:
                    files.append(os.path.relpath(path, REPO))
    return sorted(files)


CODE_RE = re.compile(r"Code area:\s*(\d+) of")
OPCODE_RE = re.compile(r"^\s+[a-z][a-z0-9]*\s")


def size_of(compiler, rel, workdir):
    asm = os.path.join(workdir, "out.asm")
    hexf = os.path.join(workdir, "out.hex")
    for stale in (asm, hexf):
        if os.path.exists(stale):
            os.unlink(stale)
    cmd = [compiler, "-s", INC, "-no-codfile", "-asm", asm, "-hex", hexf,
           os.path.join(REPO, rel)]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    m = CODE_RE.search(out)
    if m:
        return int(m.group(1)), "code"
    if proc.returncode != 0:
        return None, "error"
    n = 0
    try:
        for line in open(asm, encoding="utf-8", errors="replace"):
            s = line.strip()
            if not s or s.startswith(";") or s.startswith("--"):
                continue
            if "EQU" in s or s.startswith(("org", "db", "dw", "dt", "end")):
                continue
            if OPCODE_RE.match(line):
                n += 1
    except OSError:
        return None, "error"
    return n, "instr"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--off", required=True, help="ROM (A)=0 compiler")
    ap.add_argument("--on", required=True, help="ROM (A)=1 compiler")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    files = collect()
    rows = []
    for rel in files:
        with tempfile.TemporaryDirectory() as td:
            a, ak = size_of(args.off, rel, td)
            b, bk = size_of(args.on, rel, td)
        if a is None or b is None or ak != bk:
            rows.append((rel, a, b, None, ak + "/" + bk))
            continue
        rows.append((rel, a, b, b - a, ak))

    wins = [r for r in rows if r[3] is not None and r[3] < 0]
    losses = [r for r in rows if r[3] is not None and r[3] > 0]
    same = [r for r in rows if r[3] == 0]
    bad = [r for r in rows if r[3] is None]
    net = sum(r[3] for r in rows if r[3] is not None)

    lines = []
    for rel, a, b, d, k in rows:
        lines.append("%s %s %s %s %s" % (rel, a, b,
                                         "" if d is None else d, k))
    lines.append("")
    lines.append("files %d  wins %d  losses %d  unchanged %d  errors %d  net %d"
                 % (len(rows), len(wins), len(losses), len(same), len(bad), net))
    text = "\n".join(lines) + "\n"
    if args.out:
        open(args.out, "w").write(text)
    print(text)
    print("biggest wins:")
    for r in sorted(wins, key=lambda x: x[3])[:8]:
        print("  %-70s %d -> %d  (%+d)" % (r[0], r[1], r[2], r[3]))
    print("biggest losses:")
    for r in sorted(losses, key=lambda x: -x[3])[:8]:
        print("  %-70s %d -> %d  (%+d)" % (r[0], r[1], r[2], r[3]))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""In-process unit tests: every fixture compiles in ONE process (JAL_MULTI).

Each fixture states its own expectation in a header comment:

    -- @expect errors 0            exact error count
    -- @expect errors >0           any non-zero error count
    -- @expect contains: <text>    this text must appear in the fixture's output
    -- @expect not-contains: <text>

The point is not only speed. Compiling them together exercises
jal_reset_between_compiles() between every fixture, so a unit test that
passes alone but fails after another fixture indicates state surviving a
compile -- something no single-compile gate can see.

Usage: unit-tests/test_inprocess_units.py [--compiler PATH] [--lib PATH]
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FIXTURES = os.path.join(ROOT, "unit-tests", "fixtures", "units")
DEFAULT_LIB = os.path.join(ROOT, "demo_lib") + ";" + \
    os.path.join(ROOT, "third_party", "jallib", "lib")


def expectations(text):
    exp = {"errors": None, "contains": [], "not_contains": []}
    for m in re.finditer(r"^--\s*@expect\s+(.+)$", text, re.M):
        want = m.group(1).strip()
        if want.startswith("errors"):
            val = want.split(None, 1)[1].strip()
            exp["errors"] = ("any", None) if val.startswith(">") else ("exact", int(val))
        elif want.startswith("contains:"):
            exp["contains"].append(want.split(":", 1)[1].strip())
        elif want.startswith("not-contains:"):
            exp["not_contains"].append(want.split(":", 1)[1].strip())
    return exp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=os.path.join(ROOT, "bin", "jalv3-x86-64"))
    ap.add_argument("--lib", default=DEFAULT_LIB)
    args = ap.parse_args()

    names = sorted(f for f in os.listdir(FIXTURES) if f.endswith(".jal"))
    if not names:
        print("no fixtures", file=sys.stderr)
        return 2
    paths = [os.path.join(FIXTURES, n) for n in names]
    lists = os.path.join(ROOT, "tmp", "inproc_units.txt")
    os.makedirs(os.path.dirname(lists), exist_ok=True)
    with open(lists, "w") as f:
        f.write("\n".join(paths) + "\n")

    env = dict(os.environ)
    env["JAL_MULTI"] = lists
    r = subprocess.run(
        [args.compiler, "-s", args.lib, "-no-codfile", paths[0]],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    text = (r.stdout or b"").decode(errors="replace")

    blocks = {}
    cur = None
    buf = []
    for line in text.splitlines():
        if line.startswith("=== FILE "):
            if cur:
                blocks[cur] = "\n".join(buf)
            cur = line[len("=== FILE "):].strip()
            buf = []
        else:
            buf.append(line)
    if cur:
        blocks[cur] = "\n".join(buf)

    fails = 0
    for path in paths:
        name = os.path.basename(path)
        body = blocks.get(path)
        exp = expectations(open(path).read())
        if body is None:
            print("FAIL %-24s did not compile (process died before it?)" % name)
            fails += 1
            continue
        m = re.search(r"(\d+) errors, (\d+) warnings", body)
        if not m:
            print("FAIL %-24s no summary line" % name)
            fails += 1
            continue
        errs = int(m.group(1))
        problems = []
        if exp["errors"]:
            kind, val = exp["errors"]
            if kind == "exact" and errs != val:
                problems.append("errors %d != %d" % (errs, val))
            if kind == "any" and errs == 0:
                problems.append("expected errors, got 0")
        for want in exp["contains"]:
            if want not in body:
                problems.append("missing %r" % want)
        for want in exp["not_contains"]:
            if want in body:
                problems.append("unexpected %r" % want)
        if problems:
            print("FAIL %-24s %s" % (name, "; ".join(problems)))
            fails += 1
        else:
            print("ok   %-24s %d errors" % (name, errs))

    print("----")
    if fails:
        print("in-process units: %d FAILURE(S)" % fails)
        return 1
    print("in-process units: PASS (%d fixtures, 1 process)" % len(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main())

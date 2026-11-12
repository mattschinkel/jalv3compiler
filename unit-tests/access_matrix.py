#!/usr/bin/env python3
"""Print the kind x site matrix from an access-recorder log.

Reads the CSV written by JALV2_ACCESS_RECORD (header commented with #),
groups rows by (program, site, kind, named inputs) and prints one markdown
row per group with a count.  Step 1a uses this to prove one row.

--coverage unions the per-compile `jal access coverage: rows=N bitmap=X`
lines written by JALV2_ACCESS_COVERAGE and checks them against the pinned
coverage baseline (A12).  A recipe row that stops being exercised is a
regression; newly covered rows must be pinned with --update-coverage.
"""
import argparse
import collections
import re
import sys

INPUTS = [
    "aop", "chunked", "holed", "sv_view", "runtime_map", "value_map",
    "ptr_lookup", "ptr_read_call",
]
COVERAGE_RE = re.compile(r"coverage: rows=(\d+) bitmap=([0-9a-f]+)")


def write_matrix(log, out):
    groups = collections.OrderedDict()
    try:
        with open(log, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(",")
                if len(parts) < 6 + len(INPUTS):
                    continue
                program, generation, site, var = parts[0:4]
                kind = parts[5]
                facts = tuple(parts[6:6 + len(INPUTS)])
                key = (program, site, kind, facts)
                groups[key] = groups.get(key, 0) + 1
    except OSError as exc:
        print(f"cannot read {log}: {exc}", file=sys.stderr)
        return 1

    header = ["program", "site", "kind"] + INPUTS + ["rows"]
    lines = ["| " + " | ".join(header) + " |",
             "|" + "|".join(["---"] * len(header)) + "|"]
    for (program, site, kind, facts), count in groups.items():
        lines.append("| " + " | ".join(
            [program, site, kind] + list(facts) + [str(count)]) + " |")
    text = "\n".join(lines) + "\n"
    if out:
        with open(out, "w", encoding="utf-8") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)
    return 0


def read_coverage(path):
    rows = None
    union = 0
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = COVERAGE_RE.search(line)
                if not match:
                    continue
                rows = int(match.group(1))
                union |= int(match.group(2), 16)
    except OSError as exc:
        print(f"cannot read {path}: {exc}", file=sys.stderr)
        return None, None
    return rows, union


def read_coverage_baseline(path):
    rows = None
    exercised = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith("rows:"):
                    rows = int(line.split(":", 1)[1])
                elif line.startswith("exercised:"):
                    body = line.split(":", 1)[1].strip()
                    if body:
                        exercised = {int(x) for x in body.split(",")}
    except OSError as exc:
        print(f"cannot read {path}: {exc}", file=sys.stderr)
        return None, None
    return rows, exercised


def write_coverage_baseline(path, rows, exercised, log):
    missing = [i for i in range(rows) if i not in exercised]
    lines = [
        "# A12 recipe-row coverage, pinned by `run_access_matrix.sh --check`.",
        "# Row indexes follow the recipe table in src/libpic12/pic_access.c.",
        "# Gate corpus: the classic matrix corpus plus sims/shared, the",
        "# corpus overlays, and the FAT-gated pointer witnesses.",
        "# Not yet exercised (no gate fixture): "
        + (",".join(str(i) for i in missing) if missing else "none"),
        f"rows: {rows}",
        "exercised: " + ",".join(str(i) for i in sorted(exercised)),
    ]
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"coverage baseline updated: {path} ({len(exercised)}/{rows} rows)")
    return 0


def check_coverage(cov_log, baseline, update):
    rows, union = read_coverage(cov_log)
    if rows is None:
        print("JAL-MATRIX-COVER: no coverage lines found", file=sys.stderr)
        return 1
    exercised = {i for i in range(rows) if union >> i & 1}
    if update:
        return write_coverage_baseline(baseline, rows, exercised, cov_log)
    base_rows, base = read_coverage_baseline(baseline)
    if base_rows is None:
        print(f"JAL-MATRIX-COVER: missing baseline {baseline} "
              "(run --update)", file=sys.stderr)
        return 1
    lost = sorted(base - exercised)
    new = sorted(exercised - base)
    gaps = [i for i in range(rows) if i not in exercised]
    if lost:
        print("JAL-MATRIX-COVER: rows no longer exercised: "
              + ",".join(str(i) for i in lost), file=sys.stderr)
    if new:
        print("JAL-MATRIX-COVER: newly exercised rows (pin with --update): "
              + ",".join(str(i) for i in new), file=sys.stderr)
    print(f"coverage: {len(exercised)}/{rows} recipe rows exercised"
          + (f"; no fixture yet: {','.join(str(i) for i in gaps)}"
             if gaps else ""))
    return 1 if (lost or new) else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--out")
    parser.add_argument("--coverage")
    parser.add_argument("--coverage-baseline")
    parser.add_argument("--update-coverage", action="store_true")
    args = parser.parse_args()

    rc = write_matrix(args.log, args.out)
    if rc:
        return rc
    if args.coverage:
        if not args.coverage_baseline:
            print("--coverage needs --coverage-baseline", file=sys.stderr)
            return 1
        cov = check_coverage(args.coverage, args.coverage_baseline,
                             args.update_coverage)
        if cov:
            return cov
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""A18/C6: the duplicate-row policy for access-plan rows.

The rows are the CSV lines written by -dump-plan (JALV2_ACCESS_RECORD),
with the line annotation stripped by the plan gate.  An identical duplicate
row is collapsed and counted; two rows that share the canonical key but
differ in a fact are a fatal duplicate: the plan is not a function of the
key, which is exactly the per-emitter drift the access model replaced.

The canonical key is (program, generation, site, var, ordinal, kind) -- the
site id is file:function:label and the ordinal is the per-command access
counter (A7/A15), so two procedures cannot collide on a variable that shares
a name.  The eight named fact columns are the payload compared under that
key.  A differing recipe id for equal facts is structurally impossible (the
matcher is deterministic), so it cannot occur.

Usage: plan_dupcheck.py --log <rows-file>
Exit:  0 clean (identical duplicates collapsed), 1 differing duplicate.
"""
import argparse
import collections
import sys

KEY_FIELDS = 6   # program, generation, site, var, ordinal, kind
FACTS = 8


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--key-fields", type=int, default=KEY_FIELDS,
                        help="the canonical key column count (default 6; a "
                             "32-bit back end's rows key on its own columns "
                             "and treat the rest as the payload)")
    args = parser.parse_args()

    rows = 0
    unique = set()
    groups = collections.OrderedDict()
    try:
        with open(args.log, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(",")
                if len(parts) < args.key_fields + 1:
                    continue
                rows += 1
                unique.add(tuple(parts))
                key = tuple(parts[0:args.key_fields])
                facts = tuple(parts[args.key_fields:])
                groups.setdefault(key, []).append(facts)
    except OSError as exc:
        print(f"cannot read {args.log}: {exc}", file=sys.stderr)
        return 1

    bad = 0
    for key, facts in groups.items():
        first = facts[0]
        for other in facts[1:]:
            if other != first:
                bad += 1
                if bad <= 20:
                    print("JAL-PLAN-DUP " + ",".join(key)
                          + f" facts={','.join(first)}"
                          + f"/{','.join(other)}", file=sys.stderr)
    if bad:
        print(f"plan dupcheck: FAIL {bad} differing duplicate(s) "
              f"of {rows} rows", file=sys.stderr)
        return 1
    print(f"plan dupcheck: rows={rows} unique={len(unique)} "
          f"collapsed={rows - len(unique)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

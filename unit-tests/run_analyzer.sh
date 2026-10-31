#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# -fanalyzer over every translation unit, gated against a BASELINE of known
# findings (unit-tests/analyzer_baseline.txt).
#
# Why a baseline and not "zero warnings": the first full run produced 26
# findings, and triage says almost none of them is a defect:
#   - 7 in jal_tokn.c are correlation false positives -- `val`/`count` are
#     initialised on the mode-ENTRY transitions, which the analyzer cannot
#     tie to the state variable that selects the case;
#   - the cmd_brch/pf_cmd ones are the same shape (param count vs params);
#   - the malloc-leak ones are CLI/option tables allocated once at startup;
#   - the rest are "OOM is not checked", which is a whole-program policy
#     decision, not 26 separate bugs.
# What the gate CAN do is catch a NEW finding creeping in, which is the
# whole value of running it every time.
#
# The key is "file -Wkind", NOT file:line: a line-number baseline breaks on
# every unrelated edit above the finding, and a baseline that cries wolf is
# a baseline people regenerate without looking.
#
# Usage: unit-tests/run_analyzer.sh [--update-baseline]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
BASE="$REPO/unit-tests/analyzer_baseline.txt"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
update=0
[ "${1:-}" = "--update-baseline" ] && update=1

found="$WORK/found.txt"
: > "$found"
for f in "$REPO"/src/jal/*.c "$REPO"/src/libcore/*.c "$REPO"/src/libutils/*.c \
         "$REPO"/src/libpic12/*.c "$REPO"/src/libpic32/*.c \
         "$REPO"/src/libesp32/*.c; do
  ( cd "$REPO" && cc -fanalyzer -O1 -Wall -Wshadow -Wcast-qual -ansi\
 -pedantic -c "$f" -o "$WORK/x.o" 2>&1 ) \
    | grep -E "warning:.*\[-Wanalyzer-[a-z-]+\]" \
    | sed -E 's|^.*/src/|src/|; s|:[0-9]+:[0-9]+: warning:.*\[(-Wanalyzer-[a-z-]+)\].*| \1|' \
    >> "$found"
done
sort -u -o "$found" "$found"

if [ "$update" -eq 1 ]; then
  cp "$found" "$BASE"
  echo "analyzer: baseline updated with $(wc -l < "$BASE") findings"
  exit 0
fi
if [ ! -f "$BASE" ]; then
  echo "analyzer: no baseline ($BASE); run with --update-baseline"
  exit 2
fi
new=$(comm -23 "$found" <(sort -u "$BASE"))
gone=$(comm -13 "$found" <(sort -u "$BASE"))
total=$(wc -l < "$found")
if [ -n "$new" ]; then
  echo "FAIL analyzer: $(echo "$new" | wc -l) NEW finding(s):"
  echo "$new" | sed 's/^/  /'
  [ -n "$gone" ] && echo "  (baseline entries that no longer appear: $(echo "$gone" | wc -l))"
  exit 1
fi
echo "ok   analyzer: $total finding(s), all in the baseline"
if [ -n "$gone" ]; then
  echo "     ($(echo "$gone" | wc -l) baseline entries no longer appear - prune with --update-baseline)"
fi
exit 0

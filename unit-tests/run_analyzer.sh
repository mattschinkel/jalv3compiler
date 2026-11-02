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
# The key is "<file> -Wkind <function>", NOT file:line: a line-number
# baseline breaks on every unrelated edit above the finding, and a baseline
# that cries wolf is a baseline people regenerate without looking. The
# function is part of the key because one file can hold several findings of
# the same kind -- without it a second, unrelated instance would be absorbed
# by the first one's entry.
#
# Cross-toolchain: the baseline records the compiler that produced it, and the
# gate notes a different running version. GCC renames analyzer kinds and moves
# warning attribution between versions, so a key diff across toolchains must
# be READ, not trusted. CI pins ubuntu-24.04 for this reason; regenerate there
# (or with the same major) rather than on an unrelated dev box.
#
# GitHub: a failing run emits ::error annotations and a step summary, so the
# finding is visible on the run page. Actions logs need a token to download
# even for a public repo, which made an earlier failure undiagnosable from
# the URL alone.
#
# Usage: unit-tests/run_analyzer.sh [--update-baseline]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
BASE="$REPO/unit-tests/analyzer_baseline.txt"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
update=0
[ "${1:-}" = "--update-baseline" ] && update=1

# Parser for one compiler log: "<src path> -Wanalyzer-kind <function or ->".
# The "In function 'x':" line is printed once per function context, so it is
# remembered per FILE (a warning can be attributed to an inlined header).
# The compile runs under LC_ALL=C so GCC quotes with ASCII ' and the keys do
# not depend on the caller's locale (a UTF-8 locale makes GCC print U+2018 /
# U+2019, which mawk treats as three separate bytes).
cat > "$WORK/parse.awk" <<'AWK'
/: In function '/ {
  file = $0
  sub(/:.*$/, "", file)
  sub(/^.*\/src\//, "src/", file)
  fn = $0
  sub(/^.*In function '/, "", fn)
  sub(/':$/, "", fn)
  cur[file] = fn
  next
}
/warning:.*\[-Wanalyzer-[a-z-]+\]/ {
  file = $0
  sub(/:.*$/, "", file)
  sub(/^.*\/src\//, "src/", file)
  kind = $0
  sub(/^.*\[-Wanalyzer-/, "", kind)
  sub(/\].*$/, "", kind)
  key = file " -Wanalyzer-" kind
  if (file in cur) key = key " " cur[file]
  else            key = key " -"
  print key
}
AWK

summary() {
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
  printf '%s\n' "$*" >> "$GITHUB_STEP_SUMMARY"
}

found="$WORK/found.txt"
cc_failed="$WORK/cc_failed.txt"
: > "$found"
: > "$cc_failed"
for f in "$REPO"/src/jal/*.c "$REPO"/src/libcore/*.c "$REPO"/src/libutils/*.c \
         "$REPO"/src/libpic12/*.c "$REPO"/src/libpic32/*.c \
         "$REPO"/src/libesp32/*.c; do
  log="$WORK/one.log"
  # A translation unit that does not compile is a FAILED gate, not a silent
  # skip: discarding cc's status used to make an analyzer-only ICE look like
  # "fewer findings, all in the baseline".
  if ! ( cd "$REPO" && LC_ALL=C cc -fanalyzer -O1 -Wall -Wshadow -Wcast-qual \
         -ansi -pedantic -c "$f" -o "$WORK/x.o" ) > "$log" 2>&1 ; then
    printf '%s\n' "${f#$REPO/}" >> "$cc_failed"
  fi
  awk -f "$WORK/parse.awk" "$log" >> "$found"
done
sort -u -o "$found" "$found"

if [ -s "$cc_failed" ]; then
  echo "FAIL analyzer: $(wc -l < "$cc_failed") translation unit(s) did not compile under -fanalyzer:"
  sed 's/^/  /' "$cc_failed"
  summary "analyzer: compiler error in $(tr '\n' ' ' < "$cc_failed")"
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while read -r f; do
      printf '::error file=%s::-fanalyzer could not compile this file\n' "$f"
    done < "$cc_failed"
  fi
  exit 1
fi

if [ "$update" -eq 1 ]; then
  {
    echo "# analyzer baseline -- regenerate with:"
    echo "#   unit-tests/run_analyzer.sh --update-baseline"
    echo "# format: <src file> <-Wanalyzer-kind> <function, or ->"
    echo "# toolchain: $(cc --version 2>/dev/null | head -1)"
    cat "$found"
  } > "$BASE"
  echo "analyzer: baseline updated with $(wc -l < "$found") findings"
  exit 0
fi
if [ ! -f "$BASE" ]; then
  echo "analyzer: no baseline ($BASE); run with --update-baseline"
  exit 2
fi

recorded=$(sed -n 's/^# toolchain: //p' "$BASE" | head -1)
current=$(cc --version 2>/dev/null | head -1)
if [ -n "$recorded" ] && [ "$recorded" != "$current" ]; then
  echo "note: baseline recorded with '$recorded'"
  echo "      running           with '$current'"
  echo "      a key diff here may be a compiler-version difference, not a code change"
fi

grep -Ev '^[[:space:]]*(#|$)' "$BASE" | sort -u > "$WORK/base.txt"
new=$(comm -23 "$found" "$WORK/base.txt")
gone=$(comm -13 "$found" "$WORK/base.txt")
total=$(wc -l < "$found")
if [ -n "$new" ]; then
  echo "FAIL analyzer: $(echo "$new" | wc -l) NEW finding(s):"
  while IFS=' ' read -r f k fn; do
    [ -n "${fn:-}" ] || fn="-"
    echo "  $f $k ($fn)"
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
      printf '::error file=%s::new -fanalyzer finding: %s in %s\n' "$f" "$k" "$fn"
    fi
  done <<EOF
$new
EOF
  summary "analyzer: $(echo "$new" | wc -l) new finding(s) -- see annotations"
  [ -n "$gone" ] && echo "  (baseline entries that no longer appear: $(echo "$gone" | wc -l))"
  exit 1
fi
echo "ok   analyzer: $total finding(s), all in the baseline"
summary "analyzer: ok -- $total finding(s), all in the baseline"
if [ -n "$gone" ]; then
  echo "     ($(echo "$gone" | wc -l) baseline entries no longer appear - prune with --update-baseline)"
fi
exit 0

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# ENG S1: CLI validation for the two access-model options, -explain-access
# and -dump-plan.
#
#   1. -explain-access accepts only <file>:<line> with a decimal line in
#      1..65535; a missing colon, an empty file part, line 0, a non-numeric
#      or trailing-junk line, a negative line, an overflow and 65536 are all
#      usage errors (rc != 0, no image), and 65535 is the accepted boundary;
#   2. an over-long file name is refused instead of being truncated into a
#      name that can never match a source line;
#   3. -dump-plan needs a non-empty path;
#   4. an unreadable source fails both options and installs no image;
#   5. an unwritable dump path is an error (the recorder was requested), with
#      no image installed and a pre-existing output left untouched (atomic
#      install);
#   6. the happy paths work: the dump file is written and -explain-access
#      prints the decisions made at a real source line.
#
# Usage: unit-tests/test_access_cli.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC='demo_lib;third_party/jallib/lib;sims/shared'
WITNESS=sims/gpsim/16f887_gpsim_pointer_slot_chunked_elem.jal

fail() {
  echo "FAIL: $1"
  shift
  sed 's/^/     /' "$@" 2>/dev/null | tail -6
  fails=$((fails + 1))
}

run() {  # run <log> <args...>; returns the compiler's status
  local log="$1"
  shift
  ( cd "$REPO" && "$CC" -s "$INC" -no-codfile "$@" ) > "$log" 2>&1
}

# --- 1. the line grammar ---------------------------------------------------
expect_bad_line() {  # expect_bad_line <spec> <message-substring>
  local spec="$1" want="$2" log="$WORK/line_$(echo "$1" | tr -c 'A-Za-z0-9' '_').log"
  if run "$log" -explain-access "$spec" "$WITNESS"; then
    fail "-explain-access '$spec' was accepted" "$log"
  elif ! grep -qF -- "$want" "$log"; then
    fail "-explain-access '$spec': no diagnostic '$want'" "$log"
  else
    echo "ok   -explain-access '$spec' is refused ($want)"
  fi
}

expect_bad_line "x"        "needs <file>:<line>"
expect_bad_line ":5"       "needs a file name before ':line'"
expect_bad_line "$WITNESS:0"     "line must be 1..65535"
expect_bad_line "$WITNESS:abc"   "line must be 1..65535"
expect_bad_line "$WITNESS:12x"   "line must be 1..65535"
expect_bad_line "$WITNESS:-3"    "line must be 1..65535"
expect_bad_line "$WITNESS:999999999999999999999" "line must be 1..65535"
expect_bad_line "$WITNESS:65536" "line must be 1..65535"

LONG=$(printf 'a%.0s' $(seq 1 600))
if run "$WORK/long.log" -explain-access "$LONG:5" "$WITNESS"; then
  fail "an over-long file name was accepted" "$WORK/long.log"
elif grep -q 'the file name is too long' "$WORK/long.log"; then
  echo "ok   an over-long file name is refused"
else
  fail "no diagnostic for an over-long file name" "$WORK/long.log"
fi

# the accepted boundary still compiles and reports (no access is fine)
if run "$WORK/max.log" -explain-access "$WITNESS:65535" "$WITNESS" \
    && grep -q '^0 errors' "$WORK/max.log"; then
  echo "ok   line 65535 is accepted"
else
  fail "line 65535 was refused" "$WORK/max.log"
fi

# --- 2. the dump path -----------------------------------------------------
if run "$WORK/dump_empty.log" -dump-plan= "$WITNESS"; then
  fail "-dump-plan= was accepted" "$WORK/dump_empty.log"
elif grep -q 'dump-plan needs a file name' "$WORK/dump_empty.log"; then
  echo "ok   -dump-plan= is refused"
else
  fail "no diagnostic for an empty -dump-plan" "$WORK/dump_empty.log"
fi

if ( cd "$REPO" && "$CC" -s "$INC" -no-codfile "$WITNESS" -dump-plan ) \
    > "$WORK/dump_missing.log" 2>&1; then
  fail "-dump-plan with no value was accepted" "$WORK/dump_missing.log"
elif grep -q 'Required parameter to dump-plan is missing' \
    "$WORK/dump_missing.log"; then
  echo "ok   -dump-plan with no value is refused"
else
  fail "no diagnostic for a missing -dump-plan value" "$WORK/dump_missing.log"
fi

# --- 3. an unreadable source ---------------------------------------------
MISSING="$WORK/does_not_exist.jal"
for opt in "-explain-access" "-dump-plan"; do
  hex="$WORK/missing_$(echo "$opt" | tr -d '-').hex"
  rm -f "$hex"
  if ( cd "$REPO" && "$CC" -s "$INC" -hex "$hex" \
      "$opt" "$WORK/whatever:5" "$MISSING" ) > "$WORK/src.log" 2>&1; then
    fail "$opt accepted an unreadable source" "$WORK/src.log"
  elif [ -e "$hex" ]; then
    fail "$opt installed an image for an unreadable source"
  else
    echo "ok   $opt with an unreadable source fails without an image"
  fi
done

# --- 4. an unwritable dump path + atomic output ---------------------------
hex="$WORK/atomic.hex"
printf 'SENTINEL\n' > "$hex"
if ( cd "$REPO" && "$CC" -s "$INC" -hex "$hex" \
    -dump-plan /nonexistent-dir/plan.log "$WITNESS" ) \
    > "$WORK/unwritable.log" 2>&1; then
  fail "an unwritable dump path was accepted" "$WORK/unwritable.log"
elif ! grep -q 'access recorder cannot open /nonexistent-dir/plan.log' \
    "$WORK/unwritable.log"; then
  fail "no diagnostic for an unwritable dump path" "$WORK/unwritable.log"
elif [ "$(cat "$hex")" != "SENTINEL" ]; then
  fail "the failed compile clobbered a pre-existing output" "$hex"
else
  echo "ok   an unwritable dump path fails and leaves the output untouched"
fi

# --- 5. the happy paths ---------------------------------------------------
if run "$WORK/dump_ok.log" -dump-plan "$WORK/plan.log" "$WITNESS" \
    && [ -s "$WORK/plan.log" ] \
    && grep -q '^# program,' "$WORK/plan.log"; then
  echo "ok   -dump-plan writes the plan log"
else
  fail "-dump-plan happy path" "$WORK/dump_ok.log"
fi

if run "$WORK/explain_ok.log" -explain-access "$WITNESS:37" "$WITNESS" \
    && grep -q "access: program=$WITNESS line=37 " "$WORK/explain_ok.log"; then
  echo "ok   -explain-access prints the decisions at line 37"
else
  fail "-explain-access happy path" "$WORK/explain_ok.log"
fi

if (( fails )); then
  echo "FAIL: $fails access-CLI check(s)"
  exit 1
fi
echo "PASS: access CLI validation pin"

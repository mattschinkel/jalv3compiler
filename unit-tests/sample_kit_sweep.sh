#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Demo sample kit: every demo_sample/*.jal must COMPILE with 0 errors.
#
# This used to live only inside run_gates.sh at L3, so the kit was checked
# only on a landing run -- and a sample that stopped fitting its part
# (12f508_wide_math after the RAM-exhaustion diagnostic, 2026-11-02) sat
# broken for a day because no L3 ran. It is a cheap gate (~1 min): pull it
# out so L1 and CI can run the same code.
#
# CI cannot run the L3 ladder (runtime gates are local by design), so this
# script is the one sample-kit check CI can afford.
#
# Usage: unit-tests/sample_kit_sweep.sh [compiler] [-v]
#   -v   print each sample, not just failures
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC="$ROOT/bin/beta/jalv3-x86-64"
VERBOSE=0
for a in "$@"; do
  case "$a" in
    -v|--verbose) VERBOSE=1 ;;
    *) CC="$a" ;;
  esac
done
INC="$ROOT/demo_lib;$ROOT/third_party/jallib/lib"
[ -x "$CC" ] || { echo "sample kit: compiler not executable: $CC" >&2; exit 2; }

total=0
fails=0
warned=0
for s in "$ROOT"/demo_sample/*.jal; do
  total=$((total + 1))
  name="$(basename "$s")"
  out=$("$CC" -s "$INC" -no-codfile "$s" 2>&1) || true
  # The tally line must be PRESENT (a missing one means the compiler
  # aborted) and must start "0 errors".  It is not always the last line:
  # the ESP32 back end prints its usage report after it.
  if ! grep -qE '^0 errors, [0-9]+ warnings' <<<"$out"; then
    fails=$((fails + 1))
    echo "FAIL $name ($(grep -Eo '[0-9]+ errors?, [0-9]+ warnings?' <<<"$out" | tail -1))"
    grep -E 'error:' <<<"$out" | head -3 | sed 's/^/     /'
    continue
  fi
  w=$(grep -Eo '^0 errors, [0-9]+ warnings' <<<"$out" | grep -Eo '[0-9]+' | tail -1)
  if [ "${w:-0}" -gt 0 ]; then
    warned=$((warned + 1))
    echo "WARN $name ($w warnings)"
  elif [ "$VERBOSE" -eq 1 ]; then
    echo "  OK $name"
  fi
done

echo "----"
echo "sample kit: $total file(s), $fails failed, $warned with warnings"
if [ "$fails" -ne 0 ]; then
  echo "sample kit: FAIL"
  exit 1
fi
if [ "$total" -eq 0 ]; then
  echo "sample kit: FAIL (no samples found)"
  exit 2
fi
echo "sample kit: PASS"
exit 0

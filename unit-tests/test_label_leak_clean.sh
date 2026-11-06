#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Pins the 2026-10-29 16f1825 collaborator report: a failed compile must end
# with the real diagnostics only.  The compiler's internal label refcount
# invariant used to print
#     label leak: 0x... _lNNN
# on stderr after the error summary -- error recovery abandons partially
# built cmd trees, so the surviving reference is an artifact of the failed
# parse, not a user-facing defect.  The trace is now reported only for
# clean (-debug=compiler) runs (fixes/fix_label_leak_clean_diagnostics.md).
#
# The fixture aliases a pseudo-device name that does not exist, so the
# `print_string(serial_data, ...)` call inside a procedure takes the
# volatile-accessor synthesis path and the parse fails.
#
# Usage: unit-tests/test_label_leak_clean.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

FIX=$REPO/unit-tests/negative/label_leak_clean_alias.jal

"$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$FIX" \
  > "$WORK/out" 2> "$WORK/err"
rc=$?

if [ "$rc" -eq 0 ]; then
  echo "FAIL: fixture compiled clean (expected a diagnostic failure)"
  fails=$((fails + 1))
else
  echo "ok   fixture fails with a nonzero exit ($rc)"
fi

if grep -q '"serial_data" not defined' "$WORK/out"; then
  echo "ok   clean 'not defined' diagnostic present"
else
  echo "FAIL: expected '\"serial_data\" not defined' on stdout:"
  sed 's/^/     /' "$WORK/out"
  fails=$((fails + 1))
fi

if grep -q "errors," "$WORK/out"; then
  echo "ok   error summary line present"
else
  echo "FAIL: no 'N errors, M warnings' summary on stdout"
  fails=$((fails + 1))
fi

if grep -q "label leak" "$WORK/err"; then
  echo "FAIL: internal 'label leak' trace leaked to stderr:"
  sed 's/^/     /' "$WORK/err"
  fails=$((fails + 1))
else
  echo "ok   no internal 'label leak' trace on stderr"
fi

# The invariant itself must stay alive for developers.
"$CC" -debug=compiler -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" \
  "$FIX" > /dev/null 2> "$WORK/err_dbg"
if grep -q "label leak" "$WORK/err_dbg"; then
  echo "ok   -debug=compiler still reports the abandoned references"
else
  echo "FAIL: -debug=compiler no longer reports the leak (invariant lost)"
  fails=$((fails + 1))
fi

# A clean compile must not print the trace either -- on EVERY back end.
# The 2026-11-02 recurring-fix audit found the 32-bit pseudo-slot dispatch
# (jal_expr.c / jal_blck.c) leaking its two labels per dispatch site, which
# the PIC18-only clean check could not see.  Outputs go to $WORK so a gate
# run never writes .asm/.hex beside the fixtures.
clean_no_leak() { # name file [extra compiler args...]
  local name=$1 file=$2 out err rc
  shift 2
  out="$WORK/clean_$name.out"
  err="$WORK/clean_$name.err"
  "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$@" \
    -no-codfile -asm "$WORK/clean_$name.asm" -hex "$WORK/clean_$name.hex" \
    "$file" >"$out" 2>"$err"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL $name: clean compile exited $rc"
    tail -4 "$out" | sed 's/^/     /'
    fails=$((fails + 1))
  elif grep -q "label leak" "$err"; then
    echo "FAIL $name: clean compile printed a 'label leak' trace:"
    head -10 "$err" | sed 's/^/     /'
    fails=$((fails + 1))
  else
    echo "ok   $name: clean compile prints no leak trace"
  fi
}

clean_no_leak pic18_blink "$REPO/demo_sample/18f67j50_blink.jal"
clean_no_leak pic14h_blink "$REPO/demo_sample/16f18857_blink.jal"
clean_no_leak pic32_slot_rt "$REPO/sims/pic32/32mx270f256b_body_ptr_slot_rt.jal" \
  -s "$REPO/sims/shared" -s "$REPO/sims"
clean_no_leak esp32_slot_rt "$REPO/sims/esp32/esp32_body_ptr_slot_rt.jal" \
  -s "$REPO/sims/shared" -s "$REPO/sims"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: label-leak diagnostics stay quiet on failed and clean compiles"
else
  echo "FAIL: $fails check(s) broke"
fi
exit $(( fails != 0 ))

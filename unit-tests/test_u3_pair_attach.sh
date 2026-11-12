#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# U3 Phase 1: the open-array pair list attached to the CALL command.
#
#   1. the acceptance comparator re-derives the pair set from the call
#      parameters and agrees with the attachments on the corpus witnesses
#      (open-array chunked, string-view, pass-on, multi-pair);
#   2. the comparator is non-vacuous: JALV2_U3_PAIR_FAULT=drop and =alter
#      each make it fail, with a diagnostic and a non-zero exit;
#   3. a program with no CALL commands fails the loud comparator (the
#      "zero calls" guard), so a green footer means the call walk happened.
#
# Usage: unit-tests/test_u3_pair_attach.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib;tests;sims/shared'

compile() {
  # $1 = witness, $2 = log, extra env in the caller
  ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -asm "$WORK/o.asm" \
      -hex "$WORK/o.hex" "$1" ) > "$2" 2>&1
}

# --- 1. agreement on the four pair shapes --------------------------------
for w in sims/picsim/16f18857_picsim_openarray_chunked.jal \
         sims/picsim/16f18857_picsim_string_clone.jal \
         sims/picsim/16f18857_picsim_user_array_passed_on.jal \
         sims/picsim/16f18857_picsim_string_clone_matrix.jal; do
  log="$WORK/$(basename "$w" .jal).log"
  if JALV2_U3_PAIR_COMPARE=1 compile "$w" "$log" \
      && grep -qE '^pair-compare: [0-9]+ calls, [0-9]+ pairs, 0 mismatches$' \
        "$log" \
      && grep -q '^0 errors' "$log"; then
    echo "ok   pair attachment agrees: $(basename "$w")"
  else
    echo "FAIL: pair attachment disagrees: $(basename "$w")"
    tail -5 "$log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
done

# --- 2. the fault hooks prove the comparator catches a wrong list ---------
for fault in drop alter; do
  log="$WORK/fault_$fault.log"
  if JALV2_U3_PAIR_COMPARE=1 JALV2_U3_PAIR_FAULT=$fault \
      compile sims/picsim/16f18857_picsim_string_clone.jal "$log"; then
    echo "FAIL: fault=$fault did not fail the compile"
    fails=$((fails + 1))
  elif grep -q 'pair-compare: .*[1-9][0-9]* mismatches' "$log" \
      && grep -q 'disagree with the call parameters' "$log"; then
    echo "ok   fault=$fault trips the comparator"
  else
    echo "FAIL: fault=$fault failed without the comparator diagnostic"
    tail -5 "$log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
done

# --- 3. the zero-call guard (non-vacuous footer) --------------------------
log="$WORK/zero.log"
if JALV2_U3_PAIR_COMPARE=1 compile \
    third_party/jallib/samples/10f200_blink_intosc.jal "$log"; then
  echo "FAIL: zero-call program passed the loud comparator"
  fails=$((fails + 1))
elif grep -q 'pair-compare: no CALL commands' "$log"; then
  echo "ok   zero-call guard fires"
else
  echo "FAIL: zero-call program failed without the guard diagnostic"
  tail -5 "$log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails pair-attachment check(s)"
  exit 1
fi
echo "ok   U3 pair attachment"

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# byref (c): a multi-byte `at` overlay on a byref formal must read BOTH
# bytes.  `v = v + 1` over `var word v at arr` lowers to a word read-modify-
# write; the pre-fix build loaded only the low byte (one INDF read + a
# zero-extend of the high temp byte).  The fix emits TWO INDF reads in the
# routine body and no zero-extend of the high byte.
#
# Usage: unit-tests/test_byref_rmw_word.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib'

compile() {  # compile <file> <asm> <log>
  ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -asm "$2" -hex "$WORK/o.hex" \
      "$1" ) > "$3" 2>&1
}

cat > "$WORK/rmw.jal" <<'JALEOF'
include 16f887
procedure bump(byref in out arr[]) is
   var word v at arr
   v = v + 1
end procedure
var word z = 0x12FF
bump(z)
JALEOF
if compile "$WORK/rmw.jal" "$WORK/rmw.asm" "$WORK/rmw.capture" \
    && grep -q '^0 errors' "$WORK/rmw.capture"; then
  body=$(sed -n '/^l_bump/,/^[a-z_]*:/p' "$WORK/rmw.asm" | grep -v '^[a-z_]*:')
  reads=$(printf '%s\n' "$body" | grep -c 'movf[[:space:]]*v__ind')
  ze=$(printf '%s\n' "$body" | grep -c 'clrf[[:space:]]*v____temp_.*+1' || true)
  if [ "$reads" -eq 2 ] && [ "$ze" -eq 0 ]; then
    echo "ok   word overlay RMW reads both bytes"
  else
    echo "FAIL: word RMW body reads=$reads zero-extend=$ze"
    printf '%s\n' "$body" | head -20
    fails=$((fails + 1))
  fi
else
  echo "FAIL: compile"; sed 's/^/     /' "$WORK/rmw.capture" | tail -8
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails byref-rmw-word check(s)"
  exit 1
fi
echo "PASS: byref word overlay RMW reads both bytes"

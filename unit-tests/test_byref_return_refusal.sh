#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# `return byref` is deliberately NOT implemented (the caller-side auto-
# dereference does not survive the return marshalling -- TODO follow-up (a)).
# A use of it must be ONE actionable error naming the alternatives, not a
# parse cascade.  A normal `return <expr>` stays unchanged.
#
# Usage: unit-tests/test_byref_return_refusal.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib'

compile() {  # compile <file> <log>
  ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -hex "$WORK/o.hex" \
      "$1" ) > "$2" 2>&1
}

# --- return byref arr: one clean error -------------------------------------
cat > "$WORK/byref.jal" <<'JALEOF'
include 16f887
function get_word(byref in out arr[]) return word is
   return byref arr
end function
var word z = 0
forever loop end loop
JALEOF
compile "$WORK/byref.jal" "$WORK/byref.capture"
if grep -q '`return byref` is not implemented' "$WORK/byref.capture" \
    && [ "$(grep -c 'error' "$WORK/byref.capture")" -eq 1 ]; then
  echo "ok   return byref refuses with one actionable error"
else
  echo "FAIL: return byref refusal"; cat "$WORK/byref.capture"
  fails=$((fails + 1))
fi

# --- normal return still compiles ------------------------------------------
cat > "$WORK/normal.jal" <<'JALEOF'
include 16f887
function get_word(byte in b) return word is
   var word w
   w = b
   return w
end function
var word z = 0
z = get_word(z)
forever loop end loop
JALEOF
if compile "$WORK/normal.jal" "$WORK/normal.capture" \
    && grep -q '^0 errors' "$WORK/normal.capture"; then
  echo "ok   a normal return value still compiles"
else
  echo "FAIL: normal return"; cat "$WORK/normal.capture"
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails byref-return-refusal check(s)"
  exit 1
fi
echo "PASS: return byref refusal"

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# `at <string-var>` overlays the string's HANDLE (fixed RAM) + offset, never
# its heap payload (a runtime address), so a byte overlay silently read/wrote
# the wrong bytes.  Reject it with ONE actionable error per declaration (the
# subscript and bare forms each count once).  A byref formal over a string
# aliases the PAYLOAD and stays legal -- that is the documented path.
#
# Usage: unit-tests/test_at_string_overlay_reject.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib;sims/shared'

compile() {  # compile <file> <log>
  ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -hex "$WORK/o.hex" \
      "$1" ) > "$2" 2>&1
}

# --- direct string overlay: one clean error per declaration ---------------
cat > "$WORK/direct.jal" <<'JALEOF'
include regression_18f4620_device
include regression
pragma target string_heap_size 64
var string s
var volatile byte c at s[0]
var volatile byte d at s
forever loop end loop
JALEOF
compile "$WORK/direct.jal" "$WORK/direct.capture"
if grep -q "'at' cannot overlay a string variable" "$WORK/direct.capture" \
    && [ "$(grep -c "'at' cannot overlay a string variable" "$WORK/direct.capture")" -eq 2 ] \
    && grep -q '^2 errors' "$WORK/direct.capture"; then
  echo "ok   at-over-string refused (subscript + bare, two clean errors)"
else
  echo "FAIL: at-over-string refusal"; cat "$WORK/direct.capture"
  fails=$((fails + 1))
fi

# --- byref formal over a string still compiles (no false positive) --------
cat > "$WORK/byref.jal" <<'JALEOF'
include regression_18f4620_device
include regression
pragma target string_heap_size 64
var string s
var volatile byte ok = 0
procedure poke(byref in out x[]) is
   var byte c0 at x[0]
   c0 = 0x5A
   ok = x[0]
end procedure
poke(s)
forever loop end loop
JALEOF
compile "$WORK/byref.jal" "$WORK/byref.capture"
if grep -q '^0 errors' "$WORK/byref.capture"; then
  echo "ok   at-over-byref-formal (string payload) still compiles"
else
  echo "FAIL: byref string overlay should compile"; cat "$WORK/byref.capture"
  fails=$((fails + 1))
fi

exit $fails

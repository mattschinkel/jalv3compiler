#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Open-array layouts wider than the runtime chunk map.
#
# A formal that sees TWO different layouts needs a runtime chunk map.  The map
# holds PIC_CHUNK_MAP_MAX (8) chunks; when one call site's actual is chunked
# WIDER than that, the compiler used to fall through with no map at all and
# the callee addressed the other call site through the first call's baked
# layout -- 0 errors, wrong image (found 2026-11-09; reproduces on the
# pre-session compiler 420ea0f, so it is not a regression of this work).
#
# The fix refuses at the decision point: when a formal has more than one call
# site AND one actual's placement is wider than the map, a clear error is
# emitted instead of a silently wrong image.  A SINGLE over-wide actual is
# still fine (nothing to differ from, the baked layout is exact), and a
# representable two-layout case still compiles.
#
# Usage: unit-tests/test_openarray_pair_over_cap.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC=(-s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" -s "$REPO/sims/shared")

# 1. the defect witness: a 1200-byte (16 chunks) and a 500-byte (7 chunks)
#    array through one formal -> must be REFUSED with the map message
cat > "$WORK/two.jal" <<'EOF'
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED

var byte a[1200]
var byte b[500]
var volatile byte ok_a = 0
var volatile byte ok_b = 0

procedure walk(byte in out d[], byte in n) is
   d[n] = 7
end procedure

  a[0] = 1
  b[0] = 2
  walk(a, 200)
  walk(b, 100)
  ok_a = a[200]
  ok_b = b[100]
EOF
( cd "$WORK" && "$CC" "${INC[@]}" two.jal >two.log 2>&1 )
rc_two=$?
if [ $rc_two -ne 0 ] \
    && grep -q "the runtime chunk map holds 8 chunks but open-array formal 'd'" \
        "$WORK/two.log" \
    && [ "$(grep -c 'the runtime chunk map holds 8 chunks' "$WORK/two.log")" -eq 1 ]; then
  echo "ok   two layouts, one wider than the map: refused with one clear diagnostic"
else
  echo "FAIL: the over-cap two-layout case was not refused (rc=$rc_two)"
  tail -4 "$WORK/two.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# 2. a SINGLE over-wide actual must still compile (its baked layout is exact)
cat > "$WORK/one.jal" <<'EOF'
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED

var byte big[1200]
var volatile byte sink = 0

procedure walk(byte in out d[], byte in n) is
   d[n] = 7
end procedure

  big[0] = 1
  walk(big, 200)
  sink = big[200]
EOF
( cd "$WORK" && "$CC" "${INC[@]}" one.jal >one.log 2>&1 )
if [ $? -eq 0 ] && grep -q "0 errors" "$WORK/one.log"; then
  echo "ok   a single over-wide actual still compiles (no false refusal)"
else
  echo "FAIL: the single over-wide actual was refused"
  tail -3 "$WORK/one.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# 3. a REPRESENTABLE two-layout case still compiles (both <= 8 chunks)
cat > "$WORK/two_ok.jal" <<'EOF'
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED

var byte a[300]
var byte b[200]
var volatile byte ok_a = 0
var volatile byte ok_b = 0

procedure walk(byte in out d[], byte in n) is
   d[n] = 7
end procedure

  a[0] = 1
  b[0] = 2
  walk(a, 200)
  walk(b, 100)
  ok_a = a[200]
  ok_b = b[100]
EOF
( cd "$WORK" && "$CC" "${INC[@]}" two_ok.jal >two_ok.log 2>&1 )
if [ $? -eq 0 ] && grep -q "0 errors" "$WORK/two_ok.log"; then
  echo "ok   a representable two-layout formal still compiles"
else
  echo "FAIL: a representable two-layout formal was refused"
  tail -3 "$WORK/two_ok.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# 4. the existing 150-byte passed-on witness is untouched
cp "$REPO/sims/picsim/16f18857_picsim_user_array_passed_on.jal" "$WORK/"
( cd "$WORK" && "$CC" "${INC[@]}" 16f18857_picsim_user_array_passed_on.jal >po.log 2>&1 )
if [ $? -eq 0 ] && grep -q "0 errors" "$WORK/po.log"; then
  echo "ok   the 150-byte passed-on witness still compiles"
else
  echo "FAIL: the passed-on witness regressed"
  tail -3 "$WORK/po.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

echo
if [ "$fails" -eq 0 ]; then
  echo "RESULT: PASS"
else
  echo "RESULT: FAIL ($fails checks failed)"
fi
exit $((fails ? 1 : 0))

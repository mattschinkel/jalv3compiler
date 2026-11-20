#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# The JALv2 byte-array idiom (`const byte s[] = "..."`, fixed strings) still
# passes into an open-array `byte in arr[]` formal; a DYNAMIC (heap) string
# does not -- it is refused with a pointer to `byref in arr[]`.  The print
# library takes strings via `byref in arr[]` now, which accepts both.
#
# Usage: unit-tests/test_string_array_boundary.sh [compiler]
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

# --- const byte[] (JALv2 idiom) into byte in arr[]: OK --------------------
cat > "$WORK/const.jal" <<'JALEOF'
include 16f887
const byte s[] = "ABC"
procedure x(byte in arr[]) is
   var byte n
   n = byte(COUNT(arr))
end procedure
x(s)
forever loop end loop
JALEOF
if compile "$WORK/const.jal" "$WORK/const.capture" \
    && grep -q '^0 errors' "$WORK/const.capture"; then
  echo "ok   const byte[] into byte in arr[] compiles"
else
  echo "FAIL: const byte[]"; cat "$WORK/const.capture"
  fails=$((fails + 1))
fi

# --- dynamic string into byte in arr[]: one clean error ------------------
cat > "$WORK/dyn.jal" <<'JALEOF'
include 16f887
pragma target string_heap_size 64
var string s = "ABC"
procedure x(byte in arr[]) is
   var byte n
   n = byte(COUNT(arr))
end procedure
x(s)
forever loop end loop
JALEOF
compile "$WORK/dyn.jal" "$WORK/dyn.capture"
if grep -Fq 'use `byref in arr[]`' "$WORK/dyn.capture" \
    && [ "$(grep -c 'error' "$WORK/dyn.capture")" -eq 1 ]; then
  echo "ok   dynamic string into byte in arr[] refuses cleanly"
else
  echo "FAIL: dynamic string refusal"; cat "$WORK/dyn.capture"
  fails=$((fails + 1))
fi

# --- dynamic string into byref in arr[]: OK ------------------------------
cat > "$WORK/byref.jal" <<'JALEOF'
include 16f887
pragma target string_heap_size 64
var string s = "ABC"
procedure x(byref in arr[]) is
   var byte n
   n = byte(COUNT(arr))
end procedure
x(s)
forever loop end loop
JALEOF
if compile "$WORK/byref.jal" "$WORK/byref.capture" \
    && grep -q '^0 errors' "$WORK/byref.capture"; then
  echo "ok   dynamic string into byref in arr[] compiles"
else
  echo "FAIL: byref string"; cat "$WORK/byref.capture"
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails string-array-boundary check(s)"
  exit 1
fi
echo "PASS: string-array boundary"

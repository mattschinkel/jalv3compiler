#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# An OPEN bit-array formal (`bit in out x[]`, `bit out x[]`) is a pointer to
# the actual's packed bits: the call-site normalize copy hands READS over,
# but the callee's writes are never copied back, so the shape silently lost
# every write (a fixed-size `bit in out x[N]` formal is a real array and does
# copy).  The declaration is refused now; he IN-only open form (the
# print-bit-array idiom) and fixed-size forms stay legal.
#
# Usage: unit-tests/test_open_bit_out_refused.sh [path-to-jalv2]
set -u

COMPILER="${1:-./bin/beta/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LIBS="$ROOT/demo_lib;$ROOT/third_party/jallib/lib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

check_ok() { # name source
  local name="$1" src="$2"
  local body="$TMP/$name.jal" out rc
  printf '%s\n' "$src" > "$body"
  out="$("$COMPILER" -s "$LIBS" -no-codfile -no-hex "$body" 2>&1)"
  rc=$?
  if [ $rc -eq 0 ] && printf '%s\n' "$out" | tail -1 \
      | grep -Eq '^0 errors, [0-9]+ warnings?$'; then
    pass=$((pass + 1)); echo "PASS $name (control)"
  else
    fail=$((fail + 1)); echo "FAIL $name (control; rc=$rc)"
    printf '%s\n' "$out" | tail -4
  fi
}

check_err() { # name source wanted substring
  local name="$1" src="$2" want="${3:-}"
  local body="$TMP/$name.jal" out rc
  printf '%s\n' "$src" > "$body"
  out="$("$COMPILER" -s "$LIBS" -no-codfile -no-hex "$body" 2>&1)"
  rc=$?
  if [ $rc -eq 1 ] && printf '%s\n' "$out" | tail -1 \
      | grep -Eq '^[0-9]+ errors?, [0-9]+ warnings?$' \
      && { [ -z "$want" ] || printf '%s\n' "$out" | grep -qF "$want"; }; then
    pass=$((pass + 1)); echo "PASS $name"
  else
    fail=$((fail + 1)); echo "FAIL $name (rc=$rc)"
    printf '%s\n' "$out" | tail -4
  fi
}

check_err open_bit_inout '
include 18f4620
var bit fb[8]
procedure wr(bit in out x[]) is
   x[0] = true
end procedure
wr(fb)' "writes would not be copied back"
check_err open_bit_out '
include 18f4620
var bit fb[8]
procedure wr(bit out x[]) is
   x[0] = true
end procedure
wr(fb)' "writes would not be copied back"
check_ok open_bit_in '
include 18f4620
var bit fb[8]
function rd(bit in x[]) return bit is
   return x[1]
end function
var bit r
r = rd(fb)'
check_ok fixed_bit_inout '
include 18f4620
var bit fb[8]
procedure wr(bit in out x[8]) is
   x[0] = true
end procedure
wr(fb)'
check_ok fixed_bit_out '
include 18f4620
var bit fb[8]
procedure wr(bit out x[8]) is
   x[0] = true
end procedure
wr(fb)'
check_ok scalar_bit_out '
include 18f4620
var bit b
procedure wr(bit out x) is
   x = true
end procedure
wr(b)'

echo
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]

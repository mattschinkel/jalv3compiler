#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# A 'put/'get ACCESSOR used inside its own body is refused.
#
#   procedure slot 'put(byte in v) is
#      slot = v            -- the name resolves back to this accessor
#   end procedure
#
# The assignment/read then compiles to a CALL of the accessor from inside it:
# an unbounded hardware-stack recursion with no other symptom (gpsim reports
# "Reset on Stack overflow" and the RAM looks untouched -- found while writing
# the put_parms witness, where the first probe did exactly this).  The same
# holds for the read side (`function slot 'get ... is return slot`).
#
# Both are refused now, with ONE diagnostic.  The working shape is a backing
# variable the accessor stores to/reads from, and that shape (plus the
# ordinary `slot = 5` call from OUTSIDE the body) stays legal -- the guard
# compares the accessor's own PROC with the procedure being parsed, so an
# ordinary call site is untouched.
#
# Usage: unit-tests/test_accessor_self_recursion.sh [path-to-jalv3]
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

check_err1() { # name source wanted substring -- EXACTLY one error
  local name="$1" src="$2" want="$3"
  local body="$TMP/$name.jal" out rc
  printf '%s\n' "$src" > "$body"
  out="$("$COMPILER" -s "$LIBS" -no-codfile -no-hex "$body" 2>&1)"
  rc=$?
  if [ $rc -eq 1 ] \
      && printf '%s\n' "$out" | tail -1 | grep -Eq '^1 errors, [0-9]+ warnings?$' \
      && printf '%s\n' "$out" | grep -qF "$want"; then
    pass=$((pass + 1)); echo "PASS $name"
  else
    fail=$((fail + 1)); echo "FAIL $name (rc=$rc)"
    printf '%s\n' "$out" | tail -5
  fi
}

# --- refusals -----------------------------------------------------------------

check_err1 self_put '
include 18f4620
var byte slot
procedure slot '"'"'put(byte in v) is
   slot = v
end procedure
procedure run() is
   slot = 5
end procedure
run()' "would recurse forever"

check_err1 self_get '
include 18f4620
var byte back
function slot '"'"'get return byte is
   return slot
end function
procedure run() is
   back = slot
end procedure
run()' "would recurse forever"

# --- the working shapes stay legal --------------------------------------------

# a backing variable the accessor stores to / reads from
check_ok backing_variable '
include 18f4620
var byte back
procedure slot '"'"'put(byte in v) is
   back = v
end procedure
function slot '"'"'get return byte is
   return back
end function
procedure run() is
   slot = 5
   back = slot
end procedure
run()'

# the accessor called from ANOTHER procedure must not trip the guard
check_ok called_from_outside '
include 18f4620
var byte back
procedure slot '"'"'put(byte in v) is
   back = v
end procedure
procedure set_it(byte in v) is
   slot = v
end procedure
procedure run() is
   set_it(7)
end procedure
run()'

echo
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]

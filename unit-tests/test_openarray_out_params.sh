#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# OUT / IN OUT open-array formals (`byte out a[]`, `byte in out a[]`):
# the call passes the ARRAY ADDRESS plus the hidden _a_count, the callee
# writes through it and there is no copy-back.  Pins the acceptance rules
# (a fixed array binds; a constant does not) and the COUNT() slot.
#
# Usage: unit-tests/test_openarray_out_params.sh [path-to-jalv2]
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
  out="$("$COMPILER" -s "$LIBS" "$body" 2>&1)"
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
  out="$("$COMPILER" -s "$LIBS" "$body" 2>&1)"
  rc=$?
  if [ $rc -eq 1 ] && printf '%s\n' "$out" | tail -1 \
      | grep -Eq '^[0-9]+ errors?, [0-9]+ warnings?$' \
      && { [ -z "$want" ] || printf '%s\n' "$out" | grep -qF "$want"; }; then
    pass=$((pass + 1)); echo "PASS $name"
  else
    fail=$((fail + 1)); echo "FAIL $name (rc=$rc; wanted: ${want:-summary only})"
    printf '%s\n' "$out" | tail -4
  fi
}

DEV='include 16f877'

# --- OUT-only open array: write-through plus COUNT() of the hidden count ---
check_ok out_write "$DEV
procedure p(byte out a[]) is
   a[0] = 7
   a[1] = 8
end procedure
var byte b[4]
p(b)"

check_ok out_count "$DEV
procedure p(byte out a[]) is
   a[0] = COUNT(a)
end procedure
var byte b[4]
p(b)"

# --- IN OUT: read the caller's values and write back ----------------------
check_ok inout_readwrite "$DEV
procedure p(byte in out a[]) is
   a[1] = a[0] + 1
end procedure
var byte b[4]
b[0] = 5
p(b)"

# --- an OUT formal passed on to another OUT formal -----------------------
check_ok out_passed_on "$DEV
procedure q(byte out a[]) is
   a[0] = 1
end procedure
procedure p(byte out a[]) is
   q(a)
end procedure
var byte b[4]
p(b)"

# --- a non-array actual is still refused --------------------------------
check_err out_scalar_actual "$DEV
procedure p(byte out a[]) is
   a[0] = 1
end procedure
var byte v
p(v)" "type mismatch"

# --- a constant/ROM array destination is refused ------------------------
check_err out_const_actual "$DEV
procedure p(byte out a[]) is
   a[0] = 1
end procedure
const byte t[4] = {1, 2, 3, 4}
p(t)" "cannot be a constant"

echo
echo "passed: $pass  failed: $fail"
[ $fail -eq 0 ]

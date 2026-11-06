#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# A user POINTER actual bound to an `in out` / `out` open-array formal
# (fixes/fix_openarray_pointer_store.md): the formal stores the ADDRESS VALUE
# and the back end's linear pointer store writes the element through it.
# Pins the accepted element widths and the remaining refusals (a const
# pointee cannot be a writable actual; a scalar is not an array).
#
# Usage: unit-tests/test_openarray_pointer_store.sh [path-to-jalv2]
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

check_ok ptr_inout_byte '
include 18f4620
var byte arr[4]
var pointer p as var byte
procedure wr(byte in out x[]) is
   x[0] = 1
end procedure
p is arr[0]
wr(p)'
check_ok ptr_out_byte '
include 18f4620
var byte arr[4]
var pointer p as var byte
procedure wr(byte out x[]) is
   x[0] = 1
end procedure
p is arr[0]
wr(p)'
check_ok ptr_inout_word '
include 18f4620
var word arr[4]
var pointer p as var word
procedure wr(word in out x[]) is
   x[1] = 0x1234
end procedure
p is arr[0]
wr(p)'
check_ok ptr_inout_dword '
include 18f4620
var dword arr[3]
var pointer p as var dword
procedure wr(dword in out x[]) is
   x[1] = 0x11223344
end procedure
p is arr[0]
wr(p)'
check_ok ptr_inout_qword '
include 18f4620
var qword arr[2]
var pointer p as var qword
procedure wr(qword in out x[]) is
   x[1] = 0x1122334455667788
end procedure
p is arr[0]
wr(p)'
check_ok ptr_inout_qword_32bit '
include 32mx270f256b
pragma target image pfm
var qword arr[2]
var pointer p as var qword
procedure wr(qword in out x[]) is
   x[1] = 0x1122334455667788
end procedure
p is arr[0]
wr(p)'
check_ok ptr_inout_float '
include 18f4620
var float arr[2]
var pointer p as var float
procedure wr(float in out x[]) is
   x[1] = 2.5
end procedure
p is arr[0]
wr(p)'
check_ok ptr_in_read '
include 18f4620
var byte arr[4]
var pointer p as var byte
function rd(byte in x[]) return byte is
   return x[1]
end function
p is arr[0]
arr[1] = 5
if rd(p) == 5 then
end if'
check_ok array_inout_control '
include 18f4620
var byte arr[4]
procedure wr(byte in out x[]) is
   x[0] = 1
end procedure
wr(arr)'
check_err ptr_inout_const '
include 18f4620
const byte rom[4] = {1, 2, 3, 4}
var pointer p as const byte
procedure wr(byte in out x[]) is
   x[0] = 1
end procedure
p is rom[0]
wr(p)' "type mismatch"
check_err ptr_inout_scalar '
include 18f4620
var byte b = 0
procedure wr(byte in out x[]) is
   x[0] = 1
end procedure
wr(b)' "type mismatch"

echo
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]

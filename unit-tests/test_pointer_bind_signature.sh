#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the pointer-bind SIGNATURE rule (fix_pointer_bind_signature).
#
# A pointer is bound only to a target of the SAME signature:
#   - a SCALAR pointee (`as var byte`) takes a `var byte` variable, an
#     element of that type, or a scalar pseudo-variable ('get/'put);
#   - an ARRAY pointee (`as var byte[N]`) takes an array of that shape;
#   - a STRING pointee (`as var string`, `as var string[N]`) takes a
#     dynamic or fixed string of that shape.
# Before this, the check compared a scalar pointee with the source's
# ELEMENT, so `var pointer p as var byte` accepted a `var string`, a
# byte array and a const array. The emitter then stored the array (or the
# dynamic string's 3-byte handle) address, so `x = p` read the handle and
# `p = p + 1` overwrote it -- silently wrong code behind a clean compile.
#
# Usage: unit-tests/test_pointer_bind_signature.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
ncase=0

COMMON='
include 16f1825
pragma target clock 20_000_000
pragma target string_heap_size 15
include strings_dynamic
var string v_dyn
var string v_fixed[8]
var byte v_byte
var word v_word
var byte v_arr[4]
var word v_warr[4]
const byte v_tbl[4] = {1, 2, 3, 4}
var byte i
var pointer p_b as var byte
var pointer p_b2 as var byte
var pointer p_w as var word
var pointer p_s as var string
var pointer p_sf as var string[8]
var pointer p_a as var byte[4]
var pointer p_ca as const byte[4]
var pointer p_wa as var word[4]

record v_rec is
   byte bfield
   word wfield
end record
var v_rec v_eye

function pv'"'"'get() return byte is
   return v_byte
end function
procedure pv'"'"'put(byte in x) is
   v_byte = x
end procedure
'

mk() { # name body -> path
  local name=$1 body=$2
  printf '%s\n%s\n' "$COMMON" "$body" > "$WORK/$name.jal"
  printf '%s\n' "$WORK/$name.jal"
}

expect_ok() { # name file
  local name=$1 file=$2 out ncd
  ncase=$((ncase + 1))
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" \
        -hex "$WORK/$ncase.hex" "$file" 2>&1)
  if printf '%s\n' "$out" | grep -q "0 errors"; then
    echo "ok   $name"
  else
    echo "FAIL $name: expected a clean compile, got:"
    printf '%s\n' "$out" | grep "error" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

expect_err() { # name file
  local name=$1 file=$2 out
  ncase=$((ncase + 1))
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" \
        -hex "$WORK/$ncase.hex" "$file" 2>&1)
  if printf '%s\n' "$out" | grep -q "type mismatch"; then
    echo "ok   $name (refused: type mismatch)"
  else
    echo "FAIL $name: expected a 'type mismatch' refusal, got:"
    printf '%s\n' "$out" | tail -3 | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# --- accepted: same signature, elements, and pseudo-variables ----------------
expect_ok "var byte -> as var byte"      "$(mk ok_byte 'p_b is v_byte')"
expect_ok "byte element, const index"    "$(mk ok_elem 'p_b is v_arr[2]')"
expect_ok "byte element, runtime index"  "$(mk ok_elem_rt 'p_b is v_arr[i]')"
expect_ok "scalar pseudo-variable"       "$(mk ok_pseudo 'p_b is pv')"
expect_ok "pointer copy (both bound)"    "$(mk ok_copy 'p_b is v_byte
p_b2 is p_b')"
expect_ok "as var byte[4] <- byte[4]"    "$(mk ok_array 'p_a is v_arr')"
expect_ok "as const byte[4] <- const"    "$(mk ok_carray 'p_ca is v_tbl')"
expect_ok "as var string <- dynamic"     "$(mk ok_dstr 'p_s is v_dyn')"
expect_ok "as var string[8] <- fixed"    "$(mk ok_fstr 'p_sf is v_fixed')"
expect_ok "byte record field"            "$(mk ok_rec 'p_b is v_eye.bfield')"

# --- refused: an array/string is not a variable of the pointee signature ----
expect_err "as var byte <- dynamic string" "$(mk bad_dyn 'p_b is v_dyn')"
expect_err "as var byte <- fixed string"   "$(mk bad_fixed 'p_b is v_fixed')"
expect_err "as var byte <- byte array"     "$(mk bad_arr 'p_b is v_arr')"
expect_err "as var byte <- const array"    "$(mk bad_carr 'p_b is v_tbl')"
expect_err "as var byte <- word array"     "$(mk bad_warr 'p_b is v_warr')"
expect_err "as var byte <- word scalar"    "$(mk bad_word 'p_b is v_word')"
expect_err "as var byte <- word field"     "$(mk bad_rec 'p_b is v_eye.wfield')"
expect_err "as var word <- byte scalar"    "$(mk bad_w2b 'p_w is v_byte')"
expect_err "as var string <- byte scalar"  "$(mk bad_s2b 'p_s is v_byte')"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all pointer-bind signature pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

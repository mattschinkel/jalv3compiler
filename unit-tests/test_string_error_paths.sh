#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Deterministic negative tests for the string assign/concat error paths
# (fixes/fix_string_error_path_free_corruption.md) plus the dynamic-string
# type/syntax gates (fixes/fix_dynamic_string_syntax_safety.md).
#
# PASS per case: exit code 1 AND a final "N errors, M warnings" line AND,
# when a substring is supplied, that diagnostic is present.
# FAIL: any other exit code (crash/abort/signal), a missing summary line, or
# a missing expected diagnostic -- the same rule unittests.py applies.
#
# Usage: unit-tests/test_string_error_paths.sh [path-to-jalv2]
set -u

COMPILER="${1:-./bin/beta/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LIBS="$ROOT/demo_lib;$ROOT/third_party/jallib/lib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

check() { # name source [wanted diagnostic substring]
  local name="$1" src="$2" want="${3:-}"
  local body="$TMP/$name.jal"
  local out rc
  printf '%s\n' "$src" > "$body"
  out="$("$COMPILER" -s "$LIBS" "$body" 2>&1)"
  rc=$?
  if [ $rc -eq 1 ] && printf '%s\n' "$out" | tail -1 \
      | grep -Eq '^[0-9]+ errors?, [0-9]+ warnings?$' \
      && { [ -z "$want" ] || printf '%s\n' "$out" | grep -qF "$want"; }; then
    pass=$((pass + 1))
    echo "PASS $name"
  else
    fail=$((fail + 1))
    echo "FAIL $name (rc=$rc; wanted: ${want:-summary only})"
    printf '%s\n' "$out" | tail -5
  fi
}

check_ok() { # name source
  local name="$1" src="$2"
  local body="$TMP/$name.jal"
  local out rc
  printf '%s\n' "$src" > "$body"
  out="$("$COMPILER" -s "$LIBS" "$body" 2>&1)"
  rc=$?
  if [ $rc -eq 0 ] && printf '%s\n' "$out" | tail -1 \
      | grep -Eq '^0 errors, [0-9]+ warnings?$'; then
    pass=$((pass + 1))
    echo "PASS $name (control)"
  else
    fail=$((fail + 1))
    echo "FAIL $name (control; rc=$rc)"
    printf '%s\n' "$out" | tail -5
  fi
}

# --- heap-string configured.  Stage 2a/2b made INTEGER operands legal
# ("s = \"abc\" & n" now compiles); the remaining error shapes are
# floats, bits and byte arrays.
HEAP='include 16f877
pragma target string_heap_size 128
include strings_dynamic
var string s'

# stage 2d v1: float CONSTANTS are legal; runtime float variables
# still error (cleanly, with the dedicated message).
check float_var        "$HEAP
var float f = 1.0
s = \"abc\" & f"
check bit_var          "$HEAP
var bit b = true
s = \"abc\" & b"

# const literal that cannot fit the heap (fix_pointer_expr_deref_assign /
# user 16f18857 report follow-up): header (3 * index width) + payload must
# fit _string_heap_size at compile time. Both assign forms are pinned.
check heap_undersized_stmt '
include 16f877
pragma target string_heap_size 9
include strings_dynamic
var string s
s = "My string"'
check heap_undersized_decl '
include 16f877
pragma target string_heap_size 9
include strings_dynamic
var string s = "My string"'

# --- dynamic-string syntax safety: comparisons ------------------------
# A scalar on the right of a dynamic-string relation used to reach the
# byte walker and assert in value_dereference.  It is a clean type error.
check eq_dyn_int "$HEAP
if s == 5 then
s = \"x\"
end if" "cannot compare a string with a non-string value"
check ne_dyn_int "$HEAP
if s != 5 then
s = \"x\"
end if" "cannot compare a string with a non-string value"
check eq_dyn_byte "$HEAP
var byte b = 5
if s == b then
s = \"x\"
end if" "cannot compare a string with a non-string value"
check eq_dyn_pointer "$HEAP
var byte b = 1
var pointer p as var byte
p is b
if s == p then
s = \"x\"
end if" "cannot compare a string with a non-string value"
check eq_int_dyn "$HEAP
if 5 == s then
s = \"x\"
end if" "cannot compare a string with a non-string value"
check eq_literal_int "$HEAP
if \"ab\" == 5 then
s = \"x\"
end if" "cannot compare a string with a non-string value"
check eq_int_literal "$HEAP
if 5 == \"ab\" then
s = \"x\"
end if" "cannot compare a string with a non-string value"
check eq_fixed_int "$HEAP
var string[8] fx
fx = \"x\"
if fx == 5 then
s = \"x\"
end if" "cannot compare a string with a non-string value"

# --- truncating helpers: max_len must be an integer -------------------
check truncate_string_max "$HEAP
var bit r
r = _string_truncate(s, \"ab\")" "integer expression expected for max_len"
check truncate_conststr_max "$HEAP
const string c = \"ab\"
var bit r
r = _string_truncate(s, c)" "integer expression expected for max_len"
check truncate_float_max "$HEAP
var float fv = 2.0
var bit r
r = _string_truncate(s, fv)" "integer expression expected for max_len"

# --- string used as a subscript / case selector -----------------------
check subscript_dynamic "$HEAP
var string idx = \"a\"
var byte a[4] = \"abcd\"
var byte b
b = a[idx]" "string value cannot be used as a subscript"
check subscript_fixed "$HEAP
var string[8] fx
fx = \"a\"
var byte a[4] = \"abcd\"
var byte b
b = a[fx]" "string value cannot be used as a subscript"
check case_dynamic "$HEAP
case s of
  \"a\": s = \"b\"
end case" "case selector must be an integer"

# --- character stores through a string pointer (2026-11-04) -----------
# `p[i] = c` is a byte store routed through the handle; a multi-byte string
# RHS is not a character (same rule as the direct `s[i] = c` form).
check ptr_store_multibyte "$HEAP
var pointer p as var string
s = \"abc\"
p is s
p[1] = \"text\"" "string element assignment requires a byte value"

# --- heap-size declaration validation ---------------------------------
check heap_word_overflow '
include 16f877
pragma target string_heap_size 65599
var string s' "dynamic string heap is currently limited to 65535 bytes"
check heap_float_const '
include 16f877
pragma target string_heap_size 64.5
var string s' "_string_heap_size must be a constant integer"
check heap_negative_pragma '
include 16f877
pragma target string_heap_size -64
var string s' "_string_heap_size must not be negative"

# --- to_array destination writability ---------------------------------
# The outlined forwarder writes through an `in` byte-array formal, so the
# call machinery would bind actuals an `out` formal refuses.  A const/ROM
# destination must keep its diagnostic: the outline falls back to the
# inline path, which reports `assign to constant`.
check to_array_const_dst '
include 16f877
pragma target string_heap_size 64
include strings_dynamic
const byte rom[4] = {1, 2, 3, 4}
var string s
var bit r
s = "ab"
r = string_to_array(rom, s)' "assign to constant"

# --- string actual into out / in out array formals (2026-11-05) ---------
# The OUT direction marshals nothing (the snapshot lives in the IN parser),
# so the callee would write through the string's handle and silently corrupt
# the heap.  Refused until the payload view (fixes/fix_string_arg_view.md).
check out_string_refused '
include 16f877
pragma target string_heap_size 64
var string s
procedure pv_w(byte out x[]) is
  x[0] = 1
end procedure
s = "ab"
pv_w(s)' "writes through it would corrupt the string"
check inout_string_refused '
include 16f877
pragma target string_heap_size 64
var string s
procedure pv_w(byte in out x[]) is
  x[0] = 1
end procedure
s = "ab"
pv_w(s)' "writes through it would corrupt the string"
check inout_ptr_actual_still_mismatched '
include 16f877
pragma target string_heap_size 64
var string s
var pointer p as var string
procedure pv_w(byte in out x[]) is
  x[0] = 1
end procedure
s = "ab"
p is s
pv_w(p)' "type mismatch"
# the view exists only into a callee that cannot move the string: a
# reallocating callee keeps the refusal (no snapshot on the OUT path)
check inout_tainted_refused '
include 18f4620
pragma target string_heap_size 64
var string s
var string t
procedure grower(byte in out x[]) is
  s = s & "!"
  x[0] = 1
end procedure
s = "abcde"
t = "hello"
grower(t)' "writes through it would corrupt the string"
# a CLASSIC 14-bit core has no FSR0 walk, so the view is refused there even
# into a clean callee (the kill-criterion fallback)
check inout_classic_core_refused '
include 16f877
pragma target string_heap_size 64
var string s
procedure pv_w(byte in out x[]) is
  x[0] = 1
end procedure
s = "ab"
pv_w(s)' "writes through it would corrupt the string"
check_ok ok_out_array_actual '
include 16f877
pragma target string_heap_size 64
var string s
var byte b[4]
procedure pv_w(byte out x[]) is
  x[0] = 1
end procedure
s = "ab"
pv_w(b)'

# --- legal controls: the gates above must not over-reject ---------------
check_ok ok_dynamic_literal "$HEAP
s = \"x\"
if s == \"x\" then
s = \"y\"
end if"
check_ok ok_dynamic_fixed "$HEAP
var string[8] fx
fx = \"x\"
if s == fx then
s = \"y\"
end if"
check_ok ok_char_literal_byte "$HEAP
var byte c = 97
if \"a\" == c then
c = 1
end if"
check_ok ok_numeric_assign "$HEAP
s = 42"
check_ok ok_one_char_max "$HEAP
var bit r
r = _string_truncate(s, \"a\")"
check_ok ok_byte_index "$HEAP
var byte i = 1
var byte a[4] = \"abcd\"
var byte b
b = a[i]"
check_ok ok_to_array_writable '
include 16f877
pragma target string_heap_size 64
include strings_dynamic
var string s
var byte b[4]
var bit r
s = "ab"
r = string_to_array(b, s)'
check_ok ok_byte_case "$HEAP
var byte i = 1
case i of
  1: i = 0
end case"
check_ok ok_ptr_store_byte "$HEAP
var pointer p as var string
s = \"abc\"
p is s
p[1] = 88"
check_ok ok_ptr_store_char "$HEAP
var pointer p as var string
s = \"abc\"
p is s
p[1] = \"X\""
check_ok ok_ptr_store_runtime "$HEAP
var pointer p as var string
var byte i = 1
var byte c = 88
s = \"abc\"
p is s
p[i] = c"
check_ok ok_ptr_whole_string "$HEAP
var pointer p as var string
s = \"abc\"
p is s
p = \"wxyz\""

echo
echo "passed: $pass  failed: $fail"
[ $fail -eq 0 ]

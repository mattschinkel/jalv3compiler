#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Deterministic negative tests for the string assign/concat error paths
# (fixes/fix_string_error_path_free_corruption.md).
#
# PASS per case: exit code 1 AND a final "N errors, M warnings" line.
# FAIL: any other exit code (crash/abort/signal) or a missing summary
# line -- the same rule unittests.py applies to samples.
#
# Usage: unit-tests/test_string_error_paths.sh [path-to-jalv2]
set -u

COMPILER="${1:-./bin/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LIBS="$ROOT/demo_lib;$ROOT/demo_lib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

check() {
  local name="$1" src="$2"
  local body="$TMP/$name.jal"
  local out rc
  printf '%s\n' "$src" > "$body"
  out="$("$COMPILER" -s "$LIBS" "$body" 2>&1)"
  rc=$?
  if [ $rc -eq 1 ] && printf '%s\n' "$out" | tail -1 \
      | grep -Eq '^[0-9]+ errors?, [0-9]+ warnings?$'; then
    pass=$((pass + 1))
    echo "PASS $name"
  else
    fail=$((fail + 1))
    echo "FAIL $name (rc=$rc)"
    printf '%s\n' "$out" | tail -5
  fi
}

# --- heap-string configured.  Stage 2a/2b made INTEGER operands legal
# ("s = \"abc\" & n" now compiles); the remaining error shapes are
# floats, bits and byte arrays.
HEAP='include 16f877
const word _string_heap_size = 128
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
check byte_array       "$HEAP
var byte buf[4]
s = \"abc\" & buf"

# const literal that cannot fit the heap (fix_pointer_expr_deref_assign /
# user 16f18857 report follow-up): header (3 * index width) + payload must
# fit _string_heap_size at compile time. Both assign forms are pinned.
check heap_undersized_stmt '
include 16f877
const word _string_heap_size = 9
include strings_dynamic
var string s
s = "My string"'
check heap_undersized_decl '
include 16f877
const word _string_heap_size = 9
include strings_dynamic
var string s = "My string"'

echo
echo "passed: $pass  failed: $fail"
[ $fail -eq 0 ]

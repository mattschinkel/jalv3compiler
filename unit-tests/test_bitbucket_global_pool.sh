#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Regression pin: an empty per-procedure `_bitbucket` costs no RAM.  jalv3
# used to grow every empty bucket to one byte so the file-level gpsim symbol
# existed; that scattered ~58 dead bytes across the network witness.  Only the
# file-level (root proc) bucket needs the byte.  This compiles
# 18f4620_network_all_in_one.jal and requires the number of PLACED `_bitbucket`
# bytes to match jalv25r9 (19: one file-level pool + 18 procedures that
# genuinely pack bits), not the old 77.
#
# Usage: test_bitbucket_global_pool.sh [compiler]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/beta/jalv3-x86-64}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jal_bitbucket_pool-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

case "$CC" in
  /*) ;;
  *) CC="$(cd "$(dirname "$CC")" && pwd)/$(basename "$CC")" ;;
esac
[[ -x "$CC" ]] || { echo "FAIL: compiler not executable: $CC" >&2; exit 1; }

SRC="$ROOT/third_party/jallib/samples/18f4620_network_all_in_one.jal"
LIBS="$ROOT/third_party/jallib/lib;$ROOT/demo_lib"

"$CC" -s "$LIBS" -asm "$WORK/out.asm" -hex "$WORK/out.hex" "$SRC" \
  >"$WORK/compile.log" 2>&1 || {
    cat "$WORK/compile.log" >&2
    echo "FAIL: compilation failed" >&2
    exit 1
  }
grep -q '^0 errors, 0 warnings' "$WORK/compile.log" || {
  cat "$WORK/compile.log" >&2
  echo "FAIL: compiler diagnostics" >&2
  exit 1
}

# Placed `_bitbucket` bytes: distinct physical addresses with a real EQU
# (the empty buckets jalv25r9 skips show up as 0xffffffff in jalv3).
PLACED="$(grep -E '^v__+bitbucket(_[0-9]+)?[[:space:]]+EQU' "$WORK/out.asm" \
  | grep -v '0xffffffff' \
  | grep -oE '0x[0-9a-f]+' | sort -u | wc -l)"
if (( PLACED > 19 )); then
  echo "FAIL: $PLACED placed _bitbucket bytes (expected <= 19)" >&2
  exit 1
fi
echo "OK: $PLACED placed _bitbucket bytes"

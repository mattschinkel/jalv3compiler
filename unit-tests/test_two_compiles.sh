#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# A4 acceptance: a SECOND compile in the same process must produce exactly
# the same output as the first.
#
# This is what the per-module state conversion (fixes/fix_pic_var_state_reset
# .md and the seven module docs beside it) exists for: it drives
# jal_reset_between_compiles(), which zeroes every module's state between
# iterations, and compares the HEX of a single compile against the HEX of the
# LAST of two compiles in one process (JAL_REPEAT=2 emits only the last).
#
# A module whose state survives the reset shows up here as drifted output,
# a crash, or a non-zero exit -- none of which any single-compile gate can
# see. One sample per back end plus a corpus giant, because the back ends
# carry separate state.
#
# Usage: unit-tests/test_two_compiles.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC="$REPO/demo_lib;$REPO/third_party/jallib/lib"

check() { # name file
  local name=$1 file=$2 rc1 rc2
  [ -f "$file" ] || { echo "skip $name (not present)"; return; }
  "$CC" -s "$INC" "$file" -hex "$WORK/once.hex" >"$WORK/once.log" 2>&1
  rc1=$?
  JAL_REPEAT=2 "$CC" -s "$INC" "$file" -hex "$WORK/twice.hex" >"$WORK/twice.log" 2>&1
  rc2=$?
  if [ "$rc1" -ne 0 ] || [ "$rc2" -ne 0 ]; then
    echo "FAIL $name: exit $rc1 (once) / $rc2 (twice)"
    tail -3 "$WORK/twice.log" | sed 's/^/     /'
    fails=$((fails + 1))
  elif ! cmp -s "$WORK/once.hex" "$WORK/twice.hex"; then
    echo "FAIL $name: second compile produced different output"
    echo "     once: $(md5sum < "$WORK/once.hex" | cut -d' ' -f1)"
    echo "     twice: $(md5sum < "$WORK/twice.hex" | cut -d' ' -f1)"
    fails=$((fails + 1))
  else
    echo "ok   $name"
  fi
}

# one per back end, plus the state-heavy shapes
check pic18_float       "$REPO/demo_sample/18f67j50_float_math.jal"
check pic14h_pointers   "$REPO/demo_sample/16f1825_pointers_1.jal"
check pic32_bit_arrays  "$REPO/demo_sample/32mx270f256b_bit_arrays.jal"
check esp32_bit_arrays  "$REPO/demo_sample/esp32_xtensa_lx6_bit_arrays.jal"
check corpus_tcp_client "$REPO/third_party/jallib/samples/18f4620_network_tcp_client.jal"
check corpus_fat32      "$REPO/third_party/jallib/samples/16f18857_glcd_ili9341_fat32_sd_card.jal"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "two-compiles: PASS"
  exit 0
fi
echo "two-compiles: $fails FAILURE(S)"
exit 1

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# P2b (initializer half): an `is`-bound pointer array is a compile-time
# table.  The binding store used to emit a LINEAR symbol+offset fill
# (`movwf v_pa+2k`), which for a chunked array writes past the chunk end:
# every slot beyond the first chunk was never initialized (the 16F18857
# 200-slot probe read v_a0/v_a6's address bytes from the wrong place).
# Const-index pointer-slot stores now resolve the slot address through the
# chunk map and store through INDF, so the table lands in the chunks.
#
# The dereference half (reading pa[i] back) is tracked separately; this pin
# guards the table's PLACEMENT shape on a holey core.
#
# Usage: unit-tests/test_pointer_array_chunk_init.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

{
  echo "include 16f18857"
  echo "var byte a0 = 10"
  echo "var byte a1 = 20"
  echo "var byte a2 = 30"
  echo "var pointer pa[80] as var byte"
  i=0
  while [ "$i" -lt 80 ]; do
    echo "pa[$i] is a$((i % 3))"
    i=$((i + 1))
  done
  echo "forever loop"
  echo "end loop"
} > "$WORK/probe.jal"

( cd "$WORK" && "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" \
    probe.jal > probe.log 2>&1 )
if grep -q "0 errors" "$WORK/probe.log"; then
  echo "ok   chunked pointer array compiles"
else
  echo "FAIL: chunked pointer array did not compile"
  grep -E "error" "$WORK/probe.log" | head -3 | sed 's/^/     /'
  fails=$((fails + 1))
fi

if [ -f "$WORK/probe.asm" ]; then
  # 1. no linear symbol+offset table fill
  n=$(grep -c 'movwf[[:space:]]*v_pa+' "$WORK/probe.asm")
  if [ "$n" -eq 0 ]; then
    echo "ok   no linear v_pa+ table fill"
  else
    echo "FAIL: $n linear table-fill stores remain"
    fails=$((fails + 1))
  fi
  # 2. the table is written through INDF (chunk-resolved slots)
  n=$(grep -c 'movwf[[:space:]]*v__ind' "$WORK/probe.asm")
  if [ "$n" -ge 80 ]; then
    echo "ok   table written through INDF (staged slots)"
  else
    echo "FAIL: only $n staged stores (expected >= 80)"
    fails=$((fails + 1))
  fi
fi

echo
if [ "$fails" -eq 0 ]; then
  echo "PASS: pointer-array chunk initializer pin"
  exit 0
fi
echo "FAIL: $fails check(s)"
exit 1

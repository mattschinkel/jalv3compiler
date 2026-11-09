#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Pins the pic_data_alloc candidate-scan padding rule (P1, 2026-11-08):
# the single-bank candidate search must account for the pointer-slot
# padding, not just `need`.  Before the fix an 18F4620 refused a 64-slot
# `var pointer pa[64]` (need 192 + 64 padding = 256 = exactly one bank
# window) with "no space ... available=3966" because the first candidate
# (0x2) fit `need` but not `need + padding` and the node was abandoned.
#
# P2 (2026-11-08) changed the 65-slot case: pointer arrays are no longer
# pre-placed through the single-window path, so 65 slots (260 bytes) no
# longer refuse -- they take the normal placement (contiguous or chunked).
# The padding rule is still exercised by the 64-slot case above (the normal
# placement's first attempt is the same single-window scan); the structured
# refusal message remains for non-array/group allocations.
#
# Usage: unit-tests/test_pic_data_alloc_padding.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

gen() { # n file
  local n=$1 file=$2 i
  {
    echo "include 18f4620"
    echo "pragma target clock 20_000_000"
    echo "var byte v0 = 0"
    echo "var pointer pa[$n] as var byte"
    for ((i = 0; i < n; i++)); do
      echo "pa[$i] is v0"
    done
    echo "var byte i"
    echo "i = 0"
    echo "while i < $n loop"
    echo "   pa[i] = i"
    echo "   i = i + 1"
    echo "end loop"
  } > "$file"
}

fail() {
  echo "FAIL $1"
  shift
  echo "$@" | sed 's/^/     /'
  fails=$((fails + 1))
}

# --- 64 slots: 192 bytes + 64 padding = exactly one 256-byte bank window ---
gen 64 "$WORK/pa64.jal"
out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/pa64.jal" 2>&1)
if echo "$out" | grep -q "0 errors, 0 warnings"; then
  echo "ok   64-slot pointer array compiles"
else
  fail "64-slot pointer array refused (pre-fix behaviour)" "$out"
fi

if [ -f "$WORK/pa64.asm" ]; then
  addr=$(sed -n 's/^v_pa[[:space:]]*EQU[[:space:]]*0x\([0-9a-fA-F]*\).*/\1/p' "$WORK/pa64.asm" | head -1)
  if [ -n "$addr" ]; then
    lo=$((16#$addr))
    hi=$((lo + 63 * 3))  # 64 slots of 3 bytes (last used byte)
    if [ $((lo / 256)) -eq $((hi / 256)) ]; then
      echo "ok   pa placed in one bank (0x$(printf %x "$lo")..0x$(printf %x "$hi"))"
    else
      fail "pa spans a bank window (0x$(printf %x "$lo")..0x$(printf %x "$hi"))"
    fi
  else
    fail "pa has no EQU address in the asm"
  fi
else
  fail "no asm produced for the 64-slot build"
fi

# --- 65 slots: 260 bytes exceed one window; since P2 the normal placement
# --- takes it (was a clean single-window refusal before P2) ---
gen 65 "$WORK/pa65.jal"
out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/pa65.jal" 2>&1)
if echo "$out" | grep -q "0 errors"; then
  echo "ok   65-slot pointer array compiles (P2 always-normal placement)"
else
  fail "65-slot pointer array refused (P2 regression)" "$out"
fi

echo
if [ "$fails" -eq 0 ]; then
  echo "PASS: pic_data_alloc padding pin"
  exit 0
fi
echo "FAIL: $fails check(s)"
exit 1

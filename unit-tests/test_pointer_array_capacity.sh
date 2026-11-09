#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# P2: a pointer array is placed by the normal group/holed path, not by the
# volatile single-window pre-placement.  Pointer types are volatile, so the
# old pre-placement refused any array larger than one bank window (64 slots
# on 18F4620 after the padding fix, 46 on 16F18857); the normal path tries
# contiguous first and then segments.
#
# Witness: a 200-slot pointer array (600 bytes on PIC18) on 18F4620 lands in
# ONE contiguous run; on 16F18857 it compiles as a chunked placement.  The
# runtime proof for the chunked case is the P2b walk-aware slot access.
#
# Usage: unit-tests/test_pointer_array_capacity.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

gen() { # $1 = device
  {
    echo "include $1"
    echo "pragma target clock 20_000_000"
    echo "var byte v0 = 0"
    echo "var pointer pa[200] as var byte"
    i=0
    while [ "$i" -lt 200 ]; do
      echo "pa[$i] is v0"
      i=$((i + 1))
    done
  } > "$2"
}

check() { # $1 = device, $2 = expectation (contig|chunked)
  local dev=$1 want=$2
  local src="probe_$dev.jal"
  gen "$dev" "$WORK/$src"
  ( cd "$WORK" && "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" \
      "$src" > "$dev.log" 2>&1 )
  if ! grep -q "0 errors" "$WORK/$dev.log"; then
    echo "FAIL: $dev 200-slot pointer array did not compile"
    grep -E "error" "$WORK/$dev.log" | head -3 | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  echo "ok   $dev 200-slot pointer array compiles"
  if [ "$want" = "contig" ]; then
    if grep -qE "^v_pa +EQU +0x0060" "$WORK/probe_$dev.asm"; then
      echo "ok   $dev pa is one contiguous run at 0x0060"
    else
      echo "FAIL: $dev pa not at the expected contiguous base"
      grep -E "^v_pa " "$WORK/probe_$dev.asm" | sed 's/^/     /'
      fails=$((fails + 1))
    fi
  fi
}

check 18f4620 contig
check 16f18857 chunked

echo
if [ "$fails" -eq 0 ]; then
  echo "PASS: pointer-array capacity pin"
  exit 0
fi
echo "FAIL: $fails check(s)"
exit 1

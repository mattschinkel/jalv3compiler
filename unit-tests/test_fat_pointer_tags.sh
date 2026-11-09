#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# F0: the fat-pointer tag representation and its parity gate.
#   1. the encoding round-trips per target (PIC18 127 ids, classic 63,
#      14H 3, flat none) without touching the address bytes or the ROM bit;
#      F0b also scans every real linear file address (0x0020..0x196F on 14H,
#      ..0x01EF on classic, ..0x36FF on PIC18) and asserts that none of them
#      reads as a tag -- tag 0 must stay "legacy linear";
#   2. JALV2_FAT_POINTERS=1 is byte-identical to unset (F0 emits tag 0
#      only, so the whole corpus must not move).
#
# Usage: unit-tests/test_fat_pointer_tags.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

# --- 1. encoding round-trip + the F1 value operations ---
if ${CC_CC:-cc} -ansi -pedantic -Wall -Wextra -Werror \
    -I "$REPO/src/libpic12" -o "$WORK/fat_tags" \
    "$REPO/unit-tests/test_fat_tags.c" "$REPO/src/libpic12/pic_fat.c" \
    > "$WORK/cc.log" 2>&1; then
  if "$WORK/fat_tags" > "$WORK/run.log" 2>&1; then
    echo "ok   tag encoding round-trips on every target"
  else
    echo "FAIL: tag encoding assertions failed"
    cat "$WORK/run.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
else
  echo "FAIL: tag harness did not build"
  cat "$WORK/cc.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# --- 2. parity: fat on is byte-identical to fat off ---
mkdir -p "$WORK/off" "$WORK/on"
cp "$REPO/sims/gpsim/18f4620_gpsim_pointer_array_200.jal" "$WORK/off/"
cp "$REPO/sims/gpsim/18f4620_gpsim_pointer_array_200.jal" "$WORK/on/"
( cd "$WORK/off" && "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" \
    -s "$REPO/sims/shared" 18f4620_gpsim_pointer_array_200.jal > log 2>&1 )
( cd "$WORK/on" && JALV2_FAT_POINTERS=1 "$CC" -s "$REPO/demo_lib" \
    -s "$REPO/third_party/jallib/lib" -s "$REPO/sims/shared" \
    18f4620_gpsim_pointer_array_200.jal > log 2>&1 )
if grep -q "0 errors" "$WORK/off/log" && grep -q "0 errors" "$WORK/on/log"; then
  if cmp -s "$WORK/off/18f4620_gpsim_pointer_array_200.hex" \
      "$WORK/on/18f4620_gpsim_pointer_array_200.hex"; then
    echo "ok   JALV2_FAT_POINTERS=1 is byte-identical (F0: tag 0 only)"
  else
    echo "FAIL: fat-pointer flag changed the HEX in F0"
    fails=$((fails + 1))
  fi
else
  echo "FAIL: parity compile failed"
  grep -E "error" "$WORK/off/log" "$WORK/on/log" | head -4 | sed 's/^/     /'
  fails=$((fails + 1))
fi

echo
if [ "$fails" -eq 0 ]; then
  echo "PASS: fat-pointer representation pin"
  exit 0
fi
echo "FAIL: $fails check(s)"
exit 1

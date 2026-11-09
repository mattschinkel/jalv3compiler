#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# A pinned byte is not free in the SHARED window either (P1b, 2026-11-08).
# The device file parks the SFRs at 0xF80+ inside `pragma shared`; the
# shared fallback used to hand those bytes to user storage (the mp3/sd_card
# sample's `v_bitrate` landed on PORTA 0xF80 and the sample drives its SRAM
# chip select on PORTA bit 1).  Pins are now removed from the shared free
# list as well as from the data list.
#
# Witness: on 18F4525 the data area is filled to 88 free bytes, then a
# 100-byte array is requested.  Before the fix the array was placed at
# 0x0F80 (PORTA, over the SFRs) and the program compiled; after the fix
# there is no honest room and the compiler refuses with the array-too-big
# error instead of overlaying SFRs.
#
# Usage: unit-tests/test_pic_shared_window_pins.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

cat > "$WORK/w.jal" << 'JAL'
include 18f4525
pragma target clock 20_000_000
var byte fill[3860]
var byte big[100]
var word i
i = 0
while i < 3860 loop
   fill[i] = i
   i = i + 1
end loop
i = 0
while i < 100 loop
   big[i] = i
   i = i + 1
end loop
JAL

out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/w.jal" 2>&1)

# 1. no silent success: the old build compiled this (and put `big` on PORTA)
if echo "$out" | grep -q "0 errors"; then
  echo "FAIL: compiler accepted a program whose only 'room' is the SFR window"
  echo "$out" | sed 's/^/     /'
  fails=$((fails + 1))
else
  echo "ok   refused instead of overlaying the shared SFR window"
fi

# 2. the refusal is the array-too-big diagnostic, naming the array
if echo "$out" | grep -q "Array of 'big' (100 bytes)"; then
  echo "ok   refusal names the array and its size"
else
  echo "FAIL: expected the array-too-big diagnostic for 'big'"
  echo "$out" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# 3. and nothing was placed in the SFR block (a partial asm must not carry
#    a storage variable at 0xF80..0xFFF)
asm=$(ls "$WORK"/w.asm 2>/dev/null | head -1)
if [ -n "$asm" ]; then
  bad=$(grep -iE "^v_[A-Za-z0-9_]+ +EQU +0x0?F[89A-Fa-f][0-9A-Fa-f]" "$asm" \
        | grep -v -- "-->" | grep -v "control_sfr" | grep -v "; *po\?r\?t" | head -1)
  if [ -n "$bad" ]; then
    echo "FAIL: storage variable placed in the SFR window: $bad"
    fails=$((fails + 1))
  else
    echo "ok   no storage variable in the SFR window"
  fi
fi

echo
if [ "$fails" -eq 0 ]; then
  echo "PASS: shared-window pins pin"
  exit 0
fi
echo "FAIL: $fails check(s)"
exit 1

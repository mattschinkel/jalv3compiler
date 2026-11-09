#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# F2b/F3a: the fat-pointer deref.  A 200-byte array on the 16F18857 cannot be
# placed contiguously, so binding a pointer to it used to be refused by the
# pointer-target contiguity rule.  With the phase gate on
# (JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1) the bind stores the object id in
# the tag and the deref dispatches on it, walking the object's chunks.
#
# Checks:
#   1. the witness compiles clean with the gate off AND on;
#   2. the gate changes the image (the per-id dispatch, `xorlw id`, is only
#      in the on build) -- otherwise the witness proves nothing;
#   3. with the picsim engine available, the on build PASSES at run time.
#
# Usage: unit-tests/test_fat_pointer_deref.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WIT="$REPO/sims/picsim/16f18857_picsim_fat_pointer.jal"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC=(-s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib")

mkdir -p "$WORK/off" "$WORK/on"
cp "$WIT" "$WORK/off/"; cp "$WIT" "$WORK/on/"

( cd "$WORK/off" && "$CC" "${INC[@]}" 16f18857_picsim_fat_pointer.jal >log 2>&1 )
rc_off=$?
( cd "$WORK/on" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" "${INC[@]}" 16f18857_picsim_fat_pointer.jal >log 2>&1 )
rc_on=$?

if [ $rc_off -eq 0 ] && grep -q "0 errors" "$WORK/off/log"; then
  echo "ok   gate off: the witness compiles"
else
  echo "FAIL: gate off build failed"; sed 's/^/     /' "$WORK/off/log"; fails=$((fails + 1))
fi
if [ $rc_on -eq 0 ] && grep -q "0 errors" "$WORK/on/log"; then
  echo "ok   gate on: the chunked bind compiles (the contiguity rule yields)"
else
  echo "FAIL: gate on build failed"; sed 's/^/     /' "$WORK/on/log"; fails=$((fails + 1))
fi

off_x=$(grep -c xorlw "$WORK/off/16f18857_picsim_fat_pointer.asm" 2>/dev/null)
on_x=$(grep -c xorlw "$WORK/on/16f18857_picsim_fat_pointer.asm" 2>/dev/null)
if [ "${off_x:-0}" -eq 0 ] && [ "${on_x:-0}" -ge 4 ]; then
  echo "ok   only the gate-on build carries the per-id dispatch ($on_x sites)"
else
  echo "FAIL: dispatch not emitted (off=$off_x on=$on_x)"; fails=$((fails + 1))
fi
on_calls=$(grep -c "call.*chunkwalk" "$WORK/on/16f18857_picsim_fat_pointer.asm" 2>/dev/null)
if [ "${on_calls:-0}" -ge 1 ]; then
  echo "ok   the dispatch CALLs the shared chunk-walk helper ($on_calls calls)"
else
  echo "FAIL: the dispatch did not use the shared helper (inlined ladder?)"
  fails=$((fails + 1))
fi
if cmp -s "$WORK/off/16f18857_picsim_fat_pointer.hex" \
    "$WORK/on/16f18857_picsim_fat_pointer.hex"; then
  echo "FAIL: the gate did not change the image"; fails=$((fails + 1))
else
  echo "ok   the gate changes the image"
fi

# --- F3: an ESCAPED bind is fat with the feature on and NO knob -----------
# The escape witness lives here, not in sims/picsim: the picsim suite globs
# that directory and compiles every file with the DEFAULT environment, where
# this shape is (correctly) the escaped-bind refusal.
ESC="$WORK/esc/16f18857_fat_pointer_escape.jal"
mkdir -p "$WORK/esc"
cat > "$ESC" <<'JAL'
-- @picsim_validate begin
-- steps: 2000000
-- frequency_hz: auto
-- expect byte v_bad 0
-- @picsim_validate end
include 16f18857
var volatile byte big[200]
var byte i
var volatile byte bad = 0
var pointer p as var byte[200]
procedure check is
  var pointer q as var byte[200]
  q = p
  if q[37] != 38 then bad = 1 end if
  if q[199] != 200 then bad = 1 end if
end procedure
for 200 using i loop
  big[i] = i + 1
end loop
p is big
check()
if p[0] != 1 then bad = 1 end if
i = 100
if p[i] != 101 then bad = 1 end if
forever loop
end loop
JAL

( cd "$WORK/esc" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" \
    16f18857_fat_pointer_escape.jal >log 2>&1 )
if grep -q "0 errors" "$WORK/esc/log"; then
  echo "ok   escaped bind: fat without the knob"
else
  echo "FAIL: escaped bind did not compile"; sed 's/^/     /' "$WORK/esc/log"
  fails=$((fails + 1))
fi
# the bind must store a TAG (0x20 = id 1 on 14H), not a linear address --
# without this the dispatch would still "pass" by falling through tag 0
esc_tag=$(awk '/movwf +v_p\+1/{print prev} {prev=$0}' \
    "$WORK/esc/16f18857_fat_pointer_escape.asm" 2>/dev/null | grep -c "movlw *32")
if [ "${esc_tag:-0}" -ge 1 ]; then
  echo "ok   the escaped bind stores a tag, not a linear address"
else
  echo "FAIL: the escaped bind is still linear (tag 0) -- F3 not exercised"
  fails=$((fails + 1))
fi
esc_x=$(grep -c xorlw "$WORK/esc/16f18857_fat_pointer_escape.asm" 2>/dev/null)
if [ "${esc_x:-0}" -ge 2 ]; then
  echo "ok   escaped deref (through a copy) carries the dispatch ($esc_x sites)"
else
  echo "FAIL: no dispatch on the escaped deref (esc=$esc_x)"; fails=$((fails + 1))
fi

# --- the id cap: 3 on 14H (F0b's tag layout), refused ONCE ---------------
mkdir -p "$WORK/cap"
cat > "$WORK/cap/cap4.jal" <<'JAL'
include 16f18857
var volatile byte b1[200]
var volatile byte b2[200]
var volatile byte b3[200]
var volatile byte b4[200]
var pointer p1 as var byte[200]
var pointer p2 as var byte[200]
var pointer p3 as var byte[200]
var pointer p4 as var byte[200]
p1 is b1
p2 is b2
p3 is b3
p4 is b4
forever loop
end loop
JAL
sed '/b4\[200\]/d; /p4 as var/d; /p4 is/d' "$WORK/cap/cap4.jal" > "$WORK/cap/cap3.jal"
( cd "$WORK/cap" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 "$CC" "${INC[@]}" \
    cap4.jal >log4 2>&1 )
cap_ct=$(grep -c "too many address-taken multi-segment objects" "$WORK/cap/log4")
if [ "${cap_ct:-0}" -eq 1 ] && grep -q "3 ids on this core" "$WORK/cap/log4"; then
  echo "ok   4 fat objects on 14H: refused once, naming the 3-id cap"
else
  echo "FAIL: the id cap diagnostic (count=$cap_ct)"; sed 's/^/     /' "$WORK/cap/log4" | head -6
  fails=$((fails + 1))
fi
( cd "$WORK/cap" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 "$CC" "${INC[@]}" \
    cap3.jal >log3 2>&1 )
if grep -q "0 errors" "$WORK/cap/log3"; then
  echo "ok   3 fat objects on 14H: compiles"
else
  echo "FAIL: 3 fat objects refused"; sed 's/^/     /' "$WORK/cap/log3" | head -6
  fails=$((fails + 1))
fi

# --- the tag lives in a DIFFERENT place per target: classic 14 puts the id at
# --- bit 1 (mask 0x7E), 14H at bit 5 (0x60).  Emit-level check on the classic
# --- core, whose ladder is also the FSR/IRP one.
mkdir -p "$WORK/c14"
cat > "$WORK/c14/c14.jal" <<'JAL'
include 16f887
var volatile byte big[200]
var byte i
var volatile byte bad = 0
var pointer p as var byte[200]
for 200 using i loop
  big[i] = i + 1
end loop
p is big
if p[0] != 1 then bad = 1 end if
if p[37] != 38 then bad = 1 end if
i = 100
if p[i] != 101 then bad = 1 end if
forever loop
end loop
JAL
( cd "$WORK/c14" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 "$CC" "${INC[@]}" \
    c14.jal >log 2>&1 )
if grep -q "0 errors" "$WORK/c14/log"; then
  echo "ok   classic 14: the chunked fat bind compiles"
else
  echo "FAIL: classic 14 build failed"; sed 's/^/     /' "$WORK/c14/log" | head -6
  fails=$((fails + 1))
fi
c14_tag=$(awk '/movwf +v_p\+1/{print prev} {prev=$0}' "$WORK/c14/c14.asm" 2>/dev/null | grep -c "movlw *2$")
c14_x=$(grep -c xorlw "$WORK/c14/c14.asm" 2>/dev/null)
if [ "${c14_tag:-0}" -ge 1 ] && [ "${c14_x:-0}" -ge 2 ]; then
  echo "ok   classic 14: id at bit 1 (movlw 2) and the dispatch is present"
else
  echo "FAIL: classic 14 tag/dispatch (tag=$c14_tag x=$c14_x)"; fails=$((fails + 1))
fi

# runtime arm, when the engine is present
PICSIM="${PICSIM_DIR:-$REPO/../tmp/picsim-try/picsim}"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WIT" --picsim "$PICSIM" \
      --compiler "$CC" > "$WORK/picsim.log" 2>&1 ); then
    echo "ok   the fat deref passes picsim (const + dynamic subscripts)"
  else
    echo "FAIL: picsim run failed"; tail -20 "$WORK/picsim.log" | sed 's/^/     /'; fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$ESC" --picsim "$PICSIM" \
      --compiler "$CC" > "$WORK/picsim-esc.log" 2>&1 ); then
    echo "ok   the escaped fat deref passes picsim"
  else
    echo "FAIL: picsim escape run failed"; tail -20 "$WORK/picsim-esc.log" | sed 's/^/     /'; fails=$((fails + 1))
  fi
else
  echo "skip picsim engine not available"
fi

if [ $fails -eq 0 ]; then echo; echo "PASS: fat-pointer deref pin"; exit 0; fi
echo; echo "FAIL: $fails check(s)"; exit 1

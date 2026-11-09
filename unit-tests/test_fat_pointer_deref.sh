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

# --- F4a: `pragma target fat_pointers on|off` is packaged with the program -
# A program can opt in without an environment variable, and a pragma is
# AUTHORITATIVE (off wins even with JALV2_FAT_POINTERS=1 set).
mkdir -p "$WORK/pragma"
cat > "$WORK/pragma/body.jal" <<'JAL'
include 16f887
var volatile byte big[200]
var byte i
var volatile byte bad = 0
var pointer p as var byte[200]
procedure check is
  var pointer q as var byte[200]
  q = p
  if q[37] != 38 then bad = 1 end if
end procedure
for 200 using i loop
  big[i] = i + 1
end loop
p is big
check()
i = 100
if p[i] != 101 then bad = 1 end if
forever loop
end loop
JAL
{ echo "pragma target fat_pointers on"; cat "$WORK/pragma/body.jal"; } > "$WORK/pragma/on.jal"
{ echo "pragma target fat_pointers off"; cat "$WORK/pragma/body.jal"; } > "$WORK/pragma/off.jal"
cp "$WORK/pragma/body.jal" "$WORK/pragma/none.jal"
cp "$WORK/pragma/body.jal" "$WORK/pragma/bad.jal"
sed -i '1i pragma target fat_pointers maybe' "$WORK/pragma/bad.jal"

( cd "$WORK/pragma" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" on.jal >on.log 2>&1 )
if grep -q "0 errors" "$WORK/pragma/on.log" \
    && [ "$(grep -c xorlw "$WORK/pragma/on.asm" 2>/dev/null)" -ge 2 ]; then
  echo "ok   pragma fat_pointers on: fat without any environment variable"
else
  echo "FAIL: the pragma did not enable the engine"
  tail -4 "$WORK/pragma/on.log" | sed 's/^/     /'; fails=$((fails + 1))
fi

if ( cd "$WORK/pragma" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" off.jal >off.log 2>&1 ); then
  echo "FAIL: pragma off was overridden by JALV2_FAT_POINTERS=1"
  fails=$((fails + 1))
else
  echo "ok   pragma fat_pointers off wins over the environment"
fi

if ( cd "$WORK/pragma" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" none.jal >none.log 2>&1 ); then
  echo "FAIL: the escaped shape compiled with no gate at all"
  fails=$((fails + 1))
else
  echo "ok   no pragma, no env: the escaped shape is still refused"
fi

if ( cd "$WORK/pragma" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" none.jal >env.log 2>&1 ) \
    && [ "$(grep -c xorlw "$WORK/pragma/none.asm" 2>/dev/null)" -ge 2 ]; then
  echo "ok   no pragma + JALV2_FAT_POINTERS=1 still enables the engine"
else
  echo "FAIL: the env path no longer works"; fails=$((fails + 1))
fi

if ( cd "$WORK/pragma" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" bad.jal >bad.log 2>&1 ); then
  echo "FAIL: a bad pragma value was accepted"
  fails=$((fails + 1))
elif grep -q "pragma target fat_pointers expects on or off" "$WORK/pragma/bad.log"; then
  echo "ok   a bad pragma value is reported"
else
  echo "FAIL: no diagnostic for a bad pragma value"; fails=$((fails + 1))
fi

# --- F4: a ROM const pointer alongside a fat RAM pointer -------------------
# The ROM marker is bit 7 of the pointer; the tag mask (0x60 on 14H) clears it,
# so a ROM pointer reads as tag 0 and takes the linear/PTR_READ path.  This arm
# proves that coexistence: one chunked RAM object (so ids exist) plus a
# const-table pointer, both measured.
mkdir -p "$WORK/rom"
cat > "$WORK/rom/rom.jal" <<'JAL'
-- @picsim_validate begin
-- steps: 3000000
-- frequency_hz: auto
-- expect byte v_rom 51
-- expect byte v_ram 38
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
const byte table[4] = {0x11, 0x22, 0x33, 0x44}
var volatile byte big[120]
var byte i
var volatile byte rom = 0
var volatile byte ram = 0
var pointer t as const byte[4]
var pointer p as var byte[120]
for 120 using i loop
  big[i] = i + 1
end loop
p is big
t is table
rom = t[2]
ram = p[37]
forever loop
end loop
JAL
sed 's/expect byte v_rom 51/expect byte v_rom 52/' "$WORK/rom/rom.jal" \
    > "$WORK/rom/rom_neg.jal"
ROM_PICSIM="${PICSIM_DIR:-$REPO/../tmp/picsim-try/picsim}"
if [ -x "$ROM_PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/rom/rom.jal" \
      --picsim "$ROM_PICSIM" --compiler "$CC" > "$WORK/rom/pos.log" 2>&1 ); then
    echo "ok   a ROM const pointer coexists with a fat RAM pointer"
  else
    echo "FAIL: ROM + fat RAM coexistence"; tail -10 "$WORK/rom/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/rom/rom_neg.jal" \
      --picsim "$ROM_PICSIM" --compiler "$CC" > "$WORK/rom/neg.log" 2>&1 ); then
    echo "FAIL: the ROM arm's negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   ROM arm: negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
fi

# --- classic 14 ESCAPED deref: a MEASUREMENT under gpsim + negative control -
mkdir -p "$WORK/esc14"
cat > "$WORK/esc14/esc.jal" <<'JAL'
-- @gpsim_validate begin
-- steps: 400000
-- expect byte v_got 38
-- @gpsim_validate end
include 16f887
var volatile byte big[40]
var byte i
var volatile byte got = 0
var pointer p as var byte[40]
procedure check is
  var pointer q as var byte[40]
  q = p
  got = q[37]
end procedure
for 40 using i loop
  big[i] = i + 1
end loop
p is big
check()
forever loop
end loop
JAL
sed 's/expect byte v_got 38/expect byte v_got 39/' "$WORK/esc14/esc.jal" \
    > "$WORK/esc14/esc_neg.jal"
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK/esc14" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/esc14/esc.jal" > pos.log 2>&1 ); then
    echo "ok   classic 14 escaped deref: got = 38 (measurement)"
  else
    echo "FAIL: classic 14 escaped deref measurement"; tail -10 "$WORK/esc14/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$WORK/esc14" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/esc14/esc_neg.jal" > neg.log 2>&1 ); then
    echo "FAIL: the classic 14 negative control PASSED (the arm cannot fail)"
    fails=$((fails + 1))
  else
    echo "ok   classic 14 escaped deref: negative control fails as it must"
  fi
else
  echo "skip gpsim not available"
fi

# --- 14H ESCAPED deref: a MEASUREMENT, not a guard, and a negative control -
# The witness lives here because the picsim suite compiles with a clean
# environment (the escaped shape is then refused).  NOTE the `pragma target
# clock`: without it the picsim engine does not execute the program at all and
# every measurement silently reads zero -- the mistake that produced a false
# "the fat deref is broken" conclusion on 2026-11-09.
mkdir -p "$WORK/esc14h"
cat > "$WORK/esc14h/esc.jal" <<'JAL'
-- @picsim_validate begin
-- steps: 2000000
-- frequency_hz: auto
-- expect byte v_got 38
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var volatile byte big[200]
var byte i
var volatile byte got = 0
var pointer p as var byte[200]
procedure check is
  var pointer q as var byte[200]
  q = p
  got = q[37]
end procedure
for 200 using i loop
  big[i] = i + 1
end loop
p is big
check()
forever loop
end loop
JAL
sed 's/expect byte v_got 38/expect byte v_got 39/' "$WORK/esc14h/esc.jal" \
    > "$WORK/esc14h/esc_neg.jal"

PICSIM="${PICSIM_DIR:-$REPO/../tmp/picsim-try/picsim}"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/esc14h/esc.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/esc14h/pos.log" 2>&1 ); then
    echo "ok   14H escaped deref: got = 38 (measurement)"
  else
    echo "FAIL: 14H escaped deref measurement"; tail -10 "$WORK/esc14h/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/esc14h/esc_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/esc14h/neg.log" 2>&1 ); then
    echo "FAIL: the 14H negative control PASSED (the arm cannot fail)"
    fails=$((fails + 1))
  else
    echo "ok   14H escaped deref: negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
fi

# runtime arm, when the engine is present
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WIT" --picsim "$PICSIM" \
      --compiler "$CC" > "$WORK/picsim.log" 2>&1 ); then
    echo "ok   the fat deref passes picsim (const + dynamic subscripts)"
  else
    echo "FAIL: picsim run failed"; tail -20 "$WORK/picsim.log" | sed 's/^/     /'; fails=$((fails + 1))
  fi
else
  echo "skip picsim engine not available"
fi

if [ $fails -eq 0 ]; then echo; echo "PASS: fat-pointer deref pin"; exit 0; fi
echo; echo "FAIL: $fails check(s)"; exit 1

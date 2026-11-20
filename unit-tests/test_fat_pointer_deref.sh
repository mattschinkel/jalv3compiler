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
on_seg=$(grep -c "call.*pic_fat_seg_addr" \
    "$WORK/on/16f18857_picsim_fat_pointer.asm" 2>/dev/null)
if [ "${off_x:-0}" -eq 0 ] && [ "${on_x:-0}" -eq 0 ] \
    && [ "${on_seg:-0}" -ge 4 ]; then
  echo "ok   the gate-on build dispatches through the table ($on_seg seg_addr calls, 0 compares)"
else
  echo "FAIL: dispatch shape (off=$off_x on=$on_x on_seg=$on_seg)"
  fails=$((fails + 1))
fi
if grep -q "_pic_fat_desc_tbl" "$WORK/on/16f18857_picsim_fat_pointer.asm" \
    && grep -q "_pic_fat_desc_1" "$WORK/on/16f18857_picsim_fat_pointer.asm"; then
  echo "ok   the descriptor table and object descriptor are emitted and filled"
else
  echo "FAIL: no _obj_desc table / descriptor in the table build"
  fails=$((fails + 1))
fi
on_calls=$(grep -c "call.*chunkwalk" "$WORK/on/16f18857_picsim_fat_pointer.asm" 2>/dev/null)
if [ "${on_calls:-0}" -ge 1 ]; then
  echo "ok   the array's own accesses still CALL the shared chunk-walk helper ($on_calls calls)"
else
  echo "FAIL: the shared chunk-walk helper vanished"
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

( cd "$WORK/esc" && JALV2_FAT_POINTERS=1 JALV2_FAT_PTR3=off "$CC" "${INC[@]}" \
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
esc_add=$(grep -c "addwf.*pic_chunk_t,f" \
    "$WORK/esc/16f18857_fat_pointer_escape.asm" 2>/dev/null)
if [ "${esc_add:-0}" -ge 1 ]; then
  echo "ok   the dynamic subscript joins the object offset (addwf _pic_chunk_t)"
else
  echo "FAIL: the dispatch does not add the runtime subscript"; fails=$((fails + 1))
fi
esc_seg=$(grep -c "call.*pic_fat_seg_addr" \
    "$WORK/esc/16f18857_fat_pointer_escape.asm" 2>/dev/null)
if [ "${esc_seg:-0}" -ge 2 ]; then
  echo "ok   escaped deref (through a copy) dispatches through the table ($esc_seg sites)"
else
  echo "FAIL: no dispatch on the escaped deref (esc_seg=$esc_seg)"; fails=$((fails + 1))
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
( cd "$WORK/cap" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 JALV2_FAT_PTR3=off \
    "$CC" "${INC[@]}" cap4.jal >log4 2>&1 )
cap_ct=$(grep -c "too many address-taken multi-segment objects" "$WORK/cap/log4")
if [ "${cap_ct:-0}" -eq 1 ] && grep -q "3 ids on this core" "$WORK/cap/log4"; then
  echo "ok   4 fat objects on 14H: refused once, naming the 3-id cap"
else
  echo "FAIL: the id cap diagnostic (count=$cap_ct)"; sed 's/^/     /' "$WORK/cap/log4" | head -6
  fails=$((fails + 1))
fi
( cd "$WORK/cap" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 JALV2_FAT_PTR3=off \
    "$CC" "${INC[@]}" cap3.jal >log3 2>&1 )
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
c14_seg=$(grep -c "call.*pic_fat_seg_addr" "$WORK/c14/c14.asm" 2>/dev/null)
if [ "${c14_tag:-0}" -ge 1 ] && [ "${c14_seg:-0}" -ge 2 ]; then
  echo "ok   classic 14: id at bit 1 (movlw 2) and the table dispatch is present"
else
  echo "FAIL: classic 14 tag/dispatch (tag=$c14_tag seg=$c14_seg)"; fails=$((fails + 1))
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
    && [ "$(grep -c "call.*pic_fat_seg_addr" "$WORK/pragma/on.asm" 2>/dev/null)" -ge 2 ]; then
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

if ( cd "$WORK/pragma" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" none.jal >none.log 2>&1 ) \
    && [ "$(grep -c "call.*pic_fat_seg_addr" "$WORK/pragma/none.asm" 2>/dev/null)" -ge 2 ]; then
  echo "ok   no pragma, no env: the default enables the engine"
else
  echo "FAIL: the default no longer enables the engine"
  tail -4 "$WORK/pragma/none.log" | sed 's/^/     /'; fails=$((fails + 1))
fi

if ( cd "$WORK/pragma" && JALV2_FAT_POINTERS=0 "$CC" "${INC[@]}" none.jal >classic.log 2>&1 ); then
  echo "FAIL: JALV2_FAT_POINTERS=0 was ignored"
  fails=$((fails + 1))
else
  echo "ok   JALV2_FAT_POINTERS=0 still restores the classic refusal"
fi

if ( cd "$WORK/pragma" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" none.jal >env.log 2>&1 ) \
    && [ "$(grep -c "call.*pic_fat_seg_addr" "$WORK/pragma/none.asm" 2>/dev/null)" -ge 2 ]; then
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
-- steps: 600000
-- expect byte v_got 38
-- expect byte v_got2 38
-- @gpsim_validate end
include 16f887
var volatile byte big[200]
var byte i
var volatile byte got = 0
var volatile byte got2 = 0
var pointer p as var byte[200]
procedure check is
  var pointer q as var byte[200]
  q = p
  got = q[37]
  i = 37
  got2 = q[i]
end procedure
for 200 using i loop
  big[i] = i + 1
end loop
p is big
check()
forever loop
end loop
JAL
sed 's/expect byte v_got2 38/expect byte v_got2 39/' "$WORK/esc14/esc.jal" \
    > "$WORK/esc14/esc_neg.jal"
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK/esc14" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/esc14/esc.jal" > pos.log 2>&1 ); then
    echo "ok   classic 14 escaped deref: const + dynamic = 38 (measurement)"
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

# --- F4 default: the 3-byte form's tag lives in BYTE 2 (mask 127) -----------
# With 3-byte pointers the default on 14H, the bind must store the id in byte 2
# and the dispatch must mask with 127 (the 2-byte form used byte 1 / mask 96).
mkdir -p "$WORK/p3def"
cat > "$WORK/p3def/def.jal" <<'JAL'
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
( cd "$WORK/p3def" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" def.jal >log 2>&1 )
def_tag=$(awk '/movwf +v_p\+2/{print prev} {prev=$0}' "$WORK/p3def/def.asm" 2>/dev/null | grep -c "movlw *1$")
def_mask=$(grep -c "andlw *127" "$WORK/p3def/def.asm" 2>/dev/null)
if grep -q "0 errors" "$WORK/p3def/log" && [ "${def_tag:-0}" -ge 1 ] && [ "${def_mask:-0}" -ge 1 ]; then
  echo "ok   3-byte default: id at byte 2 (movlw 1) and the 127 mask"
else
  echo "FAIL: 3-byte default tag/dispatch (tag=$def_tag mask=$def_mask)"
  tail -3 "$WORK/p3def/log" | sed 's/^/     /'; fails=$((fails + 1))
fi

# --- F4: `pragma target fat_pointers 3byte` (3-byte pointers on 14H) -------
# The 2-byte 14H form carries the tag in byte 1 (3 ids); the 3-byte form moves
# it to byte 2 (127 ids).  The size AND the target kind must switch together
# (a size-only probe compiled but ran wrong: the bind and the deref disagreed).
mkdir -p "$WORK/p3"
cat > "$WORK/p3/p3.jal" <<'JAL'
-- @picsim_validate begin
-- steps: 2000000
-- frequency_hz: auto
-- expect byte v_got 38
-- @picsim_validate end
pragma target fat_pointers 3byte
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
sed 's/expect byte v_got 38/expect byte v_got 39/' "$WORK/p3/p3.jal" \
    > "$WORK/p3/p3_neg.jal"
cat > "$WORK/p3/four.jal" <<'JAL'
pragma target fat_pointers 3byte
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var volatile byte b1[100]
var volatile byte b2[100]
var volatile byte b3[100]
var volatile byte b4[100]
var byte idx
var volatile byte r = 0
var pointer p1 as var byte[100]
var pointer p2 as var byte[100]
var pointer p3 as var byte[100]
var pointer p4 as var byte[100]
for 100 using idx loop
  b1[idx] = idx + 1
  b2[idx] = idx + 2
  b3[idx] = idx + 3
  b4[idx] = idx + 4
end loop
p1 is b1
p2 is b2
p3 is b3
p4 is b4
r = p1[37] + p2[37] + p3[37] + p4[37]
forever loop
end loop
JAL
( cd "$WORK/p3" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" four.jal \
    >four.log 2>&1 )
if grep -q "0 errors" "$WORK/p3/four.log"; then
  echo "ok   pragma 3byte: 4 fat objects compile on 14H (127 ids)"
else
  echo "FAIL: 4 fat objects failed under pragma 3byte"
  tail -4 "$WORK/p3/four.log" | sed 's/^/     /'; fails=$((fails + 1))
fi
P3_PICSIM="${PICSIM_DIR:-$REPO/../tmp/picsim-try/picsim}"
if [ -x "$P3_PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && env -u JALV2_FAT_POINTERS \
      python3 sims/picsim_validate_engine.py "$WORK/p3/p3.jal" \
      --picsim "$P3_PICSIM" --compiler "$CC" > "$WORK/p3/pos.log" 2>&1 ); then
    echo "ok   pragma 3byte: escaped deref measures 38 (3-byte pointer)"
  else
    echo "FAIL: pragma 3byte measurement"; tail -8 "$WORK/p3/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && env -u JALV2_FAT_POINTERS \
      python3 sims/picsim_validate_engine.py "$WORK/p3/p3_neg.jal" \
      --picsim "$P3_PICSIM" --compiler "$CC" > "$WORK/p3/neg.log" 2>&1 ); then
    echo "FAIL: pragma 3byte negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   pragma 3byte: negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
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
-- steps: 3000000
-- frequency_hz: auto
-- expect byte v_got 38
-- expect byte v_got2 38
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var volatile byte big[200]
var byte i
var volatile byte got = 0
var volatile byte got2 = 0
var pointer p as var byte[200]
procedure check is
  var pointer q as var byte[200]
  q = p
  got = q[37]
  i = 37
  got2 = q[i]
end procedure
for 200 using i loop
  big[i] = i + 1
end loop
p is big
check()
forever loop
end loop
JAL
sed 's/expect byte v_got2 38/expect byte v_got2 39/' "$WORK/esc14h/esc.jal" \
    > "$WORK/esc14h/esc_neg.jal"

PICSIM="${PICSIM_DIR:-$REPO/../tmp/picsim-try/picsim}"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/esc14h/esc.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/esc14h/pos.log" 2>&1 ); then
    echo "ok   14H escaped deref: const + dynamic = 38 (measurement)"
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

# --- the TRACKED witness now really runs (clock pragma + dynamic subscript) -
# It used to have no `pragma target clock`, so picsim never executed it and
# its `bad = 0` expectation passed vacuously -- which hid the fact that the
# dynamic subscript read p[0].  With the pragma the const 0/37/199 and the
# dynamic 100 checks are a real measurement; the negative control changes the
# dynamic expectation so `bad` must go to 1.
mkdir -p "$WORK/real"
cp "$WIT" "$WORK/real/"
sed 's/if p\[i\] != 101 then bad = 1 end if/if p[i] != 102 then bad = 1 end if/' \
    "$WIT" > "$WORK/real/wit_neg.jal"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WIT" --picsim "$PICSIM" \
      --compiler "$CC" > "$WORK/real/pos.log" 2>&1 ); then
    echo "ok   tracked witness: const 0/37/199 + dynamic 100 all measure"
  else
    echo "FAIL: tracked witness run"; tail -8 "$WORK/real/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/real/wit_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/real/neg.log" 2>&1 ); then
    echo "FAIL: tracked witness negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   tracked witness: negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
fi

# --- a WORD array with a >255 index: the 16-bit object offset and the index
# --- high byte.  big[299] is object byte 598, so the pointer offset's high
# --- half is exercised and the result is a full word.
mkdir -p "$WORK/wsize"
cat > "$WORK/wsize/w.jal" <<'JAL'
-- @picsim_validate begin
-- steps: 4000000
-- frequency_hz: auto
-- expect word v_got 101
-- expect word v_got2 300
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var volatile word big[300]
var word i
var volatile word got = 0
var volatile word got2 = 0
var pointer p as var word[300]
for 300 using i loop
  big[i] = i + 1
end loop
p is big
i = 100
got = p[i]
i = 299
got2 = p[i]
forever loop
end loop
JAL
sed 's/expect word v_got 101/expect word v_got 102/' "$WORK/wsize/w.jal" \
    > "$WORK/wsize/w_neg.jal"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/wsize/w.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/wsize/pos.log" 2>&1 ); then
    echo "ok   dynamic fat subscript at a >255 byte offset on a word array"
  else
    echo "FAIL: word-array dynamic subscript"; tail -8 "$WORK/wsize/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/wsize/w_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/wsize/neg.log" 2>&1 ); then
    echo "FAIL: word-array negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   word-array dynamic subscript: negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
fi

# --- F5: WHEREIS() of a byte inside a CHUNKED object ----------------------
# WHEREIS(p) must report the PHYSICAL address the object model gives the
# target, not `variable base + linear offset` (which lands in the RAM hole
# past the first chunk).  The pin measures the distance between element 0 and
# element 100: for a chunked 200-byte array a hole intervenes, so the true
# distance is MORE than 100 (40 -> 188 here); the linear formula gives exactly
# 100.  Layout-independent: any hole between the chunks makes it > 100.
mkdir -p "$WORK/whereis"
cat > "$WORK/whereis/w.jal" <<'JAL'
-- @picsim_validate begin
-- steps: 2000000
-- frequency_hz: auto
-- expect byte v_bad 0
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var volatile byte big[200]
var byte i
var volatile byte bad = 0
var word wa
var word wb
var word wbase
var pointer p0 as var byte
var pointer p100 as var byte
for 200 using i loop
  big[i] = i + 1
end loop
p0 is big[0]
p100 is big[100]
wa = WHEREIS(p0)
wb = WHEREIS(p100)
wbase = WHEREIS(big)
if wa != wbase then bad = 1 end if
if (wb - wa) <= 100 then bad = 2 end if
forever loop
end loop
JAL
sed 's/if (wb - wa) <= 100 then bad = 2 end if/if (wb - wa) > 100 then bad = 2 else bad = 0 end if/' \
    "$WORK/whereis/w.jal" > "$WORK/whereis/w_neg.jal"
( cd "$WORK/whereis" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" "${INC[@]}" w.jal >log 2>&1 )
if grep -q "0 errors" "$WORK/whereis/log"; then
  echo "ok   WHEREIS of a chunked element compiles"
else
  echo "FAIL: WHEREIS witness build"; tail -4 "$WORK/whereis/log" | sed 's/^/     /'
  fails=$((fails + 1))
fi
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/whereis/w.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/whereis/pos.log" 2>&1 ); then
    echo "ok   WHEREIS of a chunked element is the physical address"
  else
    echo "FAIL: WHEREIS chunk resolution"; tail -8 "$WORK/whereis/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/whereis/w_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/whereis/neg.log" 2>&1 ); then
    echo "FAIL: WHEREIS negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   WHEREIS: negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
fi

# --- F2d: the O(1) dispatch's code size, pinned ---------------------------
# Three fat objects, nine const derefs.  The old per-id compare chain emitted
# 27 compares and 1102 instructions on this witness; the table emits nine
# calls to the one shared walk and 855 instructions.  The chain is gone, so
# the pin cannot rebuild it -- it pins the SHAPE (0 compares, 9 calls, three
# descriptors) and an instruction ceiling that a regression toward O(ids)
# would break (27 compares would add ~247 instructions; the ceiling is 1000).
mkdir -p "$WORK/size/table"
cat > "$WORK/size/size.jal" <<'JAL'
-- @picsim_validate begin
-- steps: 4000000
-- frequency_hz: auto
-- expect byte v_bad 0
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var volatile byte b1[200]
var volatile byte b2[150]
var volatile byte b3[90]
var byte i
var volatile byte bad = 0
var pointer p1 as var byte[200]
var pointer p2 as var byte[150]
var pointer p3 as var byte[90]
for 200 using i loop
  b1[i] = i + 1
end loop
for 150 using i loop
  b2[i] = i + 11
end loop
for 90 using i loop
  b3[i] = i + 21
end loop
p1 is b1
p2 is b2
p3 is b3
if p1[0] != 1 then bad = 1 end if
if p1[37] != 38 then bad = 2 end if
if p1[90] != 91 then bad = 3 end if
if p2[0] != 11 then bad = 4 end if
if p2[37] != 48 then bad = 5 end if
if p2[90] != 101 then bad = 6 end if
if p3[0] != 21 then bad = 7 end if
if p3[37] != 58 then bad = 8 end if
if p3[80] != 101 then bad = 9 end if
forever loop
end loop
JAL
sed 's/expect byte v_bad 0/expect byte v_bad 5/' "$WORK/size/size.jal" \
    > "$WORK/size/size_neg.jal"
cp "$WORK/size/size.jal" "$WORK/size/table/"
( cd "$WORK/size/table" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" "${INC[@]}" size.jal >log 2>&1 )
insn_ct() {
  awk '{ line=$0; sub(/^[ \t]+/,"",line);
         if (line ~ /^[a-z][a-z0-9_.]*([ \t]|$)/ &&
             line !~ /^(equ|org|end|db|cblock|endc|__config|datahi_set|datahi_clr|movlp)/)
           n++ }
       END { print n+0 }' "$1"
}
if grep -q "0 errors" "$WORK/size/table/log"; then
  t_ct=$(insn_ct "$WORK/size/table/size.asm")
  t_x=$(grep -c xorlw "$WORK/size/table/size.asm")
  t_seg=$(grep -c "call.*pic_fat_seg_addr" "$WORK/size/table/size.asm")
  t_desc=$(grep -c "v__pic_fat_desc_" "$WORK/size/table/size.asm")
  if [ "$t_x" -eq 0 ] && [ "$t_seg" -eq 9 ] && [ "$t_desc" -ge 3 ] \
      && [ "$t_ct" -lt 1000 ]; then
    echo "ok   F2d size: 3 objects x 3 sites, ${t_ct} instructions ($t_seg calls, 0 compares)"
  else
    echo "FAIL: F2d size/shape (insn=$t_ct xor=$t_x seg=$t_seg desc=$t_desc)"
    fails=$((fails + 1))
  fi
else
  echo "FAIL: the F2d size witness did not compile"
  tail -4 "$WORK/size/table/log" | sed 's/^/     /'; fails=$((fails + 1))
fi
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/size/size.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/size/pos.log" 2>&1 ); then
    echo "ok   F2d size: 3 objects x 3 sites all read back"
  else
    echo "FAIL: F2d size run"; tail -8 "$WORK/size/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
      python3 sims/picsim_validate_engine.py "$WORK/size/size_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/size/neg.log" 2>&1 ); then
    echo "FAIL: F2d size negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   F2d size: negative control fails as it must"
  fi
fi

# --- F5: a pointer-ARRAY element bind keeps its SLOT index ----------------
# F5's fat-slot surface (the design's F3 escape: "store it in a pointer
# array") cannot work while the store itself goes to the wrong slot.
# `pa[1] is arr[4]` used to store the pointer into slot 0: the destination
# value's base offset (the slot index) was cleared before the base store AND
# before the element-offset add, so `pa[0]` was overwritten by the later
# bind and `pa[1]` was never bound.  Measured on 16F18857 (and 16F887,
# identical defect): pre-fix pa[0] = 50 (arr[4]) and pa[1] = 0; post-fix
# 10 / 50.  The fix keeps a CONSTANT destination base offset; a runtime
# slot index (`slots[rti] is x`) keeps its old path -- recorded as a
# pre-existing limitation in docs/fixes/fix_fat_pointer_slot_element_bind.md.
mkdir -p "$WORK/slot"
cat > "$WORK/slot/s.jal" <<'JALS'
-- @picsim_validate begin
-- steps: 2000000
-- frequency_hz: auto
-- expect byte v_s0 10
-- expect byte v_s1 50
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var byte arr[8]
var volatile byte s0 = 0
var volatile byte s1 = 0
var pointer pa[2] as var byte
arr[0] = 10
arr[4] = 50
pa[0] is arr[0]
pa[1] is arr[4]
s0 = pa[0]
s1 = pa[1]
forever loop
end loop
JALS
sed 's/expect byte v_s0 10/expect byte v_s0 11/' "$WORK/slot/s.jal" \
    > "$WORK/slot/s_neg.jal"
( cd "$WORK/slot" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" s.jal >log 2>&1 )
if grep -q "0 errors" "$WORK/slot/log"; then
  echo "ok   pointer-slot element binds compile"
else
  echo "FAIL: pointer-slot element bind build"; tail -4 "$WORK/slot/log" | sed 's/^/     /'
  fails=$((fails + 1))
fi
# the ENCODING: slot 1's base and element add must hit the slot's own bytes
slot_w=$(grep -c "movwf *v_pa+3" "$WORK/slot/s.asm" 2>/dev/null)
slot_r=$(grep -c "movf *v_pa+3" "$WORK/slot/s.asm" 2>/dev/null)
if [ "${slot_w:-0}" -ge 2 ] && [ "${slot_r:-0}" -ge 1 ]; then
  echo "ok   slot 1 stores its base and its element offset in slot 1 (v_pa+3)"
else
  echo "FAIL: slot 1 bind wrote the wrong slot (w=$slot_w r=$slot_r)"
  fails=$((fails + 1))
fi
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/slot/s.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/slot/pos.log" 2>&1 ); then
    echo "ok   pointer slots: pa[0] = 10 and pa[1] = 50 (measurement)"
  else
    echo "FAIL: pointer-slot element bind measurement"; tail -8 "$WORK/slot/pos.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/slot/s_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/slot/neg.log" 2>&1 ); then
    echo "FAIL: pointer-slot negative control PASSED (the arm cannot fail)"
    fails=$((fails + 1))
  else
    echo "ok   pointer slots: negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
fi
# classic 14 carries the same 14-bit store path (2-byte slots, stride 2)
sed -e 's/include 16f18857/include 16f887/' -e '/pragma target clock/d' \
    -e '/pragma target WDT/d' -e 's/@picsim_validate/@gpsim_validate/g' \
    "$WORK/slot/s.jal" > "$WORK/slot/s14.jal"
sed 's/expect byte v_s0 10/expect byte v_s0 11/' "$WORK/slot/s14.jal" \
    > "$WORK/slot/s14_neg.jal"
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK/slot" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/slot/s14.jal" > pos14.log 2>&1 ); then
    echo "ok   classic 14 pointer slots: pa[0] = 10 and pa[1] = 50"
  else
    echo "FAIL: classic 14 pointer-slot measurement"; tail -8 "$WORK/slot/pos14.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$WORK/slot" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/slot/s14_neg.jal" > neg14.log 2>&1 ); then
    echo "FAIL: classic 14 pointer-slot negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   classic 14 pointer slots: negative control fails as it must"
  fi
else
  echo "skip gpsim not available"
fi

# --- G3: a pointer SLOT bound to a CHUNKED element -----------------------
# The survey's §5.2 shape: `pa[0] is big[100]; s0 = pa[0]` where `big` is a
# 200-byte array the allocator splits.  The slot deref used the ARRAY's
# unescaped bind walk (the subscript it added was the slot BYTE offset, not a
# pointee offset) and resolved object offset 0, so it read big[0] and never
# consulted the slot's runtime value.  Element 100 is deliberately in a LATER
# chunk, so the slot store must also carry the element's chunk-resolved
# PHYSICAL address.  The defect is not fat-specific (it reproduces gate off),
# so both gates are measured; the 14H and classic-14 encodings differ.
G3W="$REPO/sims/gpsim/16f887_gpsim_pointer_slot_chunked_elem.jal"
mkdir -p "$WORK/g3"
# the tracked witness is the classic-14 form; the 14H form swaps the device
# and the validate family.  `pragma target clock` is REQUIRED: without it
# picsim never executes the program and every expect "passes" vacuously.
sed -e 's/include 16f887/include 16f18857/' \
    -e 's/@gpsim_validate/@picsim_validate/g' \
    -e '/^include 16f18857$/a pragma target clock 4_000_000\npragma target WDT DISABLED' \
    "$G3W" > "$WORK/g3/g3_14h.jal"
cp "$WORK/g3/g3_14h.jal" "$WORK/g3/g3_14h_off.jal"
sed 's/expect byte v_s0 101/expect byte v_s0 102/' "$WORK/g3/g3_14h.jal" \
    > "$WORK/g3/g3_14h_neg.jal"
cp "$G3W" "$WORK/g3/g3_14.jal"
cp "$G3W" "$WORK/g3/g3_14_off.jal"
sed 's/expect byte v_s0 101/expect byte v_s0 102/' "$G3W" \
    > "$WORK/g3/g3_14_neg.jal"

( cd "$WORK/g3" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" g3_14h.jal >on14h.log 2>&1 )
( cd "$WORK/g3" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" g3_14h_off.jal >off14h.log 2>&1 )
if grep -q "0 errors" "$WORK/g3/on14h.log" && grep -q "0 errors" "$WORK/g3/off14h.log"; then
  echo "ok   G3 14H: the chunked-slot witness compiles on both gates"
else
  echo "FAIL: G3 14H build"; tail -4 "$WORK/g3/on14h.log" "$WORK/g3/off14h.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi
( cd "$WORK/g3" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" g3_14.jal >on14.log 2>&1 )
( cd "$WORK/g3" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" g3_14_off.jal >off14.log 2>&1 )
if grep -q "0 errors" "$WORK/g3/on14.log" && grep -q "0 errors" "$WORK/g3/off14.log"; then
  echo "ok   G3 classic 14: the chunked-slot witness compiles on both gates"
else
  echo "FAIL: G3 classic 14 build"; tail -4 "$WORK/g3/on14.log" "$WORK/g3/off14.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi
# encoding (gate-on 14H): the slot must hold the CHUNK-RESOLVED address, not
# the linear object base + 100, and the deref must load the SLOT -- not call
# big's chunk walk at object offset 0.
g3_big=$(awk '$1 == "v_big" { print $3 }' "$WORK/g3/g3_14h.asm" 2>/dev/null)
g3_stored=$(awk '/movwf +v_pa[ ]*$/ { print prev; exit } { prev = $0 }' \
    "$WORK/g3/g3_14h.asm" 2>/dev/null | awk '{ print $2 }')
g3_add=$(awk '/pa\[0\] is big\[100\]/ { f = 1 } /s0 = pa\[0\]/ { f = 0 } \
    f && /addwf +v_pa,w/ { n++ } END { print n + 0 }' \
    "$WORK/g3/g3_14h.asm" 2>/dev/null)
if [ -n "${g3_big:-}" ] && [ -n "${g3_stored:-}" ] \
    && [ "${g3_add:-1}" -eq 0 ] \
    && [ "$g3_stored" -ne "$(( (g3_big + 100) & 0xff ))" ]; then
  echo "ok   G3: the slot stores one chunk-resolved literal ($g3_stored, not big+100)"
else
  echo "FAIL: G3 slot store is the linear base + add (big=$g3_big stored=$g3_stored adds=$g3_add)"
  fails=$((fails + 1))
fi
g3_lin=$(grep -A1 'movf *v_pa,w' "$WORK/g3/g3_14h.asm" 2>/dev/null \
    | grep -c 'movwf *v__fsr0l')
g3_walk=$(awk '/pa\[0\] is big\[100\]/ { f = 1 } f && /call.*pic_chunkwalk_big/ { n++ } END { print n + 0 }' \
    "$WORK/g3/g3_14h.asm" 2>/dev/null)
if [ "${g3_lin:-0}" -ge 1 ] && [ "${g3_walk:-0}" -eq 0 ]; then
  echo "ok   G3: the deref reads the SLOT value (no object-offset-0 bind walk)"
else
  echo "FAIL: G3 deref shape (slot_reads=$g3_lin bind_walk_calls=$g3_walk)"
  fails=$((fails + 1))
fi

# G3 control: a slot whose POINTEE is an ARRAY (`pa[0] is big`, then
# `pa[0][100]`) now dispatches under the fat engine (design D4): the slot
# holds `(id(big), 0)` and the deref adds the constant subscript 100.
# Gate-off keeps the walked-bind path (and the NOT_FAT_BIND row).  Slot 1 is
# off by the slot stride on HEAD too, so only slot 0 is measured (the
# pre-existing gap for k>0 stays recorded).
cat > "$WORK/g3/g3_arr.jal" <<'JALG3A'
-- @picsim_validate begin
-- steps: 2000000
-- frequency_hz: auto
-- expect byte v_g0 101
-- @picsim_validate end
include 16f18857
pragma target clock 4_000_000
pragma target WDT DISABLED
var volatile byte big[200]
var byte i
var volatile byte g0 = 0
var pointer pa[2] as var byte[200]
for 200 using i loop
  big[i] = i + 1
end loop
pa[0] is big
g0 = pa[0][100]
forever loop
end loop
JALG3A
sed 's/expect byte v_g0 101/expect byte v_g0 102/' "$WORK/g3/g3_arr.jal" \
    > "$WORK/g3/g3_arr_neg.jal"
( cd "$WORK/g3" && JALV2_FAT_POINTERS=1 "$CC" "${INC[@]}" g3_arr.jal >arr_on.log 2>&1 )
( cd "$WORK/g3" && env -u JALV2_FAT_POINTERS "$CC" "${INC[@]}" g3_arr.jal >arr_off.log 2>&1 )
if grep -q "0 errors" "$WORK/g3/arr_on.log" && grep -q "0 errors" "$WORK/g3/arr_off.log"; then
  echo "ok   G3 control: the pointer-to-ARRAY slot compiles on both gates"
else
  echo "FAIL: G3 pointer-to-array slot build"; tail -4 "$WORK/g3/arr_on.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/g3/g3_14h.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/g3/pos14h.log" 2>&1 ); then
    echo "ok   G3 14H gate on: pa[0] (chunk 2) = 101, pa[1] = 50 (measurement)"
  else
    echo "FAIL: G3 14H gate-on measurement"; tail -8 "$WORK/g3/pos14h.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && env -u JALV2_FAT_POINTERS \
      python3 sims/picsim_validate_engine.py "$WORK/g3/g3_14h.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/g3/offpos14h.log" 2>&1 ); then
    echo "ok   G3 14H gate off: pa[0] = 101, pa[1] = 50 (not fat-specific)"
  else
    echo "FAIL: G3 14H gate-off measurement"; tail -8 "$WORK/g3/offpos14h.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/g3/g3_arr.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/g3/posarr.log" 2>&1 ); then
    echo "ok   G3 control 14H: a pointer-to-ARRAY slot still reads pa[0][100] = 101"
  else
    echo "FAIL: G3 pointer-to-array slot measurement"; tail -8 "$WORK/g3/posarr.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && env -u JALV2_FAT_POINTERS \
      python3 sims/picsim_validate_engine.py "$WORK/g3/g3_arr.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/g3/offposarr.log" 2>&1 ); then
    echo "ok   G3 control 14H gate off: the pointer-to-ARRAY slot reads 101"
  else
    echo "FAIL: G3 pointer-to-array slot gate-off measurement"; tail -8 "$WORK/g3/offposarr.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/g3/g3_arr_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/g3/negarr.log" 2>&1 ); then
    echo "FAIL: G3 pointer-to-array control negative PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 pointer-to-array control negative fails as it must"
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/g3/g3_14h_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/g3/neg14h.log" 2>&1 ); then
    echo "FAIL: G3 14H negative control PASSED (the arm cannot fail)"
    fails=$((fails + 1))
  else
    echo "ok   G3 14H negative control fails as it must"
  fi
  if ( cd "$REPO" && env -u JALV2_FAT_POINTERS \
      python3 sims/picsim_validate_engine.py "$WORK/g3/g3_14h_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/g3/offneg14h.log" 2>&1 ); then
    echo "FAIL: G3 14H gate-off negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 14H gate-off negative control fails as it must"
  fi
else
  echo "skip picsim engine not available"
fi
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK/g3" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/g3/g3_14.jal" > pos14.log 2>&1 ); then
    echo "ok   G3 classic 14 gate on: pa[0] (chunk 2) = 101, pa[1] = 50"
  else
    echo "FAIL: G3 classic 14 gate-on measurement"; tail -8 "$WORK/g3/pos14.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$WORK/g3" && env -u JALV2_FAT_POINTERS \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/g3/g3_14.jal" > offpos14.log 2>&1 ); then
    echo "ok   G3 classic 14 gate off: pa[0] = 101, pa[1] = 50"
  else
    echo "FAIL: G3 classic 14 gate-off measurement"; tail -8 "$WORK/g3/offpos14.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$WORK/g3" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/g3/g3_14_neg.jal" > neg14.log 2>&1 ); then
    echo "FAIL: G3 classic 14 negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 classic 14 negative control fails as it must"
  fi
else
  echo "skip gpsim not available"
fi

# --- G3 S1: a pointer SLOT bound to a RUNTIME element offset --------------
# `pa[0] is big[i]` with a runtime `i` selecting a later chunk.  Under the fat
# engine the slot holds the tagged id + object-relative offset (the bind adds
# the runtime index; no bind-time chunk walk), and the deref dispatch resolves
# the chunk.  Two cores: 14H (picsim) and classic 14 (gpsim).
S1_14H="$REPO/sims/picsim/16f18857_picsim_pointer_slot_runtime_ofs.jal"
S1_14="$REPO/sims/gpsim/16f887_gpsim_pointer_slot_runtime_ofs.jal"
mkdir -p "$WORK/s1"
sed 's/expect byte v_got 42/expect byte v_got 43/' "$S1_14H" \
    > "$WORK/s1/s1_14h_neg.jal"
sed 's/expect byte v_got 42/expect byte v_got 43/' "$S1_14" \
    > "$WORK/s1/s1_14_neg.jal"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$S1_14H" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s1/pos14h.log" 2>&1 ); then
    echo "ok   G3 S1 14H gate on: a runtime element offset (chunk 2) reads 42"
  else
    echo "FAIL: G3 S1 14H measurement"; tail -8 "$WORK/s1/pos14h.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && JALV2_FAT_POINTERS=1 \
      python3 sims/picsim_validate_engine.py "$WORK/s1/s1_14h_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s1/neg14h.log" 2>&1 ); then
    echo "FAIL: G3 S1 14H negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 S1 14H negative control fails as it must"
  fi
else
  echo "skip picsim engine not available (G3 S1 14H)"
fi
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK/s1" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$S1_14" > pos14.log 2>&1 ); then
    echo "ok   G3 S1 classic 14 gate on: a runtime element offset reads 42"
  else
    echo "FAIL: G3 S1 classic 14 measurement"; tail -8 "$WORK/s1/pos14.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$WORK/s1" && JALV2_FAT_POINTERS=1 \
      bash "$REPO/sims/jal_gpsim_validate.sh" --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/s1/s1_14_neg.jal" > neg14.log 2>&1 ); then
    echo "FAIL: G3 S1 classic 14 negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 S1 classic 14 negative control fails as it must"
  fi
  # encoding: the bind adds the runtime index and never walks the target's
  # chunks; the deref carries the fat dispatch.
  ( cd "$WORK/s1" && JALV2_FAT_POINTERS=1 "$CC" \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      -no-codfile -asm s1_14.asm -hex s1_14.hex "$S1_14" > enc.log 2>&1 )
  s1_add=$(awk '/pa\[0\] is big\[i\]/ { f = 1 } /got = pa\[0\]/ { f = 0 } \
      f && /addwf/ { a = 1 } f && /v_i/ { b = 1 } \
      END { print ((a && b) ? 1 : 0) }' \
      "$WORK/s1/s1_14.asm" 2>/dev/null)
  s1_walk=$(awk '/pa\[0\] is big\[i\]/ { f = 1 } /got = pa\[0\]/ { f = 0 } \
      f && /call.*pic_chunkwalk_big/ { n++ } END { print n + 0 }' \
      "$WORK/s1/s1_14.asm" 2>/dev/null)
  s1_disp=$(awk '/got = pa\[0\]/ { f = 1 } f && /call.*pic_fat_seg_addr/ { n++ } \
      END { print n + 0 }' "$WORK/s1/s1_14.asm" 2>/dev/null)
  if [ "${s1_add:-0}" -ge 1 ] && [ "${s1_walk:-1}" -eq 0 ] \
      && [ "${s1_disp:-0}" -ge 1 ]; then
    echo "ok   G3 S1 encoding: runtime add in the bind, dispatch at the deref"
  else
    echo "FAIL: G3 S1 encoding (runtime_add=$s1_add bind_walk=$s1_walk dispatch=$s1_disp)"
    fails=$((fails + 1))
  fi
else
  echo "skip gpsim not available (G3 S1 classic 14)"
fi

# --- G3 S2: a pointer SLOT at a RUNTIME index -----------------------------
# `pa[k] is big[100]` with runtime k.  The bind computes the value into a
# scratch, stages FSR = &pa + k*psz, and copies the bytes through INDF (the
# runtime slot index used to be lowered as slot 0).  The non-fat linear value
# takes the same scratch+copy path, so it is measured too.
S2_14H="$REPO/sims/picsim/16f18857_picsim_pointer_slot_runtime_index.jal"
S2_14="$REPO/sims/gpsim/16f887_gpsim_pointer_slot_runtime_index.jal"
S2_LIN="$REPO/sims/picsim/16f18857_picsim_pointer_slot_runtime_index_linear.jal"
mkdir -p "$WORK/s2"
sed 's/expect byte v_got 42/expect byte v_got 43/' "$S2_14H" \
    > "$WORK/s2/s2_14h_neg.jal"
sed 's/expect byte v_got 42/expect byte v_got 43/' "$S2_14" \
    > "$WORK/s2/s2_14_neg.jal"
sed 's/expect byte v_got 50/expect byte v_got 51/' "$S2_LIN" \
    > "$WORK/s2/s2_lin_neg.jal"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$S2_14H" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s2/pos14h.log" 2>&1 ); then
    echo "ok   G3 S2 14H: a runtime slot index receives the fat bind"
  else
    echo "FAIL: G3 S2 14H measurement"; tail -8 "$WORK/s2/pos14h.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$WORK/s2/s2_14h_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s2/neg14h.log" 2>&1 ); then
    echo "FAIL: G3 S2 14H negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 S2 14H negative control fails as it must"
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$S2_LIN" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s2/poslin.log" 2>&1 ); then
    echo "ok   G3 S2 14H: a runtime slot index receives a linear bind too"
  else
    echo "FAIL: G3 S2 linear measurement"; tail -8 "$WORK/s2/poslin.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$WORK/s2/s2_lin_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s2/neglin.log" 2>&1 ); then
    echo "FAIL: G3 S2 linear negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 S2 linear negative control fails as it must"
  fi
else
  echo "skip picsim engine not available (G3 S2)"
fi
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK/s2" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$S2_14" > pos14.log 2>&1 ); then
    echo "ok   G3 S2 classic 14: a runtime slot index receives the fat bind"
  else
    echo "FAIL: G3 S2 classic 14 measurement"; tail -8 "$WORK/s2/pos14.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$WORK/s2" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/s2/s2_14_neg.jal" > neg14.log 2>&1 ); then
    echo "FAIL: G3 S2 classic 14 negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 S2 classic 14 negative control fails as it must"
  fi
  # encoding: the bind writes the scratch through INDF at &pa + k*psz; it must
  # not write v_pa directly (the old slot-0 store), and the deref dispatches.
  ( cd "$WORK/s2" && "$CC" \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      -no-codfile -asm s2_14.asm -hex s2_14.hex "$S2_14" > enc.log 2>&1 )
  s2_ind=$(awk '/pa\[k\] is big\[100\]/ { f = 1 } /got = pa\[0\]|got = pa\[1\]/ { f = 0 } \
      f && /movwf +v__ind/ { n++ } END { print n + 0 }' \
      "$WORK/s2/s2_14.asm" 2>/dev/null)
  s2_direct=$(awk '/pa\[k\] is big\[100\]/ { f = 1 } /got = pa\[0\]|got = pa\[1\]/ { f = 0 } \
      f && /movwf +v_pa/ { n++ } END { print n + 0 }' \
      "$WORK/s2/s2_14.asm" 2>/dev/null)
  s2_disp=$(awk '/got = pa\[1\]/ { f = 1 } f && /call.*pic_fat_seg_addr/ { n++ } \
      END { print n + 0 }' "$WORK/s2/s2_14.asm" 2>/dev/null)
  if [ "${s2_ind:-0}" -ge 1 ] && [ "${s2_direct:-1}" -eq 0 ] \
      && [ "${s2_disp:-0}" -ge 1 ]; then
    echo "ok   G3 S2 encoding: INDF store at the runtime slot, no slot-0 write"
  else
    echo "FAIL: G3 S2 encoding (indf=$s2_ind direct=$s2_direct dispatch=$s2_disp)"
    fails=$((fails + 1))
  fi
else
  echo "skip gpsim not available (G3 S2 classic 14)"
fi

# --- G3 S3: a pointer-to-ARRAY slot with a RUNTIME pointee subscript ------
# `pa[0] is big`, then `pa[0][i]` with a runtime `i`.  Under the fat engine
# the slot holds the ordinary fat pointer `(id(big), 0)` (design D4), so the
# deref dispatches on the slot's tag and adds the runtime subscript.  The
# witness also carries the loaded-pointer arm: `p = pa[0]` copies the stored
# fat pointer, so `s = p[37]` reads big[37].
S3_14H="$REPO/sims/picsim/16f18857_picsim_pointer_slot_array_runtime.jal"
S3_14="$REPO/sims/gpsim/16f887_gpsim_pointer_slot_array_runtime.jal"
mkdir -p "$WORK/s3"
sed 's/expect byte v_g 101/expect byte v_g 102/' "$S3_14H" \
    > "$WORK/s3/s3_14h_neg.jal"
sed 's/expect byte v_g 101/expect byte v_g 102/' "$S3_14" \
    > "$WORK/s3/s3_14_neg.jal"
if [ -x "$PICSIM" ] && [ -f "$REPO/sims/picsim_validate_engine.py" ]; then
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$S3_14H" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s3/pos14h.log" 2>&1 ); then
    echo "ok   G3 S3 14H: a runtime pointee subscript through an array slot reads 101"
  else
    echo "FAIL: G3 S3 14H measurement"; tail -8 "$WORK/s3/pos14h.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$WORK/s3/s3_14h_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/s3/neg14h.log" 2>&1 ); then
    echo "FAIL: G3 S3 14H negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 S3 14H negative control fails as it must"
  fi
else
  echo "skip picsim engine not available (G3 S3)"
fi
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK/s3" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$S3_14" > pos14.log 2>&1 ); then
    echo "ok   G3 S3 classic 14: a runtime pointee subscript reads 101"
  else
    echo "FAIL: G3 S3 classic 14 measurement"; tail -8 "$WORK/s3/pos14.log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if ( cd "$WORK/s3" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      "$WORK/s3/s3_14_neg.jal" > neg14.log 2>&1 ); then
    echo "FAIL: G3 S3 classic 14 negative control PASSED"
    fails=$((fails + 1))
  else
    echo "ok   G3 S3 classic 14 negative control fails as it must"
  fi
  # encoding: the runtime deref dispatches on the slot's tag (seg_addr) and
  # the runtime index joins the object offset; the loaded-pointer copy reads
  # the slot's bytes into p (movf v_pa -> movwf v_p), not the slot address.
  ( cd "$WORK/s3" && "$CC" \
      -s "$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared" \
      -no-codfile -asm s3_14h.asm -hex s3_14h.hex "$S3_14H" > enc.log 2>&1 )
  s3_disp=$(awk '/g = pa\[0\]\[i\]/ { f = 1 } /forever/ { f = 0 } \
      f && /call.*pic_fat_seg_addr/ { n++ } END { print n + 0 }' \
      "$WORK/s3/s3_14h.asm" 2>/dev/null)
  s3_add=$(awk '/g = pa\[0\]\[i\]/ { f = 1 } /forever/ { f = 0 } \
      f && /addwf.*pic_chunk_t/ { n++ } END { print n + 0 }' \
      "$WORK/s3/s3_14h.asm" 2>/dev/null)
  s3_copy=$(awk '/p = pa\[0\]/ { f = 1 } /s = p\[37\]/ { f = 0 } \
      f && /movf +v_pa,/ { n++ } END { print n + 0 }' \
      "$WORK/s3/s3_14h.asm" 2>/dev/null)
  if [ "${s3_disp:-0}" -ge 1 ] && [ "${s3_add:-0}" -ge 1 ] \
      && [ "${s3_copy:-0}" -ge 1 ]; then
    echo "ok   G3 S3 encoding: the runtime subscript dispatches and the copy reads the slot"
  else
    echo "FAIL: G3 S3 encoding (dispatch=$s3_disp add=$s3_add copy=$s3_copy)"
    fails=$((fails + 1))
  fi
else
  echo "skip gpsim not available (G3 S3 classic 14)"
fi

if [ $fails -eq 0 ]; then echo; echo "PASS: fat-pointer deref pin"; exit 0; fi
echo; echo "FAIL: $fails check(s)"; exit 1

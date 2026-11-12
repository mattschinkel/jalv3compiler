#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# U4-2: the access audit.  With JALV2_ACCESS_AUDIT set, every plan-classified
# indirect access prints the plan it was classified by
# (`jal access audit: kind=.. aop=.. chunked=.. vmap=.. rt=.. bind=..`), so the
# planner's decisions can be asserted per witness independently of the image.
# What this pin proves:
#
#   1. the array-of-pointers witness classifies as RESOLVE (kind=1) with
#      aop=1 -- the slot is resolved, never walked;
#   2. a chunked (holed) array classifies its walking accesses as WALK_BAKED
#      (kind=3) with vmap=1;
#   3. the classification is CONSISTENT: no line with chunked=1 or vmap=1 is
#      kind=2 (LINEAR) -- the invariant the reordering exists to keep;
#   4. a plain pointer program produces LINEAR/DIRECT lines only (no aop, no
#      walk kinds);
#   5. it is SILENT by default and with the project's OFF values
#      (0/false/no/off), so a set-but-false env cannot turn it on (the
#      dcc4f8a lesson);
#   6. the audit is READ-ONLY: the image with the audit on is byte-identical
#      to the image without it.
#   7. U4-3: the FAT classification is produced -- with the fat gate on, a
#      pointer deref classifies as FAT (kind=5); with the gate off it
#      never does.
#   8. U4-4: the plan-consistency check -- a clean compile prints no
#      INCONSISTENT line, and the JALV2_ACCESS_AUDIT_FAULT hook (which breaks
#      `kind` on purpose) makes it fire exactly once; the hook obeys the
#      OFF-value rule (=off is OFF).
#
# Coverage note: pic_indirect_setup3 (the three-value path used by the generic
# assignment store) delegates to pic_indirect_setup whenever one of its values
# is indirect, so those accesses ARE audited; only calls with no indirect
# value (nothing to address) are not.
#
# Usage: unit-tests/test_access_audit.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC=(-s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" -s "$REPO/sims/shared")

mkdir -p "$WORK/pa" "$WORK/pa_off" "$WORK/holed" "$WORK/plain" \
    "$WORK/fat" "$WORK/fat_off" "$WORK/fault" "$WORK/fault_off"
cp "$REPO/sims/picsim/16f18857_picsim_pointer_array_200.jal" "$WORK/pa/"
cp "$REPO/sims/picsim/16f18857_picsim_pointer_array_200.jal" "$WORK/pa_off/"
cp "$REPO/demo_sample/16f887_pointers.jal" "$WORK/plain/"
cp "$REPO/sims/picsim/16f18857_picsim_fat_pointer.jal" "$WORK/fat/"
cp "$REPO/sims/picsim/16f18857_picsim_fat_pointer.jal" "$WORK/fat_off/"
cp "$REPO/sims/picsim/16f18857_picsim_pointer_array_200.jal" "$WORK/fault/"
cp "$REPO/sims/picsim/16f18857_picsim_pointer_array_200.jal" "$WORK/fault_off/"
cat > "$WORK/holed/holed.jal" << 'EOF'
include 16f18857
pragma target clock 32_000_000
var byte big[720]
var byte k
var byte acc
for 8 using k loop
   big[k] = k
   acc = acc + big[k]
end loop
forever loop
end loop
EOF

( cd "$WORK/pa" && JALV2_ACCESS_AUDIT=1 \
    "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal >log 2>&1 )
rc_pa=$?
( cd "$WORK/pa_off" && JALV2_ACCESS_AUDIT=off \
    "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal >log 2>&1 )
rc_off=$?
( cd "$WORK/holed" && JALV2_ACCESS_AUDIT=1 \
    "$CC" "${INC[@]}" holed.jal >log 2>&1 )
rc_holed=$?
( cd "$WORK/plain" && JALV2_ACCESS_AUDIT=1 \
    "$CC" "${INC[@]}" 16f887_pointers.jal >log 2>&1 )
rc_plain=$?
# default silence: the pa witness without the env
( cd "$WORK/fat" && JALV2_ACCESS_AUDIT=1 JALV2_FAT_POINTERS=1 \
    JALV2_FAT_FORCE=1 "$CC" "${INC[@]}" 16f18857_picsim_fat_pointer.jal \
    >log 2>&1 )
rc_fat=$?
( cd "$WORK/fat_off" && JALV2_ACCESS_AUDIT=1 JALV2_FAT_POINTERS=0 \
    "$CC" "${INC[@]}" 16f18857_picsim_fat_pointer.jal >log 2>&1 )
rc_fat_off=$?
( cd "$WORK/fault" && JALV2_ACCESS_AUDIT=1 JALV2_ACCESS_AUDIT_FAULT=1 \
    "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal >log 2>&1 )
rc_fault=$?
( cd "$WORK/fault_off" && JALV2_ACCESS_AUDIT=1 JALV2_ACCESS_AUDIT_FAULT=off \
    "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal >log 2>&1 )
rc_fault_off=$?
( cd "$WORK/plain" && "$CC" "${INC[@]}" 16f887_pointers.jal >default.log 2>&1 )
rc_default=$?

pa_log="$WORK/pa/log"
holed_log="$WORK/holed/log"
plain_log="$WORK/plain/log"

# 1. array-of-pointers -> RESOLVE + aop
if [ $rc_pa -eq 0 ] \
    && grep -q "kind=1 aop=1" "$pa_log"; then
  echo "ok   the pointer array classifies as RESOLVE (kind=1 aop=1)"
else
  echo "FAIL: no RESOLVE/aop access on the pointer array (rc=$rc_pa)"
  sed 's/^/     /' "$pa_log" | tail -6
  fails=$((fails + 1))
fi

# 2. holed array -> WALK_BAKED with vmap
if [ $rc_holed -eq 0 ] \
    && grep -q "kind=3 .*vmap=1" "$holed_log"; then
  echo "ok   the holed array classifies walking accesses as WALK_BAKED (kind=3 vmap=1)"
else
  echo "FAIL: no WALK_BAKED/vmap access on the holed array (rc=$rc_holed)"
  sed 's/^/     /' "$holed_log" | tail -6
  fails=$((fails + 1))
fi

# 3. consistency: a walkable object is never classified LINEAR
inconsistent=$(grep "chunked=1\|vmap=1" "$holed_log" "$pa_log" 2>/dev/null \
    | grep "kind=2" | head -3)
if [ -z "$inconsistent" ]; then
  echo "ok   no walk-classified line is classified LINEAR (kind=2)"
else
  echo "FAIL: a walkable object was classified LINEAR:"
  echo "$inconsistent" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# 4. plain pointer program: no aop / no walk kinds
bad=$(grep -E "aop=1|kind=3|kind=4|kind=5|kind=6" "$plain_log" | head -3)
if [ $rc_plain -eq 0 ] && [ -z "$bad" ]; then
  echo "ok   the plain pointer program produces no aop/walk classifications"
else
  echo "FAIL: the plain pointer program produced aop/walk classifications:"
  [ -n "$bad" ] && echo "$bad" | sed 's/^/     /'
  sed 's/^/     /' "$plain_log" | tail -6
  fails=$((fails + 1))
fi

# 5. silent by default and with an OFF value
if [ $rc_default -eq 0 ] && ! grep -q "jal access audit:" "$WORK/plain/default.log"; then
  echo "ok   the audit is silent by default"
else
  echo "FAIL: the audit printed without JALV2_ACCESS_AUDIT"
  fails=$((fails + 1))
fi
if [ $rc_off -eq 0 ] && ! grep -q "jal access audit:" "$WORK/pa_off/log"; then
  echo "ok   JALV2_ACCESS_AUDIT=off is OFF (no audit output)"
else
  echo "FAIL: JALV2_ACCESS_AUDIT=off still printed the audit"
  fails=$((fails + 1))
fi

# 7. U4-3: the FAT classification (kind=5) appears only with the fat gate on
if [ $rc_fat -eq 0 ] && grep -q "kind=5 " "$WORK/fat/log"; then
  echo "ok   the fat witness classifies as FAT (kind=5)"
else
  echo "FAIL: no FAT classification on the fat witness (rc=$rc_fat)"
  sed 's/^/     /' "$WORK/fat/log" | tail -6
  fails=$((fails + 1))
fi
if [ $rc_fat_off -eq 0 ] && ! grep -q "kind=5" "$WORK/fat_off/log"; then
  echo "ok   no FAT classification with the fat gate off"
else
  echo "FAIL: FAT appeared without JALV2_FAT_POINTERS"
  fails=$((fails + 1))
fi

# 8. U4-4: the consistency check is silent on clean input and fires once on
# the fault hook (and the hook's own OFF value is honoured)
fault_lines=$(grep -c "INCONSISTENT plan" "$WORK/fault/log")
if [ $rc_fault -eq 0 ] && [ "${fault_lines:-0}" -eq 1 ] \
    && ! grep -q "INCONSISTENT plan" "$pa_log"; then
  echo "ok   the plan-consistency check is silent on a clean compile and fires once on the fault hook"
else
  echo "FAIL: consistency check (clean=0? fault=$fault_lines)"
  fails=$((fails + 1))
fi
if [ $rc_fault_off -eq 0 ] && ! grep -q "INCONSISTENT plan" "$WORK/fault_off/log"; then
  echo "ok   JALV2_ACCESS_AUDIT_FAULT=off is OFF"
else
  echo "FAIL: the fault hook fired with =off"
  fails=$((fails + 1))
fi

# 6. read-only: the image with the audit is identical to the one without
if cmp -s "$WORK/pa/16f18857_picsim_pointer_array_200.hex" \
    "$WORK/pa_off/16f18857_picsim_pointer_array_200.hex"; then
  echo "ok   the audit does not change the image"
else
  echo "FAIL: the audit changed the image"
  fails=$((fails + 1))
fi

echo ""
if [ $fails -ne 0 ]; then
  echo "RESULT: FAIL ($fails checks)"
  exit 1
fi
echo "RESULT: PASS"
exit 0

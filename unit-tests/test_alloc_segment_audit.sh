#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# P3a/P3b: the placement invariant audit and its capacity report.  The audit
# reads the object model after placement is final and checks geometry,
# containment, pins, object disjointness and coverage; P3b adds used/free and
# the largest free run per region to the report.  What this pin proves:
#
#   1. the audit SEES a chunked layout (owned>=1 on the 16F18857 pointer
#      array, whose 200 slots are six chunks);
#   2. it reports zero violations on that layout (and on the fat-pointer
#      witness, compiled with the feature gate on);
#   3. the capacity report is populated: data_free>0 and data_largest>0 (the
#      numbers are the allocator's own state, not a constant);
#   4. it can FIRE: JALV2_ALLOC_AUDIT_SELFTEST feeds the geometry checker a
#      deliberately broken layout and must report exactly 2 violations --
#      without this arm the rest of the pin could pass on a no-op checker;
#   5. it is READ-ONLY: the image with the audit enabled is byte-identical to
#      the image without it;
#   6. it is SILENT by default (no audit output when the env is unset);
#   7. U4: it runs on EVERY compile -- the JALV2_ALLOC_AUDIT_FAULT test hook
#      (one injected violation, no other env set) must produce the
#      placement-verdict error and a non-zero exit, which is impossible if the
#      audit were still gated behind JALV2_ALLOC_AUDIT.
#   8. A7: the RAM report prints on a clean compile and agrees with the audit.
#
# A pre-fix compiler has no audit at all, so arm 1 fails there: the pin is
# discriminating (jaldbg pin-check: new=PASS, old=FAIL).
#
# Usage: unit-tests/test_alloc_segment_audit.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC=(-s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" -s "$REPO/sims/shared")
W1="$REPO/sims/picsim/16f18857_picsim_pointer_array_200.jal"
W2="$REPO/sims/picsim/16f18857_picsim_fat_pointer.jal"

mkdir -p "$WORK/plain" "$WORK/audit" "$WORK/fat" "$WORK/self" "$WORK/fault"
cp "$W1" "$WORK/plain/"; cp "$W1" "$WORK/audit/"; cp "$W2" "$WORK/fat/"
cp "$W1" "$WORK/self/"; cp "$W1" "$WORK/fault/"

( cd "$WORK/plain" && "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal \
    >log 2>&1 )
rc_plain=$?
( cd "$WORK/audit" && JALV2_ALLOC_AUDIT=1 \
    "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal >log 2>&1 )
rc_audit=$?
( cd "$WORK/fat" && JALV2_ALLOC_AUDIT=1 JALV2_FAT_POINTERS=1 \
    JALV2_FAT_FORCE=1 \
    "$CC" "${INC[@]}" 16f18857_picsim_fat_pointer.jal >log 2>&1 )
rc_fat=$?
( cd "$WORK/self" && JALV2_ALLOC_AUDIT_SELFTEST=1 \
    "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal >log 2>&1 )
rc_self=$?
( cd "$WORK/fault" && JALV2_ALLOC_AUDIT_FAULT=1 \
    "$CC" "${INC[@]}" 16f18857_picsim_pointer_array_200.jal >log 2>&1 )
rc_fault=$?

field() { # $1 = log, $2 = field name -> last value
  grep -o "$2=[0-9]*" "$1" 2>/dev/null | tail -1 | cut -d= -f2
}

# 6. silent by default
if [ $rc_plain -eq 0 ] && ! grep -q "jal alloc audit:" "$WORK/plain/log"; then
  echo "ok   the audit is silent by default"
else
  echo "FAIL: the audit printed without JALV2_ALLOC_AUDIT"
  fails=$((fails + 1))
fi

# 7. always-on: the FAULT hook fails the compile with an error verdict
if [ $rc_fault -ne 0 ] \
    && grep -q "error: alloc: 1 placement invariant violation" "$WORK/fault/log" \
    && ! grep -q "^RAM:" "$WORK/fault/log"; then
  echo "ok   the audit fails the compile on a violation (no RAM line, no image)"
else
  echo "FAIL: the error verdict did not fail the compile (rc=$rc_fault)"
  sed 's/^/     /' "$WORK/fault/log" | tail -10
  fails=$((fails + 1))
fi

# 1 + 2 + 3. sees the chunked object, zero violations, populated report
sum=$(grep -o "objects=[0-9]* segments=[0-9]* owned=[0-9]* violations=[0-9]*" \
    "$WORK/audit/log" | tail -1)
owned=$(field "$WORK/audit/log" owned)
viol=$(field "$WORK/audit/log" violations)
dfree=$(field "$WORK/audit/log" data_free)
dlarge=$(field "$WORK/audit/log" data_largest)
sused=$(field "$WORK/audit/log" shared_used)
if [ $rc_audit -eq 0 ] && grep -q "0 errors" "$WORK/audit/log" \
    && [ -n "$sum" ]; then
  echo "ok   the chunked witness compiles and reports: $sum"
else
  echo "FAIL: the chunked witness did not produce an audit summary"
  sed 's/^/     /' "$WORK/audit/log" | tail -10
  fails=$((fails + 1))
fi
if [ "${owned:-0}" -ge 1 ]; then
  echo "ok   the audit sees an allocator-owned (chunked) object (owned=$owned)"
else
  echo "FAIL: the audit saw no allocator-owned (chunked) object"
  fails=$((fails + 1))
fi
if [ "${viol:-1}" -eq 0 ]; then
  echo "ok   the audit reports zero violations"
else
  echo "FAIL: the audit reports violations (violations=${viol:-?})"
  fails=$((fails + 1))
fi
if [ "${dfree:-0}" -gt 0 ] && [ "${dlarge:-0}" -gt 0 ] \
    && [ "${sused:-x}" != "x" ]; then
  echo "ok   capacity report: data_free=$dfree data_largest=$dlarge shared_used=$sused"
else
  echo "FAIL: the capacity report is missing or empty"
  fails=$((fails + 1))
fi
if grep "jal alloc audit:" "$WORK/audit/log" | grep -qv "objects="; then
  echo "FAIL: violation lines on a clean witness:"
  grep "jal alloc audit:" "$WORK/audit/log" | grep -v "objects=" | sed 's/^/     /'
  fails=$((fails + 1))
else
  echo "ok   no violation lines on the chunked witness"
fi

# 2b. the fat-pointer witness (feature gate on) is clean too
fowned=$(field "$WORK/fat/log" owned)
fviol=$(field "$WORK/fat/log" violations)
if [ $rc_fat -eq 0 ] && [ "${fowned:-0}" -ge 1 ] \
    && [ "${fviol:-1}" -eq 0 ]; then
  echo "ok   the fat witness is clean (owned=$fowned violations=$fviol)"
else
  echo "FAIL: the fat witness audit (rc=$rc_fat owned=${fowned:-?} violations=${fviol:-?})"
  sed 's/^/     /' "$WORK/fat/log" | tail -10
  fails=$((fails + 1))
fi

# 4. the checker can fire
if [ $rc_self -eq 0 ] \
    && grep -q "selftest violations=2 (expect 2)" "$WORK/self/log" \
    && grep -q "selftest: segments 0 .* and 1 .* overlap" "$WORK/self/log" \
    && grep -q "selftest: segment 2 has zero length" "$WORK/self/log"; then
  echo "ok   the selftest detects an injected overlap and a zero-length segment"
else
  echo "FAIL: the audit selftest did not detect the injected defects"
  grep "selftest" "$WORK/self/log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# 5. read-only
if [ $rc_plain -eq 0 ] && [ $rc_audit -eq 0 ] \
    && cmp -s "$WORK/plain/16f18857_picsim_pointer_array_200.hex" \
              "$WORK/audit/16f18857_picsim_pointer_array_200.hex"; then
  echo "ok   the audit does not change the image"
else
  echo "FAIL: the audit changed the image"
  fails=$((fails + 1))
fi

# 8. A7: the RAM report prints on a clean compile and agrees with the audit
if [ $rc_plain -eq 0 ] \
    && grep -q "^RAM: data used [0-9]*/[0-9]* (free [0-9]*, largest [0-9]*); shared used [0-9]*/[0-9]* (free [0-9]*, largest [0-9]*)" "$WORK/plain/log"; then
  echo "ok   the RAM report prints on a clean compile"
else
  echo "FAIL: the RAM report did not print on the clean witness (rc=$rc_plain)"
  fails=$((fails + 1))
fi
if [ $rc_audit -eq 0 ]; then
  ram_nums=$(grep "^RAM:" "$WORK/audit/log" | tail -1 \
    | sed -n 's/.*data used \([0-9]*\)\/[0-9]* (free \([0-9]*\), largest \([0-9]*\)); shared used \([0-9]*\)\/[0-9]* (free \([0-9]*\), largest \([0-9]*\)).*/\1 \2 \3 \4 \5 \6/p')
  set -- $ram_nums
  dused=$(field "$WORK/audit/log" data_used)
  sfree=$(field "$WORK/audit/log" shared_free)
  slarge=$(field "$WORK/audit/log" shared_largest)
  if [ "$1" = "$dused" ] && [ "$2" = "$dfree" ] && [ "$3" = "$dlarge" ] \
      && [ "$4" = "$sused" ] && [ "$5" = "$sfree" ] && [ "$6" = "$slarge" ]; then
    echo "ok   RAM report agrees with the audit"
  else
    echo "FAIL: RAM report disagrees with the audit (RAM: $ram_nums)"
    fails=$((fails + 1))
  fi
fi

echo ""
if [ $fails -ne 0 ]; then
  echo "RESULT: FAIL ($fails checks)"
  exit 1
fi
echo "RESULT: PASS"
exit 0

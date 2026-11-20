#!/usr/bin/env bash
#
# Step 4 gate: the FAT recipe dispatch, its fatal default, and the
# compile-time plan record across generations.  Integration-level: it runs
# the real compiler on the fat-pointer witness and on a plain sample.
#
# E1 (A11/A12/C3/C5) adds: the dispatch store's ENOMEM fault, the recorder
# write-fault, the store growth past 4,096 ids, and the recipe-row coverage
# report.
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC="$ROOT/bin/beta/jalv3-x86-64"
LIBS='demo_lib;third_party/jallib/lib;tests'
FAT="$ROOT/sims/picsim/16f18857_picsim_fat_pointer.jal"
PLAIN="$ROOT/demo_sample/16f887_stopwatch_probe.jal"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/access-recipes.XXXXXX") || exit 1
FAIL=0

fail() {
    echo "FAIL $1"
    FAIL=1
}

# 1. The FAT witness compiles with forced fat pointers.
if JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/a.hex" "$FAT" \
    >"$TMP/normal.log" 2>&1 && grep -q '^0 errors' "$TMP/normal.log"; then
    echo "PASS fat dispatch compiles"
else
    fail "fat dispatch compiles"
    tail -3 "$TMP/normal.log"
fi

# 2. The fatal default: a forced miss must fail with the internal error.
if JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 JALV2_ACCESS_RECIPE_FAULT=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/b.hex" "$FAT" \
    >"$TMP/fault.log" 2>&1; then
    fail "fault must fail the compile"
elif grep -q 'internal error: no access recipe' "$TMP/fault.log"; then
    echo "PASS fatal default fires"
else
    fail "fatal default message"
    tail -3 "$TMP/fault.log"
fi
# A19: a fatal recipe error must not leave an image behind.
if [ -f "$TMP/b.hex" ]; then
    fail "fatal recipe left a hex image"
else
    echo "PASS no image on fatal recipe"
fi

# 3. The fault hook must not touch a non-FAT compile.
if JALV2_ACCESS_RECIPE_FAULT=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/c.hex" "$PLAIN" \
    >"$TMP/plain.log" 2>&1 && grep -q '^0 errors' "$TMP/plain.log"; then
    echo "PASS non-FAT unaffected"
else
    fail "non-FAT unaffected"
    tail -3 "$TMP/plain.log"
fi

# 4. The plan record covers at least two layout generations (passes/trials).
JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 JALV2_ACCESS_RECORD=1 \
    JALV2_ACCESS_RECORD_OUT="$TMP/plan.log" \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/d.hex" "$FAT" >/dev/null 2>&1
GENS=$(grep -v '^#' "$TMP/plan.log" 2>/dev/null | cut -d, -f2 | sort -u | wc -l)
if [ "$GENS" -ge 2 ]; then
    echo "PASS plan generations: $GENS"
else
    fail "plan generations (got $GENS)"
fi

# 5. The bind recipe: a slot store is never a fat bind.
if JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" -s "$LIBS" -s "$ROOT/sims/shared" -no-codfile -hex "$TMP/e.hex" \
    "$ROOT/sims/picsim/16f18857_picsim_pointer_array_200.jal" \
    >"$TMP/bind.log" 2>&1 && grep -q '^0 errors' "$TMP/bind.log"; then
    echo "PASS slot bind stays linear"
else
    fail "slot bind stays linear"
    tail -3 "$TMP/bind.log"
fi

# 6. The stored-plan verify path (C2): clean passes, the fault hook fires.
PTRS="$ROOT/demo_sample/16f18857_pointers_1.jal"
if JALV2_ACCESS_VERIFY=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/f.hex" "$PTRS" \
    >"$TMP/verify.log" 2>&1 && grep -q '^0 errors' "$TMP/verify.log"; then
    echo "PASS stored-plan verify clean"
else
    fail "stored-plan verify clean"
    tail -3 "$TMP/verify.log"
fi
if JALV2_ACCESS_VERIFY=1 JALV2_ACCESS_VERIFY_FAULT=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/g.hex" "$PTRS" \
    >"$TMP/vfault.log" 2>&1; then
    fail "verify fault must fail"
elif grep -q 'internal error: access plan mismatch' "$TMP/vfault.log"; then
    echo "PASS stored-plan mismatch is fatal"
else
    fail "stored-plan mismatch message"
    tail -3 "$TMP/vfault.log"
fi

# 7. A11/C4: the dispatch store's allocation failure is fatal.
if JALV2_ACCESS_STORE_FAULT=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/h.hex" "$PTRS" \
    >"$TMP/storefault.log" 2>&1; then
    fail "store fault must fail the compile"
elif grep -q 'error: out of memory; compilation stopped' "$TMP/storefault.log"; then
    echo "PASS store ENOMEM is fatal"
else
    fail "store fault message"
    tail -3 "$TMP/storefault.log"
fi
if [ -f "$TMP/h.hex" ]; then
    fail "store fault left a hex image"
else
    echo "PASS no image on store ENOMEM"
fi

# 8. C5/S1: the recorder's write failure is an error while recording.
if JALV2_ACCESS_RECORD=1 JALV2_ACCESS_RECORD_OUT="$TMP/lf.log" \
    JALV2_ACCESS_LOG_FAULT=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/i.hex" "$PTRS" \
    >"$TMP/logfault.log" 2>&1; then
    fail "recorder fault must fail the compile"
elif grep -q 'internal error: access recorder write failed' \
    "$TMP/logfault.log"; then
    echo "PASS recorder write failure is an error"
else
    fail "recorder fault message"
    tail -3 "$TMP/logfault.log"
fi

# 9. C3: both stores grow past the old 4,096-id ceiling and stay consistent
# (VERIFY recomputes every cache hit and a mismatch is fatal).
if JALV2_ACCESS_STATS=1 JALV2_ACCESS_VERIFY=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/j.hex" "$PTRS" \
    >"$TMP/grow.log" 2>&1 && grep -q '^0 errors' "$TMP/grow.log"; then
    STATS=$(grep 'jal access stats' "$TMP/grow.log" | tail -1)
    VLIVE=$(printf '%s\n' "$STATS" | sed -n 's/.*value=\([0-9]*\)\/.*/\1/p')
    VCAP=$(printf '%s\n' "$STATS" | sed -n 's/.*value=[0-9]*\/\([0-9]*\).*/\1/p')
    RLIVE=$(printf '%s\n' "$STATS" | sed -n 's/.*var=\([0-9]*\)\/.*/\1/p')
    RCAP=$(printf '%s\n' "$STATS" | sed -n 's/.*var=[0-9]*\/\([0-9]*\).*/\1/p')
    if [ -n "$VCAP" ] && [ "$VCAP" -gt 4096 ] && [ "$RCAP" -gt 4096 ] \
        && [ "$VLIVE" -gt 0 ] && [ "$RLIVE" -gt 0 ]; then
        echo "PASS store growth: value=$VLIVE/$VCAP var=$RLIVE/$RCAP"
    else
        fail "store growth (value=$VLIVE/$VCAP var=$RLIVE/$RCAP)"
    fi
else
    fail "store growth compile"
    tail -3 "$TMP/grow.log"
fi

# 10. A12: the coverage report names the rows this compile exercised; the
# FAT witness must exercise row 0 (the FAT deref recipe).
if JALV2_ACCESS_COVERAGE=1 JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" -s "$LIBS" -no-codfile -hex "$TMP/k.hex" "$FAT" \
    >"$TMP/cov.log" 2>&1 && grep -q '^0 errors' "$TMP/cov.log"; then
    COV=$(grep 'jal access coverage' "$TMP/cov.log" | tail -1)
    CROWS=$(printf '%s\n' "$COV" | sed -n 's/.*rows=\([0-9]*\).*/\1/p')
    CBITS=$(printf '%s\n' "$COV" | sed -n 's/.*bitmap=\([0-9a-f]*\).*/\1/p')
    if [ "$CROWS" = 28 ] && [ -n "$CBITS" ] && (( 0x$CBITS & 1 )); then
        echo "PASS coverage report (rows=$CROWS fat row exercised)"
    else
        fail "coverage report ($COV)"
    fi
else
    fail "coverage compile"
    tail -3 "$TMP/cov.log"
fi

# 11. E2: -explain-access prints the decisions for one line.  The FAT
# witness's line 40 dereferences a fat pointer: the row names the dispatch
# recipe and the runtime-tag note follows; exit 0.
if JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" -s "$LIBS" -no-codfile -no-hex -quiet \
    -explain-access "sims/picsim/16f18857_picsim_fat_pointer.jal:40" \
    sims/picsim/16f18857_picsim_fat_pointer.jal \
    >"$TMP/explain.log" 2>&1 \
    && grep -q 'kind=FAT .*op=DEREF recipe=FAT_DEREF' "$TMP/explain.log" \
    && grep -q 'note: FAT dispatch' "$TMP/explain.log"; then
    echo "PASS explain-access prints the FAT deref"
else
    fail "explain-access FAT deref"
    tail -5 "$TMP/explain.log"
fi

# 12. A line with no access is exit 0 with the explanatory line.
if "$CC" -s "$LIBS" -no-codfile -no-hex -quiet \
    -explain-access "sims/picsim/16f18857_picsim_fat_pointer.jal:1" \
    sims/picsim/16f18857_picsim_fat_pointer.jal \
    >"$TMP/noaccess.log" 2>&1 \
    && grep -q 'no access at sims/picsim/16f18857_picsim_fat_pointer.jal:1' \
        "$TMP/noaccess.log"; then
    echo "PASS explain-access: no access is exit 0"
else
    fail "explain-access no-access case"
    tail -3 "$TMP/noaccess.log"
fi

rm -rf "$TMP"
exit "$FAIL"

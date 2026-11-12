#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# E3: the target-neutral access-recipe dispatcher (libcore) and the
# per-back-end table self-checks.
#
#   1. the pure-libcore dispatcher test: matching (exact facts, masked
#      facts, ANY kind, operation equality, table order) and the mask
#      rule that a row may require only bits it compares;
#   2. a classic compile runs the classic table's self-check (27 rows)
#      and reports no inconsistency;
#   3. a PIC32 compile runs the PIC32 pointer-read table's self-check
#      and reports no inconsistency;
#   4. the PIC32 pointer-read spike is byte-identical to the pinned
#      pre-spike image on the named fixture (E3's proof).  The pin lives
#      in plan-baselines/ and is generated from the spike base commit.
#
# Usage: unit-tests/test_access_recipe.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}

# GATE MODE (T3): these baselines are the ENGINE-OFF behavior; pin it
# explicitly so an ambient JALV2_FAT_POINTERS (e.g. after the default flip)
# cannot change them.  Mode-specific arms set their own value per command.
JALV2_FAT_POINTERS=0
export JALV2_FAT_POINTERS

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib;tests;sims/shared'

# --- 1. the dispatcher ---
if ${CC_CC:-cc} -ansi -pedantic -Wall -Wextra -Werror \
    -I "$REPO/src/libcore" -o "$WORK/recipe" \
    "$REPO/unit-tests/test_access_recipe.c" \
    "$REPO/src/libcore/access_recipe.c" > "$WORK/cc.log" 2>&1; then
  if "$WORK/recipe" > "$WORK/run.log" 2>&1; then
    echo "ok   dispatcher: match, mask, order, dispatch and consistency"
  else
    echo "FAIL: dispatcher assertions failed"
    sed 's/^/     /' "$WORK/run.log"
    fails=$((fails + 1))
  fi
else
  echo "FAIL: dispatcher harness did not build"
  sed 's/^/     /' "$WORK/cc.log"
  fails=$((fails + 1))
fi

# --- 2. the classic table's self-check runs and passes ---
if ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -hex "$WORK/c.hex" \
      demo_sample/16f18857_pointers_1.jal > "$WORK/classic.log" 2>&1 ) \
    && grep -q '^0 errors' "$WORK/classic.log" \
    && ! grep -q 'inconsistent access recipe' "$WORK/classic.log"; then
  echo "ok   classic recipe table self-check"
else
  echo "FAIL: classic recipe table self-check"
  tail -5 "$WORK/classic.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# --- 3. the PIC32 pointer-read table's self-check runs and passes ---
PIC32=sims/pic32/32mx270f256b_smoke_pointer.jal
if ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -hex "$WORK/p.hex" \
      "$PIC32" > "$WORK/pic32.log" 2>&1 ) \
    && grep -q '^0 errors' "$WORK/pic32.log" \
    && ! grep -q 'inconsistent access recipe' "$WORK/pic32.log"; then
  echo "ok   PIC32 pointer-read table self-check"
else
  echo "FAIL: PIC32 pointer-read table self-check"
  tail -5 "$WORK/pic32.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# --- 3b. the pointer-array shapes (the array + const-offset rows) ---
# These four samples were the ones a missing row broke during the spike:
# an array element with a CONSTANT base offset must take the slot-const row.
bad=0
for f in sims/pic32/32mz1024efh064_body_ptr_slot_pseudo.jal \
         sims/pic32/32mz1024efh064_body_ptr_slots.jal \
         sims/pic32/32mx270f256b_body_ptr_slot_rt.jal \
         sims/pic32/32mx270f256b_body_ptr_slots.jal; do
  if ! ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -hex "$WORK/s.hex" \
      "$f" > "$WORK/slot.log" 2>&1 ) \
      || ! grep -q '^0 errors' "$WORK/slot.log"; then
    echo "FAIL: $f does not compile"
    tail -3 "$WORK/slot.log" | sed 's/^/     /'
    fails=$((fails + 1))
    bad=$((bad + 1))
  fi
done
if [ "$bad" -eq 0 ]; then
  echo "ok   pointer-array slot rows (const and runtime offsets)"
fi

# --- 4. the spike is byte-identical to the pinned pre-spike image ---
PIN="$REPO/unit-tests/plan-baselines/32mx270f256b_smoke_pointer.hex"
if ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -hex "$WORK/now.hex" \
      "$PIC32" > "$WORK/proof.log" 2>&1 ); then
  if [ -f "$PIN" ]; then
    if cmp -s "$PIN" "$WORK/now.hex"; then
      echo "ok   PIC32 pointer-read spike is byte-identical"
    else
      echo "FAIL: the PIC32 spike image changed"
      fails=$((fails + 1))
    fi
  else
    echo "FAIL: missing spike baseline $PIN"
    echo "      regenerate it from the spike base commit:"
    echo "      jaldbg.py build <ref>; <ref-binary> -s '$LIBS' -no-codfile -hex $PIN $PIC32"
    fails=$((fails + 1))
  fi
else
  echo "FAIL: SPIKE fixture does not compile"
  tail -5 "$WORK/proof.log" | sed 's/^/     /'
  fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || exit 1
exit 0

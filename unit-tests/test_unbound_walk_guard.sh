#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# F5 section 4: the walk-path null guard.
#
# A CONDITIONALLY bound pointer that is dereferenced through the compile-time
# chunk walk never read the pointer's value, so an unbound pointer silently
# read the target's first byte (big[0] = 1) instead of trapping.  The walk now
# emits the same all-bytes-zero guard as the fat and slot paths (a local
# `goto self` hang).  The fat path already trapped; the walk path is the
# pre-existing gap this pin closes.
#
#   1. 14H and classic-14 trap arms: `g` stays 0 (the guard hangs);
#   2. the BOUND controls read big[100] = 101 (the guard does not break the
#      normal walk);
#   3. negative controls (wrong expectations) fail as they must;
#   4. the 14H trap's emitted code carries the guard (`iorwf v_p` + the
#      STATUS,Z test) and still resolves the chunk-resident literal.
#
# Usage: unit-tests/test_unbound_walk_guard.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
PICSIM="${PICSIM_DIR:-$REPO/../tmp/picsim-try/picsim}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib;sims/shared'
T14H="$REPO/sims/picsim/16f18857_picsim_unbound_walk_trap.jal"
B14H="$REPO/sims/picsim/16f18857_picsim_unbound_walk_bound.jal"
T14="$REPO/sims/gpsim/16f887_gpsim_unbound_walk_trap.jal"
B14="$REPO/sims/gpsim/16f887_gpsim_unbound_walk_bound.jal"

fail() {
  echo "FAIL: $1"
  shift
  sed 's/^/     /' "$@" 2>/dev/null | head -6
  fails=$((fails + 1))
}

sed 's/expect byte v_g 0/expect byte v_g 101/' "$T14H" > "$WORK/t14h_neg.jal"
sed 's/expect byte v_g 101/expect byte v_g 102/' "$B14H" > "$WORK/b14h_neg.jal"
sed 's/expect byte v_g 0/expect byte v_g 101/' "$T14" > "$WORK/t14_neg.jal"
sed 's/expect byte v_g 101/expect byte v_g 102/' "$B14" > "$WORK/b14_neg.jal"

if [ -f "$PICSIM/src/libpicsim.so" ] || [ -f "$PICSIM/src/libpicsim.a" ]; then
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$T14H" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/t14h.log" 2>&1 ); then
    echo "ok   unbound walk 14H: an unbound pointer traps (g stays 0)"
  else
    fail "unbound walk 14H trap" "$WORK/t14h.log"
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$B14H" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/b14h.log" 2>&1 ); then
    echo "ok   unbound walk 14H: the bound control reads big[100] = 101"
  else
    fail "unbound walk 14H bound control" "$WORK/b14h.log"
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$WORK/t14h_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/t14hneg.log" 2>&1 ); then
    fail "unbound walk 14H trap negative control PASSED"
  else
    echo "ok   unbound walk 14H trap negative control fails as it must"
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$WORK/b14h_neg.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/b14hneg.log" 2>&1 ); then
    fail "unbound walk 14H bound negative control PASSED"
  else
    echo "ok   unbound walk 14H bound negative control fails as it must"
  fi
else
  echo "skip picsim engine not available (unbound walk 14H)"
fi
if command -v gpsim >/dev/null 2>&1; then
  for pair in "trap:$T14:t14" "bound:$B14:b14"; do
    kind=${pair%%:*}; rest=${pair#*:}; wit=${rest%:*}; tag=${rest##*:}
    if ( cd "$WORK" && bash "$REPO/sims/jal_gpsim_validate.sh" \
        --compiler "$CC" --quiet -s "$REPO/$LIBS" "$wit" > "$tag.log" 2>&1 ); then
      echo "ok   unbound walk classic 14: $kind arm passes"
    else
      fail "unbound walk classic 14 $kind" "$WORK/$tag.log"
    fi
  done
  if ( cd "$WORK" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet -s "$REPO/$LIBS" "$WORK/t14_neg.jal" \
      > t14neg.log 2>&1 ); then
    fail "unbound walk classic 14 trap negative control PASSED"
  else
    echo "ok   unbound walk classic 14 trap negative control fails as it must"
  fi
  if ( cd "$WORK" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet -s "$REPO/$LIBS" "$WORK/b14_neg.jal" \
      > b14neg.log 2>&1 ); then
    fail "unbound walk classic 14 bound negative control PASSED"
  else
    echo "ok   unbound walk classic 14 bound negative control fails as it must"
  fi
else
  echo "skip gpsim not available (unbound walk classic 14)"
fi

# --- 4. the guard is in the emitted code ---------------------------------
( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -asm "$WORK/g.asm" \
    -hex "$WORK/g.hex" "$T14H" ) > "$WORK/enc.log" 2>&1
guard=$(awk '/g = p\[100\]/ { f = 1 } /forever/ { f = 0 } \
    f && /iorwf +v_p/ { n++ } END { print n + 0 }' "$WORK/g.asm" 2>/dev/null)
ztest=$(awk '/g = p\[100\]/ { f = 1 } /forever/ { f = 0 } \
    f && /btfss +v__status, +v__z/ { n++ } END { print n + 0 }' \
    "$WORK/g.asm" 2>/dev/null)
if [ "${guard:-0}" -ge 1 ] && [ "${ztest:-0}" -ge 1 ]; then
  echo "ok   unbound walk: the guard (all-zero test) precedes the walk"
else
  fail "unbound walk: guard missing (iorwf=$guard ztest=$ztest)" "$WORK/g.asm"
fi

if (( fails )); then
  echo "FAIL: $fails unbound-walk check(s)"
  exit 1
fi
echo "ok   unbound walk null guard"

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Dynamic-string append growth on a chunked string heap.
#
# `s = ""; for 80 using i loop s[i] = i end loop` on a 200-byte heap used to
# stall at length 64: every append reallocated an exactly-sized block, the
# heap fragmented, and a 65-byte payload (68-byte block) no longer fit
# between the two ~66-byte free runs.  The append now uses the handle's spare
# capacity, grows geometrically, and compacts ONCE (the outlined
# string_heap_compact call) before retrying.
#
#   1. the 14H and classic-14 runtime measurements (length and the whole
#      payload: s[0], s[3], s[40], s[79] -- the growth at len 63 compacts,
#      and an outlined `string_put` body holds a COPY of the handle, so the
#      retry must re-point it from the compaction's work table; the old
#      code copied from and freed the stale block, overwriting s[3..5] with
#      the stale block's free header);
#   2. failing negative controls on each (length off by one, and a wrong
#      first byte);
#   3. the compact retry is in the emitted code (one outlined call);
#   4. the witness really exercises a chunked heap (3 chunks).
#
# Usage: unit-tests/test_string_heap_append.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
PICSIM="${PICSIM_DIR:-$REPO/../tmp/picsim-try/picsim}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib;sims/shared'
W14H="$REPO/sims/picsim/16f18857_picsim_string_heap_append_grow.jal"
W14="$REPO/sims/gpsim/16f887_gpsim_string_heap_append_grow.jal"

fail() {
  echo "FAIL: $1"
  shift
  sed 's/^/     /' "$@" 2>/dev/null | head -6
  fails=$((fails + 1))
}

sed 's/expect byte v_len 80/expect byte v_len 81/' "$W14H" \
    > "$WORK/neg14h.jal"
sed 's/expect byte v_len 80/expect byte v_len 81/' "$W14" \
    > "$WORK/neg14.jal"
sed 's/expect byte v_first 0/expect byte v_first 1/' "$W14" \
    > "$WORK/negcontent14.jal"

# --- 1/2. runtime on both cores ------------------------------------------
if [ -x "$PICSIM/src/libpicsim.so" ] || [ -x "$PICSIM/src/libpicsim.a" ] \
    || [ -f "$PICSIM/src/libpicsim.so" ] || [ -f "$PICSIM/src/libpicsim.a" ]; then
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$W14H" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/pos14h.log" 2>&1 ); then
    echo "ok   append growth 14H: 80 appends, count=80, last byte=79"
  else
    fail "append growth 14H measurement" "$WORK/pos14h.log"
  fi
  if ( cd "$REPO" && python3 sims/picsim_validate_engine.py "$WORK/neg14h.jal" \
      --picsim "$PICSIM" --compiler "$CC" > "$WORK/neg14h.log" 2>&1 ); then
    fail "append growth 14H negative control PASSED"
  else
    echo "ok   append growth 14H negative control fails as it must"
  fi
else
  echo "skip picsim engine not available (append growth 14H)"
fi
if command -v gpsim >/dev/null 2>&1; then
  if ( cd "$WORK" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet -s "$REPO/$LIBS" "$W14" > pos14.log 2>&1 ); then
    echo "ok   append growth classic 14: 80 appends, whole payload"
  else
    fail "append growth classic 14 measurement" "$WORK/pos14.log"
  fi
  if ( cd "$WORK" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet -s "$REPO/$LIBS" "$WORK/negcontent14.jal" \
      > negc14.log 2>&1 ); then
    fail "append growth classic 14 content control PASSED"
  else
    echo "ok   append growth classic 14 content control fails as it must"
  fi
  if ( cd "$WORK" && bash "$REPO/sims/jal_gpsim_validate.sh" \
      --compiler "$CC" --quiet -s "$REPO/$LIBS" "$WORK/neg14.jal" \
      > neg14.log 2>&1 ); then
    fail "append growth classic 14 negative control PASSED"
  else
    echo "ok   append growth classic 14 negative control fails as it must"
  fi
else
  echo "skip gpsim not available (append growth classic 14)"
fi

# --- 3. the compact retry is in the emitted code -------------------------
( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -asm "$WORK/a.asm" \
    -hex "$WORK/a.hex" "$W14H" ) > "$WORK/enc.log" 2>&1
cmp_retry=$(grep -c 'call.*string_heap_compact' "$WORK/a.asm" 2>/dev/null || true)
if [ "${cmp_retry:-0}" -ge 1 ]; then
  echo "ok   append growth: the failed growth retries through string_heap_compact"
else
  fail "append growth: no compact retry in the emitted code" "$WORK/a.asm"
fi

# --- 4. the witness uses a chunked heap ----------------------------------
chunks=$( ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -asm /dev/null \
    -hex /dev/null -debug=varalloc "$W14H" ) 2>&1 \
    | grep 'CHUNKMAP r1 _string_heap' | sort -u | wc -l )
if [ "${chunks:-0}" -ge 2 ]; then
  echo "ok   append growth: the heap is chunked ($chunks chunks)"
else
  fail "append growth: the heap is not chunked ($chunks chunks)"
fi

if (( fails )); then
  echo "FAIL: $fails append-growth check(s)"
  exit 1
fi
echo "ok   string heap append growth"

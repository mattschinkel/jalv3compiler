#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# A3 frame placement: refuse only what genuinely needs one contiguous frame.
#
# History:
#   increment 1  stopped the silent aliasing: a frame larger than one window
#                was laid out from VARIABLE_BASE_UNKNOWN (0xFFFF/0x1D/0x3B).
#   increment 2  scattered the shared ISR block one object at a time.
#   increment 3  places what still refused: an ISR local larger than one
#                window (deferred to the group/chunk placement), and an
#                indirect proc's automatic frame (per-variable placement;
#                only a re-entrant frame or a user `pragma frame` needs one
#                contiguous run).
#
# The refusals that remain are the two that are REAL: a re-entrant frame
# (pushed byte by byte from the base) and a user `pragma frame`.
#
# Usage: unit-tests/test_frame_overflow.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
LIBS="$REPO/demo_lib;$REPO/third_party/jallib/lib"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

# expect_refusal FILE PATTERN: nonzero exit, PATTERN on stderr/stdout, no HEX
expect_refusal() {
  local file=$1 pattern=$2 out rc
  rm -f "$WORK/out.hex" "$WORK/out.asm"
  out=$("$CC" -s "$LIBS" -no-codfile -asm "$WORK/out.asm" -hex "$WORK/out.hex" \
        "$file" 2>&1)
  rc=$?
  if (( rc == 0 )); then
    echo "FAIL $(basename "$file"): compiled clean, expected a refusal"
    fails=$((fails + 1))
    return
  fi
  if ! echo "$out" | grep -qF "$pattern"; then
    echo "FAIL $(basename "$file"): refusal does not name '$pattern'"
    echo "$out" | grep "error" | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  if [[ -e "$WORK/out.hex" || -e "$WORK/out.asm" ]]; then
    echo "FAIL $(basename "$file"): refusal still wrote an output file"
    fails=$((fails + 1))
    return
  fi
  echo "ok   $(basename "$file"): refused with '$pattern'"
}

# expect_placed FILE SYMBOL_PREFIX: compiles 0/0 and no local of that proc is
# left unplaced (`EQU 0xffffffff`).  `_bitbucket` mirrors are unplaced by
# design, so they are excluded.
expect_placed() {
  local file=$1 prefix=$2 out asm
  asm="$WORK/placed.asm"
  rm -f "$asm"
  out=$("$CC" -s "$LIBS" -no-codfile -asm "$asm" "$file" 2>&1)
  if ! echo "$out" | grep -q "0 errors, 0 warnings"; then
    echo "FAIL $(basename "$file"): expected 0 errors, 0 warnings"
    echo "$out" | grep -E "error|warning" | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  if grep 'EQU 0xffffffff' "$asm" 2>/dev/null \
      | grep -F "$prefix" | grep -qv '_bitbucket'; then
    echo "FAIL $(basename "$file"): a local is still unplaced"
    grep 'EQU 0xffffffff' "$asm" | grep -F "$prefix" \
      | grep -v '_bitbucket' | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  echo "ok   $(basename "$file"): placed (no unplaced local)"
}

# expect_clean FILE: compiles with the standard 0 errors / 0 warnings line
expect_clean() {
  local file=$1 out
  out=$("$CC" -s "$LIBS" -no-codfile "$file" 2>&1)
  if echo "$out" | grep -q "0 errors, 0 warnings"; then
    echo "ok   $(basename "$file"): compiles clean"
  else
    echo "FAIL $(basename "$file"): expected 0 errors, 0 warnings"
    echo "$out" | grep -E "error|warning" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# 1-2. the two shapes that now place: an ISR local and an indirect local,
#      each larger than one window
expect_placed "$REPO/unit-tests/fixtures/frame_chunked_isr.jal" "_ext_isr:"
expect_placed "$REPO/unit-tests/fixtures/frame_chunked_indirect.jal" "_t:"

# 3-4. the two frames that genuinely need one contiguous run still refuse
expect_refusal "$REPO/unit-tests/negative/frame_reentrant_overflow.jal" \
  "no space for the frame of '_t'"
expect_refusal "$REPO/unit-tests/negative/frame_explicit_overflow.jal" \
  "no space for the frame of '_t'"

# 5. the shared ISR block no longer fits, but each object does: scattered
cat > "$WORK/isr_scattered.jal" <<'EOF'
include 16f18857
pragma target clock 8_000_000
var volatile byte g1 = 0
var volatile byte g2 = 0
procedure _ext_isr() is
   pragma interrupt
   var byte a[40]
   var byte b[40]
   var byte c[40]
   var byte i
   for 40 using i loop
      a[i] = g1
      b[i] = g2
      c[i] = i
   end loop
end procedure
procedure _run() is
   var byte k
   k = 0
   g1 = 1
   g2 = 2
end procedure
_run()
EOF
expect_placed "$WORK/isr_scattered.jal" "_ext_isr:"

# 6. control: an ISR frame that FITS one window keeps the shared-block path
cat > "$WORK/isr_fits.jal" <<'EOF'
include 16f18857
pragma target clock 8_000_000
var volatile byte g1 = 0
var volatile byte g2 = 0
procedure _ext_isr() is
   pragma interrupt
   var byte a[20]
   var byte b[20]
   var byte c[20]
   var byte i
   for 20 using i loop
      a[i] = g1
      b[i] = g2
      c[i] = i
   end loop
end procedure
procedure _run() is
   var byte k
   k = 0
end procedure
_run()
EOF
expect_clean "$WORK/isr_fits.jal"

# 7. control: the same oversized ISR frame is legal on a contiguous-GPR part
cat > "$WORK/isr_18f.jal" <<'EOF'
include 18f4620
pragma target clock 8_000_000
procedure _ext_isr() is
   pragma interrupt
   var byte a[30]
   var byte b[30]
   var byte c[30]
   var byte i
   for 30 using i loop
      a[i] = i
      b[i] = i
      c[i] = i
   end loop
end procedure
procedure _run() is
   var byte k
   k = 0
end procedure
_run()
EOF
expect_clean "$WORK/isr_18f.jal"

# 8. control: a normal proc-local array still takes the chunked placement
cat > "$WORK/local_chunked.jal" <<'EOF'
include 16f18857
pragma target clock 8_000_000
procedure _use() is
   var byte big[200]
   var byte i
   for 200 using i loop
      big[i] = i
   end loop
end procedure
_use()
EOF
expect_clean "$WORK/local_chunked.jal"

echo "----"
if (( fails )); then
  echo "FAIL ($fails)"
  exit 1
fi
echo "PASS"
exit 0

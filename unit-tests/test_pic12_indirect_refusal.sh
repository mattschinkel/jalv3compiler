#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# P3c: the 12-bit capability refusal.  The 12-bit family (PIC10F/12F5xx) has
# no FSR/IRP, so a runtime index or a pointer cannot be lowered.  Before this
# pin the compiler emitted the IRP flag instruction, the flag name failed to
# resolve, and one dynamic store buried the user under two "'_irp' not found"
# errors from deep inside the emitter.
#
# What this pin proves:
#   1. the refusal is ONE diagnostic, naming the core's limit
#      ("12-bit core: no indirect addressing ..."), for every shape that
#      needs indirection -- a runtime array index, a pointer-to-array index,
#      and a byref view of a const array (which takes the 14-bit table
#      lowering) -- and on every 12-bit family member tested (12F509, 10F200);
#   2. the old noise is gone (no "'_irp' not found" and no "'_pclath' not
#      found" anywhere);
#   3. the refusal is a CAPABILITY, not a blanket ban: a constant index on the
#      same part still compiles;
#   4. the same shapes on a 14-bit part still compile (the control: the
#      programs are legal where the core has the hardware);
#   5. a pre-fix compiler has 2 "'_irp' not found" errors per dynamic shape,
#      and one "'_pclath' not found" for the byref view, so the pin is
#      discriminating.
#
# Usage: unit-tests/test_pic12_indirect_refusal.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC=(-s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib")

gen_dyn() { # $1 device, $2 file: runtime index into an array
  {
    echo "include $1"
    echo "var byte a[4]"
    echo "var byte i"
    echo "i = 0"
    echo "a[i] = 1"
  } > "$2"
}

gen_ptr() { # $1 device, $2 file: runtime index through a pointer-to-array
  {
    echo "include $1"
    echo "var byte a[4]"
    echo "var byte i"
    echo "var pointer p as var byte[4]"
    echo "p is a"
    echo "i = 0"
    echo "p[i] = 1"
  } > "$2"
}

gen_const() { # $1 device, $2 file: constant index
  {
    echo "include $1"
    echo "var byte a[4]"
    echo "a[0] = 1"
  } > "$2"
}

gen_byref_view() { # $1 device, $2 file: byref view of a const array
  {
    echo "include $1"
    echo "var volatile byte out"
    echo "procedure sink(byref in a[]) is"
    echo "   out = a[0]"
    echo "end procedure"
    echo "const byte m1[] = {0, 66}"
    echo "out = 0x11"
    echo "sink(m1)"
  } > "$2"
}

check_refusal() { # $1 device, $2 label, $3 generator
  local dev=$1 label=$2 gen=$3 src log nerr rc
  src="$WORK/${label}_$dev.jal"
  log="$WORK/${label}_$dev.log"
  "$gen" "$dev" "$src"
  ( cd "$WORK" && "$CC" "${INC[@]}" "$(basename "$src")" > "$log" 2>&1 )
  rc=$?
  nerr=$(grep -c ": error: " "$log" 2>/dev/null || true)
  if [ "$rc" -ne 0 ] && [ "${nerr:-0}" -eq 1 ] \
      && grep -q "12-bit core: no indirect addressing" "$log"; then
    echo "ok   $dev $label: one capability diagnostic"
  else
    echo "FAIL: $dev $label: expected exactly one capability diagnostic (rc=$rc errors=${nerr:-?})"
    grep -E "error|_irp" "$log" | head -5 | sed 's/^/     /'
    fails=$((fails + 1))
  fi
  if grep -q "'_irp' not found" "$log"; then
    echo "FAIL: $dev $label: the old '_irp' noise is still present"
    fails=$((fails + 1))
  else
    echo "ok   $dev $label: no '_irp' not found lines"
  fi
  if grep -q "'_pclath' not found" "$log"; then
    echo "FAIL: $dev $label: the 14-bit table setup leaked '_pclath'"
    fails=$((fails + 1))
  else
    echo "ok   $dev $label: no '_pclath' not found lines"
  fi
}

check_compiles() { # $1 device $2 label $3 generator
  local dev=$1 label=$2 gen=$3 src log
  src="$WORK/${label}_$dev.jal"
  log="$WORK/${label}_$dev.log"
  "$gen" "$dev" "$src"
  ( cd "$WORK" && "$CC" "${INC[@]}" "$(basename "$src")" > "$log" 2>&1 )
  if grep -q "0 errors" "$log"; then
    echo "ok   $label on $dev compiles (0 errors)"
  else
    echo "FAIL: $label on $dev did not compile"
    grep -E "error" "$log" | head -5 | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# 1 + 2. both 12-bit family members refuse both shapes once, cleanly
check_refusal 12f509 dyn gen_dyn
check_refusal 12f509 ptr gen_ptr
check_refusal 10f200 dyn gen_dyn
check_refusal 10f200 ptr gen_ptr
# A byref view of a CONST array takes the 14-bit table lowering (PCLATH);
# it is a pointer access too, so it must refuse with the same capability
# message instead of leaking "'_pclath' not found".
check_refusal 12f509 byref gen_byref_view
check_refusal 10f200 byref gen_byref_view
# 3. a constant index is still legal on the 12-bit part
check_compiles 12f509 const gen_const
check_compiles 10f200 const gen_const
# 4. the same shapes are legal on a 14-bit part (the control)
check_compiles 12f1840 dyn gen_dyn
check_compiles 12f1840 ptr gen_ptr
check_compiles 12f1840 byref gen_byref_view

echo ""
if [ "$fails" -ne 0 ]; then
  echo "RESULT: FAIL ($fails checks)"
  exit 1
fi
echo "RESULT: PASS"
exit 0

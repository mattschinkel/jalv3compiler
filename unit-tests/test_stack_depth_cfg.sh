#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Pin the PIC hardware-return-stack-depth walker (src/libpic12/pic_stk.c)
# against hand-built control-flow shapes.
#
# The walker's edge cases have a long defect history (nine fixes:
# unresolved-GOTO fall-through, conditional-GOTO threading, DFS cycle cut,
# shallow revisit, BRA conditional edges, pc0/ORG sink, ...).  Every one of
# them was found by a LARGE real sample, and the gate coverage is still those
# corpus witnesses plus `expect_hw_stkptr_le_compiler_depth` (which only runs
# on parts that have STKPTR).  Nothing compiled a small shape whose expected
# depth is obvious, so a walker regression that only shows on an edge class
# stays invisible until some sample happens to trip it.
#
# Each fixture is a minimal program whose depth is unambiguous.  The expected
# number below was cross-checked against the emitted `.asm` call nesting when
# the probe was written (and recorded in the fix write-up).
#
# Usage: unit-tests/test_stack_depth_cfg.sh [compiler]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$ROOT/bin/beta/jalv3-x86-64}
FIX="$ROOT/unit-tests/fixtures/stack_depth"
INC="$ROOT/demo_lib;$ROOT/third_party/jallib/lib;$ROOT/sims/shared"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

# check NAME FIXTURE EXPECTED-LINE [overflow]
#   EXPECTED-LINE is matched literally (the walker's own diagnostic text).
#   "overflow" flips the negative control: the probe MUST report the overflow
#   diagnostic, and a depth probe must NOT.
check() {
  name=$1
  file=$2
  want=$3
  mode=${4:-depth}
  out=$("$CC" -s "$INC" -no-codfile -hex "$WORK/$name.hex" \
        "$FIX/$file" 2>&1)
  rc=$?

  if [ "$rc" -ne 0 ]; then
    bad "$name: compiler exited $rc" "$(printf '%s\n' "$out" | tail -3)"
    return
  fi
  if ! printf '%s\n' "$out" | grep -q '^0 errors'; then
    bad "$name: compile did not report 0 errors" \
      "$(printf '%s\n' "$out" | grep -i error | head -3)"
    return
  fi
  if ! printf '%s\n' "$out" | grep -qF "$want"; then
    bad "$name: missing \"$want\"" \
      "$(printf '%s\n' "$out" | grep -i 'Hardware stack' | head -2)"
    return
  fi
  if [ "$mode" = "depth" ] \
     && printf '%s\n' "$out" | grep -qF 'Hardware stack overflow!'; then
    bad "$name: unexpected overflow diagnostic" \
      "$(printf '%s\n' "$out" | grep -i 'Hardware stack' | head -2)"
    return
  fi
  ok "$name: $want"
}

check nest3              nest3.jal                'Hardware stack depth 3 of 8'
check branch_arms        branch_arms.jal          'Hardware stack depth 2 of 8'
check loop_call          loop_call.jal            'Hardware stack depth 1 of 8'
check tail_chain         tail_chain.jal           'Hardware stack depth 1 of 8'
check deep8_at_limit     deep8.jal                'Hardware stack depth 8 of 8'
check deep9_over_limit   deep9.jal \
      'Hardware stack overflow! (main+tasks depth 9, total with ISR 9, limit 8)' overflow
check branch_arms_p18    branch_arms_18f4620.jal  'Hardware stack depth 2 of 31'

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "stack depth cfg: PASS"
  exit 0
fi
echo "stack depth cfg: $fails FAILURE(S)"
exit 1

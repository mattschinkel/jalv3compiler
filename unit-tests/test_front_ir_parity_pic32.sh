#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# PIC32 cross-target sem-on runtime-equivalence pin.
#
# The runtime-equivalence pin is 18F4620-gpsim only; the flat 32-bit back end
# must also execute the same string bodies with the layer off and on.  Each
# witness is compiled and QEMU-validated twice (JALV2_FRONT_IR=0 and =1).
#
# Usage: unit-tests/test_front_ir_parity_pic32.sh [path-to-jalv3]
set -u

COMPILER="${1:-./bin/beta/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

cd "$ROOT" || exit 1

for w in \
  sims/pic32/32mx270f256b_body_string_arg_view.jal \
  sims/pic32/32mx270f256b_body_fixed_string_body.jal  sims/pic32/32mx270f256b_front_ir_count.jal
do
  for mode in 0 1; do
    if JALV2_FRONT_IR=$mode "$ROOT/sims/jal_sim_validate.sh" \
        --compiler "$COMPILER" --quiet "$ROOT/$w" \
        >"$TMP/run.log" 2>&1; then
      echo "PASS $(basename "$w" .jal) (JALV2_FRONT_IR=$mode qemu)"
      pass=$((pass + 1))
    else
      echo "FAIL $(basename "$w" .jal) (JALV2_FRONT_IR=$mode qemu)"
      tail -20 "$TMP/run.log"
      fail=$((fail + 1))
    fi
  done
done

echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]

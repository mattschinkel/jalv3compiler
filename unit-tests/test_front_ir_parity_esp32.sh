#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# ESP32 cross-target sem-on runtime-equivalence pin.
#
# The runtime-equivalence pin is 18F4620-gpsim only; the flat 32-bit back end
# must also execute the same string bodies.  Each witness is compiled and
# QEMU-validated once; the retired JALV2_FRONT_IR switch is gone, so this is
# the single-arm runtime check.
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
  sims/esp32/esp32_body_fixed_string_body.jal  sims/esp32/esp32_front_ir_count.jal
do
  if "$ROOT/sims/jal_esp32_validate.sh" \
      --compiler "$COMPILER" --quiet "$ROOT/$w" \
      >"$TMP/run.log" 2>&1; then
    echo "PASS $(basename "$w" .jal) (esp32-qemu)"
    pass=$((pass + 1))
  else
    echo "FAIL $(basename "$w" .jal) (esp32-qemu)"
    tail -20 "$TMP/run.log"
    fail=$((fail + 1))
  fi
done

echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]

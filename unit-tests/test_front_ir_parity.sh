#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Phase-1/2 semantic-layer RUNTIME-EQUIVALENCE pin.
#
# The layer is behind JALV2_FRONT_IR (default off; the values 0/false/no/off
# are also off).  It is an ALTERNATE LOWERING, not a byte-identical one:
# every witness is compiled and run under gpsim twice -- once with the layer
# off and once with it on -- and BOTH runs must pass their RAM checks.
# (The default, unset path is covered by the normal gpsim suite as well.)
#
# Historical note: earlier versions of this pin compared gate-off/on HEX for
# identity, but `jal_sem_enabled` used to treat ANY value (including "0") as
# on, so the "off" arm was actually on.  The gate now parses its value, and
# this pin checks behaviour, not identity.
#
# Usage: unit-tests/test_front_ir_parity.sh [path-to-jalv3]
set -u

COMPILER="${1:-./bin/beta/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LIBS="demo_lib;sims/shared;third_party/jallib/lib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

cd "$ROOT" || exit 1

for w in \
  sims/gpsim/18f4620_gpsim_string_clone.jal \
  sims/gpsim/18f4620_gpsim_string_clone_matrix.jal \
  sims/gpsim/18f4620_gpsim_string_clone_alias.jal \
  sims/gpsim/18f4620_gpsim_front_ir_subscript.jal \
  sims/gpsim/18f4620_gpsim_front_ir_edges.jal \
  sims/gpsim/18f4620_gpsim_front_ir_count.jal \
  sims/gpsim/18f4620_gpsim_front_ir_whole_assign.jal \
  sims/gpsim/18f4620_gpsim_front_ir_relation.jal \
  sims/gpsim/18f4620_gpsim_front_ir_relation_array.jal \
  sims/gpsim/18f4620_gpsim_front_ir_concat.jal \
  sims/gpsim/18f4620_gpsim_front_ir_pass_on.jal \
  sims/gpsim/18f4620_gpsim_front_ir_ptr_count.jal \
  sims/gpsim/18f4620_gpsim_front_ir_builtin_append.jal \
  sims/gpsim/18f4620_gpsim_front_ir_legacy_length.jal \
  sims/gpsim/18f4620_gpsim_front_ir_legacy_length_fixed.jal \
  sims/gpsim/18f4620_gpsim_front_ir_clone_builtin.jal
do
  for mode in 0 1; do
    if JALV2_FRONT_IR=$mode "$ROOT/sims/jal_gpsim_validate.sh" \
        --compiler "$COMPILER" -s "$LIBS" "$ROOT/$w" \
        >"$TMP/run.log" 2>&1; then
      echo "PASS $(basename "$w" .jal) (JALV2_FRONT_IR=$mode runtime)"
      pass=$((pass + 1))
    else
      echo "FAIL $(basename "$w" .jal) (JALV2_FRONT_IR=$mode runtime)"
      tail -20 "$TMP/run.log"
      fail=$((fail + 1))
    fi
  done
done

echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]

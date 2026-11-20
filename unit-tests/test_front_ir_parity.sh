#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Front-end IR / string-clone RUNTIME pin.
#
# Every witness here exercises a representation-dependent operation (a
# subscript load/store through an open-array or heap-string formal, COUNT,
# a whole-formal assign, a relation, a concat, a pass-on call) plus the
# string clone that specializes them.  Each is compiled and run under gpsim
# and must pass its RAM checks.  The witnesses are also in the normal gpsim
# suites; this pin keeps the whole feature matrix in ONE fast run so a
# lowering change cannot quietly lose a shape.
#
# Historical note: this pin used to compare gate-off/on HEX (then runtime)
# against an alternate lowering selected by JALV2_FRONT_IR.  The layer is now
# the compiler's only lowering -- the switch and the source re-parse it
# guarded are gone -- so there is no second arm to compare against.
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
  sims/gpsim/18f4620_gpsim_string_clone_putparams.jal \
  sims/gpsim/18f4620_gpsim_front_ir_clone_builtin.jal
do
  for mode in 0; do
    # one arm: the front-end IR (the clone body is the duplicated generic
    # body re-lowered per the clone's formals)
    case $mode in
      *) label="front-end IR"
         envs=() ;;
    esac
    if env "${envs[@]}" "$ROOT/sims/jal_gpsim_validate.sh" \
        --compiler "$COMPILER" -s "$LIBS" "$ROOT/$w" \
        >"$TMP/run.log" 2>&1; then
      echo "PASS $(basename "$w" .jal) ($label runtime)"
      pass=$((pass + 1))
    else
      echo "FAIL $(basename "$w" .jal) ($label runtime)"
      tail -20 "$TMP/run.log"
      fail=$((fail + 1))
    fi
  done
done

echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]

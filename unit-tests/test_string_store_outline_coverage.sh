#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# String-store OUTLINE COVERAGE pin (phase 4b of the front-end IR work).
#
# Every user-level `s[i] = c` on a heap string must lower to ONE call of the
# shared `string_put`/`_string_put` body (fixes/fix_string_outline_calls.md),
# not to the ~500-word inline expansion.  The inline expansion stays legal in
# exactly one place: inside the forwarder body itself (outlining a call to
# yourself would recurse), which is what `jal_string_forwarder_body_p` guards.
#
# The compiler reports every inline string store it emits when
# `JALV2_DBG_OL` is set:
#
#   OL inline_string_store proc=<procedure tag>
#
# This pin compiles a representative set of string programs and requires that
# every such line names a FORWARDER body.  A new decline path (a base shape,
# a source width, a missing heap) shows up here as a user-procedure tag and
# fails the gate -- which is the only way a coverage regression stays visible,
# since both paths produce correct code (the inline one is just 8-40x bigger).
#
# Usage: unit-tests/test_string_store_outline_coverage.sh [path-to-jalv3]
set -u

COMPILER="${1:-./bin/beta/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LIBS="demo_lib;sims/shared;third_party/jallib/lib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
seen=0
checked=0

cd "$ROOT" || exit 1

for w in \
  sims/gpsim/18f4620_gpsim_front_ir_subscript.jal \
  sims/gpsim/18f4620_gpsim_front_ir_whole_assign.jal \
  sims/gpsim/18f4620_gpsim_front_ir_concat.jal \
  sims/gpsim/18f4620_gpsim_string_clone.jal \
  sims/gpsim/18f4620_gpsim_string_clone_matrix.jal \
  sims/gpsim/18f4620_gpsim_front_ir_clone_builtin.jal \
  demo_sample/18f4620_strings.jal \
  demo_sample/18f67j50_string_dynamic.jal \
; do
  checked=$((checked + 1))
  log="$TMP/$(basename "$w").log"
  if ! env JALV2_DBG_OL=1 "$COMPILER" -s "$LIBS" -asm "$TMP/o.asm" "$w" \
      >"$log" 2>&1; then
    echo "FAIL $w: compile failed"
    sed -n '1,5p' "$log" | sed 's/^/     /'
    fail=$((fail + 1))
    continue
  fi
  # every inline string store must be inside a forwarder body
  bad="$(grep -oE 'OL inline_string_store proc=[^ ]+' "$log" \
    | sed 's/.*proc=//' \
    | grep -vE '^_?string_put$' | sort -u)"
  n="$(grep -c 'OL inline_string_store' "$log")"
  seen=$((seen + n))
  if [ -n "$bad" ]; then
    echo "FAIL $w: inline string store outside a forwarder body"
    echo "$bad" | sed 's/^/     proc: /'
    fail=$((fail + 1))
  else
    echo "PASS $w ($n inline site(s), all in the shared body)"
  fi
done

# The probe must have fired at all: a silent `JALV2_DBG_OL` (renamed, gated
# off, or the emitter routed elsewhere) would make every check above vacuous.
if [ "$seen" -lt 5 ]; then
  echo "FAIL the inline-store probe saw only $seen site(s) in $checked program(s)"
  echo "     (expected the shared forwarder bodies to report several)"
  fail=$((fail + 1))
fi

echo "---"
if [ "$fail" -eq 0 ]; then
  echo "RESULT: every user-level string store is outlined ($checked programs, $seen inline site(s) inside the shared body)"
  exit 0
fi
echo "RESULT: $fail coverage failure(s)"
exit 1

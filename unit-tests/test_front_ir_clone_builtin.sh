#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# The finer alias attribution (the outlined/inline builtin growth names its
# destination actual):
#   POSITIVE -- a clone body that grows a GLOBAL string distinct from the
#     actual clones (the runtime witness front_ir_clone_builtin).
#   NEGATIVE -- the same body called with the very grown global is still
#     refused: the clone's handle copy cannot see that growth.
#
# Usage: unit-tests/test_front_ir_clone_builtin.sh [path-to-jalv3]
set -u

CC="${1:-./bin/beta/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LIBS="demo_lib;sims/shared;third_party/jallib/lib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

cd "$ROOT" || exit 1

cat > "$TMP/neg.jal" <<'JAL'
include regression_18f4620_device
include regression
pragma target string_heap_size 64
include strings_dynamic
var string g
procedure mix(byte in out x[]) is
   x[0] = "Z"
   string_append(g, "xy")
end procedure
procedure run() is
   regression_begin_run()
   g = "abc"
   mix(g)
   regression_checkpoint()
end procedure
run()
JAL

if "$CC" -s "$LIBS" -hex "$TMP/pos.hex" \
    sims/gpsim/18f4620_gpsim_front_ir_clone_builtin.jal \
    >"$TMP/pos.log" 2>&1; then
  echo "PASS distinct-global clone builtin (compiles)"
else
  echo "FAIL distinct-global clone builtin (did not compile)"
  tail -10 "$TMP/pos.log"
  fail=1
fi

if "$CC" -s "$LIBS" -hex "$TMP/neg.hex" "$TMP/neg.jal" \
    >"$TMP/neg.log" 2>&1; then
  echo "FAIL same-string clone builtin (compiled; the alias gate must refuse)"
  fail=1
elif grep -q "cannot pass the string g" "$TMP/neg.log" \
    && grep -q "would corrupt the string" "$TMP/neg.log"; then
  echo "PASS same-string clone builtin refused (alias gate)"
else
  echo "FAIL same-string clone builtin (wrong diagnostic)"
  tail -10 "$TMP/neg.log"
  fail=1
fi

[ "$fail" -eq 0 ]

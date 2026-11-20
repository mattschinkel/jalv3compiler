#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# A pointer-returning function followed by a dereference of the result used
# to hang the compiler: pfile_lookup_mark's fixpoint looped forever because
# the function's pointer return slot has no backing variable, so
# pfile_ptr_flag_set(NULL, ...) reported a change on every pass.  The guard
# (`dst &&`) makes the fixpoint terminate; this pins that the program now
# COMPILES in bounded time (the old build times out).
#
# Note: the feature itself still miscompiles (`return <pointer>` dereferences
# instead of returning the address) -- see the byref TODOS.  This pin is only
# about the hang.
#
# Usage: unit-tests/test_ptr_return_no_hang.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

cat > "$WORK/ptrret.jal" <<'JALEOF'
include 16f887
var byte g
function getp() return pointer is
   var pointer p as var byte
   p is g
   return p
end function
var pointer q as var byte
q = getp()
var byte v at q
v = 0x42
forever loop end loop
JALEOF

if ( cd "$REPO" && timeout 30 "$CC" -s 'demo_lib;third_party/jallib/lib' \
    -no-codfile -hex "$WORK/o.hex" "$WORK/ptrret.jal" ) > "$WORK/capture" 2>&1; then
  echo "ok   pointer-return + deref compiles (no pfile_lookup_mark hang)"
else
  echo "FAIL: pointer-return + deref (hang or error)"; tail -6 "$WORK/capture"
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails ptr-return-no-hang check(s)"
  exit 1
fi
echo "PASS: pointer-return + deref does not hang"

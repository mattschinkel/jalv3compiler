#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for pointer-bind storage semantics (fix_pointer_bind_const_fold):
# `p is <var>` is an ADDRESS ESCAPE -- the target must stay in RAM and
# stores through the pointer must work even when the target is never
# written directly (it used to fold to a CONST retlw table and every
# store was refused "Attempt to assign to a lookup table"). The ROM
# refusals that must SURVIVE: a const-typed pointee, and a mutable
# pointer bound to a declared-const array.
#
# Usage: unit-tests/test_pointer_bind_semantics.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

expect_ok() { # name file -- must compile with 0 errors
  local name=$1 file=$2 out n
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$file" 2>&1)
  n=$(echo "$out" | grep -c "^.*error:")
  if echo "$out" | grep -q "0 errors"; then
    echo "ok   $name"
  else
    echo "FAIL $name: unexpected errors:"
    echo "$out" | grep "error:" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

expect_err() { # name file substring
  local name=$1 file=$2 want=$3 out
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$file" 2>&1)
  if echo "$out" | grep -q "$want"; then
    echo "ok   $name"
  else
    echo "FAIL $name: expected '$want' in:"
    echo "$out" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# --- the fixed case: never-written var arrays, stores through the pointer ---
cat > "$WORK/fix_byte.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
var byte arr[10]
var pointer p as var byte[10]
var byte x
var word idx
procedure t() is
   p is arr
   idx = 1
   x = p[idx]
   p[idx] = x
end procedure
t()
forever loop
end loop
EOF
expect_ok "var byte[10] bind + store through pointer" "$WORK/fix_byte.jal"

cat > "$WORK/fix_word.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
var word arr[10]
var pointer p as var word[10]
var word x
var word idx
procedure t() is
   p is arr
   idx = 1
   x = p[idx]
   p[idx] = x
end procedure
t()
forever loop
end loop
EOF
expect_ok "var word[10] bind + store through pointer" "$WORK/fix_word.jal"

# --- preserved refusal 1: const-typed pointee ---
cat > "$WORK/rom1.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const byte tbl[4] = {1, 2, 3, 4}
var pointer p as const byte[4]
var byte x
var word idx
procedure t() is
   p is tbl
   idx = 1
   x = p[idx]
   p[idx] = x
end procedure
t()
forever loop
end loop
EOF
expect_err "const-typed pointee store refused" "$WORK/rom1.jal" \
  "cannot assign or modify through pointer to const data"

# --- preserved refusal 2: mutable pointer bound to a const array ---
cat > "$WORK/rom2.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const byte tbl[4] = {1, 2, 3, 4}
var pointer p as var byte[4]
var byte x
var word idx
procedure t() is
   p is tbl
   idx = 1
   x = p[idx]
   p[idx] = x
end procedure
t()
forever loop
end loop
EOF
expect_err "mutable pointer to const array store refused" "$WORK/rom2.jal" \
  "Attempt to assign to a lookup table"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all pointer-bind semantics pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

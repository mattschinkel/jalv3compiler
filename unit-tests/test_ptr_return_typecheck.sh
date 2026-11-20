#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# `return p` is context-sensitive on the return type: a `return pointer`
# function returns the POINTER VALUE (the address); a `return byte/word`
# function returns the POINTEE (the deref).  The compiler checks the
# operand is a pointer for a pointer return, and the assign checks the
# pointee type for a scalar return.
#
# Usage: unit-tests/test_ptr_return_typecheck.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib'

compile() {  # compile <file> <log>
  ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -hex "$WORK/o.hex" \
      "$1" ) > "$2" 2>&1
}

# --- pointer return + pointer operand: OK --------------------------------
cat > "$WORK/ok.jal" <<'JALEOF'
include 16f887
var byte g
function getp() return pointer is
   var pointer p as var byte
   p is g
   return p
end function
var pointer q as var byte
q = getp()
forever loop end loop
JALEOF
if compile "$WORK/ok.jal" "$WORK/ok.capture" \
    && grep -q '^0 errors' "$WORK/ok.capture"; then
  echo "ok   return pointer + pointer operand compiles"
else
  echo "FAIL: pointer return"; cat "$WORK/ok.capture"
  fails=$((fails + 1))
fi

# --- pointer return + non-pointer operand: one clean error ---------------
cat > "$WORK/bad.jal" <<'JALEOF'
include 16f887
var byte g
function getp() return pointer is
   return g
end function
forever loop end loop
JALEOF
compile "$WORK/bad.jal" "$WORK/bad.capture"
if grep -q "return type is a pointer, but 'g' is not a pointer" \
    "$WORK/bad.capture" \
    && [ "$(grep -c 'error' "$WORK/bad.capture")" -eq 1 ]; then
  echo "ok   return pointer + non-pointer refuses cleanly"
else
  echo "FAIL: pointer-return type check"; cat "$WORK/bad.capture"
  fails=$((fails + 1))
fi

# --- scalar return + pointer operand: deref (pointee) --------------------
cat > "$WORK/deref.jal" <<'JALEOF'
include 16f887
var byte g = 0x5A
function getv() return byte is
   var pointer p as var byte
   p is g
   return p
end function
var byte b
b = getv()
forever loop end loop
JALEOF
if compile "$WORK/deref.jal" "$WORK/deref.capture" \
    && grep -q '^0 errors' "$WORK/deref.capture"; then
  echo "ok   return byte + pointer operand derefs the pointee"
else
  echo "FAIL: scalar deref return"; cat "$WORK/deref.capture"
  fails=$((fails + 1))
fi

# --- pointer return + pointer to a LOCAL: warns --------------------------
cat > "$WORK/local.jal" <<'JALEOF'
include 16f887
function getp() return pointer is
   var byte g
   var pointer p as var byte
   p is g
   return p
end function
var pointer q as var byte
q = getp()
forever loop end loop
JALEOF
if compile "$WORK/local.jal" "$WORK/local.capture" \
    && grep -q '^0 errors' "$WORK/local.capture" \
    && grep -q "returning a pointer to local variable 'g'" "$WORK/local.capture"; then
  echo "ok   return pointer to a local warns"
else
  echo "FAIL: local pointer return warning"; cat "$WORK/local.capture"
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails ptr-return-typecheck check(s)"
  exit 1
fi
echo "PASS: pointer-return type check"

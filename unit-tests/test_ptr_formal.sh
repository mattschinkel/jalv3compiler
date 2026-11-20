#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# `pointer in q` is a data-pointer FORMAL: it passes the ADDRESS, and the
# body reads the pointee (`x = q`), writes through it (`q = x`), and
# `pointer in out q` retargets the caller's pointer (`q is y`).  A non-
# pointer actual is a type mismatch.
#
# Usage: unit-tests/test_ptr_formal.sh [compiler]
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

# --- read + write + retarget all compile ----------------------------------
cat > "$WORK/ok.jal" <<'JALEOF'
include 16f887
var byte g = 0x42
var byte g2 = 0x22
var pointer p as var byte
p is g
procedure rd(pointer in q) is
   var byte v
   v = q
end procedure
procedure wr(pointer in q) is
   q = 0x43
end procedure
procedure rt(pointer in out q) is
   q is g2
end procedure
rd(p)
wr(p)
rt(p)
forever loop end loop
JALEOF
if compile "$WORK/ok.jal" "$WORK/ok.capture" \
    && grep -q '^0 errors' "$WORK/ok.capture"; then
  echo "ok   pointer formal read/write/retarget compiles"
else
  echo "FAIL: pointer formal"; cat "$WORK/ok.capture"
  fails=$((fails + 1))
fi

# --- non-pointer actual is a type mismatch --------------------------------
cat > "$WORK/bad.jal" <<'JALEOF'
include 16f887
procedure rd(pointer in q) is
   var byte v
   v = q
end procedure
var byte b
rd(b)
forever loop end loop
JALEOF
compile "$WORK/bad.jal" "$WORK/bad.capture"
if grep -q 'type mismatch' "$WORK/bad.capture" \
    && [ "$(grep -c 'error' "$WORK/bad.capture")" -eq 1 ]; then
  echo "ok   non-pointer actual for a pointer formal is refused"
else
  echo "FAIL: pointer-formal type check"; cat "$WORK/bad.capture"
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails ptr-formal check(s)"
  exit 1
fi
echo "PASS: pointer formal type check"

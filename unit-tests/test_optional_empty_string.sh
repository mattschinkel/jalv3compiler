#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the optional-parameter empty-string sentinel
# (fix_optional_empty_string): `p(1, "")` on a NON-string optional
# formal behaves like `p(1)` -- the declared default fills. The guard
# rails: REQUIRED formals never get the idiom, a 2+-character string
# stays a type error, and the 1-character literal keeps the long
# standing char-code coercion.
#
# Usage: unit-tests/test_optional_empty_string.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

run() { "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$1" 2>&1; }

expect_ok() { # name file
  local name=$1 file=$2 out
  out=$(run "$file")
  if echo "$out" | grep -q "0 errors"; then
    echo "ok   $name"
  else
    echo "FAIL $name:"
    echo "$out" | grep "error" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

expect_err() { # name file substring
  local name=$1 file=$2 want=$3 out
  out=$(run "$file")
  if echo "$out" | grep -q "$want"; then
    echo "ok   $name"
  else
    echo "FAIL $name: expected '$want' in:"
    echo "$out" | grep -v "^generating\|^writing\|tokens\|^jal " | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# --- the feature: "" on a non-string optional = omitted ---------------
cat > "$WORK/s1.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
procedure report(byte in tag, optional byte in level default 7) is
end procedure
procedure mix(byte in base, optional word in scale default 5,
    optional byte in bias default 1) is
end procedure
report(1, "")
mix(20, "", 9)
forever loop
end loop
EOF
expect_ok "report(\"\") compiles (scalar + mid-list shapes)" "$WORK/s1.jal"

# --- open-array optional: "" passes the default literal ---------------
cat > "$WORK/s2.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
procedure report(byte in tag, optional byte in d[] default "1s") is
end procedure
report(1, "")
forever loop
end loop
EOF
expect_ok "report(\"\") compiles (open-array default fills)" "$WORK/s2.jal"

# --- REQUIRED formal: no idiom ----------------------------------------
cat > "$WORK/s3.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
procedure report(byte in tag) is
end procedure
report("")
forever loop
end loop
EOF
expect_err "required formal + \"\" stays a type error" "$WORK/s3.jal" "type mismatch"

# --- 2+-character string: stays a type error --------------------------
cat > "$WORK/s4.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
procedure report(byte in tag, optional byte in level default 7) is
end procedure
report(1, "xy")
forever loop
end loop
EOF
expect_err "2+-character string stays a type error" "$WORK/s4.jal" "type mismatch"

# --- 1-character literal: char-code coercion (pre-existing idiom) -----
cat > "$WORK/s5.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
procedure report(byte in tag, optional byte in level default 7) is
end procedure
report(1, "x")
forever loop
end loop
EOF
expect_ok "1-char literal keeps the char-code coercion" "$WORK/s5.jal"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all optional empty-string sentinel pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

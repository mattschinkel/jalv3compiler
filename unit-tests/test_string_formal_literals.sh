#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for string-formal literal marshalling (fix_string_formal_literals):
# a const/fixed string LITERAL actual into a DYNAMIC-string formal
# stages into a compiler string cell -- ANY length works, including
# empty (pre-fix only 3-char literals compiled, by the
# count==handle-size coincidence; "ab"/"abcd"/"" were "type mismatch").
# Optional-string DEFAULTS are staged the same way, so the omitted-arg
# default reads as a real string (COUNT = its length; pre-fix the
# callee read const-table bytes as a heap handle).
#
# Usage: unit-tests/test_string_formal_literals.sh [compiler]
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
  if echo "$out" | grep -q "0 errors, 0 warnings"; then
    echo "ok   $name"
  else
    echo "FAIL $name:"
    echo "$out" | grep -E "error|warning" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

base() { # $1 = literal
cat << EOF
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 64
include strings_dynamic
procedure report(byte in tag, string in s) is
end procedure
report(1, $1)
forever loop
end loop
EOF
}

for lit in '""' '"ab"' '"abc"' '"abcd"' '"a much longer literal"'; do
  base "$lit" > "$WORK/lit.jal"
  expect_ok "literal $lit into a dynamic-string formal" "$WORK/lit.jal"
done

# optional-string default + omitted arg compiles (staged default)
cat > "$WORK/od.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 64
include strings_dynamic
procedure report(byte in tag, optional string in s default "dflt") is
end procedure
report(1)
report(1, "given")
forever loop
end loop
EOF
expect_ok "optional-string default + omitted arg" "$WORK/od.jal"

# FIXED-string formal: literal actuals stage into a var string[N] cell
for lit in '""' '"hello"' '"0123456789abcdef"'; do
cat > "$WORK/flit.jal" << EOF
include 16f1825
pragma target clock 20_000_000
procedure p(string[16] in s) is
end procedure
p($lit)
forever loop
end loop
EOF
  expect_ok "fixed formal + literal $lit" "$WORK/flit.jal"
done

# over-capacity literal: clean compile error naming both numbers
cat > "$WORK/fover.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
procedure p(string[16] in s) is
end procedure
p("0123456789abcdefg")
forever loop
end loop
EOF
out=$(run "$WORK/fover.jal")
if echo "$out" | grep -q "string literal is 17 characters but the fixed-string parameter's capacity is 16"; then
  echo "ok   over-capacity literal errors with both numbers"
else
  echo "FAIL over-capacity literal error:"
  echo "$out" | grep "error" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# same-capacity var actual: unchanged direct pass
cat > "$WORK/fvar.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
var string[16] m
procedure p(string[16] in s) is
end procedure
p(m)
forever loop
end loop
EOF
expect_ok "fixed formal + same-capacity var (direct)" "$WORK/fvar.jal"

# dynamic actual (var string) still passes by handle -- no extra staging
cat > "$WORK/dyn.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 64
include strings_dynamic
var string t
procedure report(byte in tag, string in s) is
end procedure
report(1, t)
forever loop
end loop
EOF
expect_ok "dynamic actual passes by handle (unchanged)" "$WORK/dyn.jal"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all string-formal literal pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

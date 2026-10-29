#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the Option A string heap-sizing advisory
# (fix_string_heap_sizing_warning): warn when _string_heap_size is below
# the recommendation for the const literals assigned (payload + headers
# across all handles -- user strings AND retained _string_chainN cells --
# x1.25 headroom), stay silent when the heap is roomy, and SKIP entirely
# when any runtime-length assign exists (string_from_array, concat with
# runtime operands) -- the aggregate is only computable for all-const
# lengths.
#
# Usage: unit-tests/test_string_heap_sizing.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

run() { # file -> full compiler output
  "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$1" 2>&1
}

expect_warn() { # name file substring
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

expect_clean() { # name file
  local name=$1 file=$2 out
  out=$(run "$file")
  if echo "$out" | grep -q "0 errors, 0 warnings"; then
    echo "ok   $name"
  else
    echo "FAIL $name: expected 0 warnings, got:"
    echo "$out" | grep "warning:" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# --- 1: literal assigns under-size -> warn with both numbers ---------
cat > "$WORK/w1.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 9
var string s
var string t
procedure demo() is
   s = "hello"
   t = "hi"
end procedure
demo()
forever loop
end loop
EOF
expect_warn "literal assigns: heap 9, recommended 17" "$WORK/w1.jal" \
  "recommend at least 17 (7 payload + 3-byte headers across 2 strings"

# --- 2: roomy heap -> silent -----------------------------------------
sed "s/_string_heap_size = 9/_string_heap_size = 64/" "$WORK/w1.jal" \
  > "$WORK/w2.jal"
expect_clean "roomy heap is silent" "$WORK/w2.jal"

# --- 3: runtime-length assign (from_array) -> SKIP -------------------
cat > "$WORK/w3.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 9
include strings_dynamic
var string s
var byte buf[4]
procedure demo() is
   buf[0] = 65
   buf[1] = 66
   var byte r
   r = string_from_array(s, buf)
end procedure
demo()
forever loop
end loop
EOF
expect_clean "runtime-length assign skips the advisory" "$WORK/w3.jal"

# --- 4: const chain counted (payload = static total) ------------------
cat > "$WORK/w4.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 10
var string s
procedure demo() is
   s = "abc" & "def" & "ghi"
end procedure
demo()
forever loop
end loop
EOF
expect_warn "const chain: heap 10, recommended 15" "$WORK/w4.jal" \
  "recommend at least 15 (9 payload"

# --- 5: copy of a statically-known string propagates the bound --------
cat > "$WORK/w5.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 12
var string a
var string b
procedure demo() is
   a = "12345"
   b = a & "!"
end procedure
demo()
forever loop
end loop
EOF
expect_warn "copy + const chain: heap 12, recommended 22" "$WORK/w5.jal" \
  "recommend at least 22 (11 payload + 3-byte headers across 2 strings"

# --- 6: runtime-number operand -> SKIP (scratch capacity is not len) --
cat > "$WORK/w6.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 8
var string s
var word lvl
procedure demo() is
   lvl = 3
   s = "v=" & lvl & "!"
end procedure
demo()
forever loop
end loop
EOF
expect_clean "runtime-number chain skips the advisory" "$WORK/w6.jal"

# --- 7: const-number render IS counted --------------------------------
cat > "$WORK/w7.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
const word _string_heap_size = 8
var string s
procedure demo() is
   s = "v=" & 123 & "!"
end procedure
demo()
forever loop
end loop
EOF
expect_warn "const-number chain counted (6 payload)" "$WORK/w7.jal" \
  "recommend at least 12 (6 payload"

# --- 8: index-width feedback: crossing 255 widens the header ----------
{
  echo "include 16f1825"
  echo "pragma target clock 20_000_000"
  echo "const word _string_heap_size = 250"
  for i in $(seq 0 11); do echo "var string s$i"; done
  echo "procedure demo() is"
  for i in $(seq 0 11); do
    echo "   s$i = \"01234567890123456789\""
  done
  echo "end procedure"
  echo "demo()"
  echo "forever loop"
  echo "end loop"
} > "$WORK/w8.jal"
expect_warn "index feedback: 6-byte headers, 390" "$WORK/w8.jal" \
  "recommend at least 390 (240 payload + 6-byte headers across 12 strings"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all string heap-sizing advisory pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

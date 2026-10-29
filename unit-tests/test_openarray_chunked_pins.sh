#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the open-array-over-holes specialization
# (fix_openarray_chunked_specialization): DIRECT calls with chunked
# actuals compile and walk the chunks; the shapes that CANNOT be
# specialized keep the clean refusals -- indirect (pointer) calls and
# pointer binds.
#
# Usage: unit-tests/test_openarray_chunked_pins.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

run() { # file -> full compiler output
  "$CC" -s "$REPO/third_party/jallib/lib" "$1" 2>&1
}

expect_error() { # name file substring
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
    echo "FAIL $name: expected 0 errors, 0 warnings, got:"
    echo "$out" | grep -E "error|warning" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# --- 1: direct call over a chunked actual specializes -> compiles clean
cat > "$WORK/d1.jal" << 'EOF'
include 16f1455
pragma target clock 16_000_000
var byte arr[300]
var volatile byte r = 0
function _f(byte in b[]) return byte is
   return b[200]
end function
r = _f(arr)
EOF
expect_clean "direct_call_specializes" "$WORK/d1.jal"

# --- 2: indirect (pointer) call with a chunked actual -> refusal naming
# -------- the open-array escape
cat > "$WORK/e2.jal" << 'EOF'
include 16f1455
pragma target clock 16_000_000
var byte arr[300]
var volatile byte r = 0
function _f(byte in b[]) return byte is
   return b[200]
end function
var pointer fp as function (byte in b[]) return byte
fp is _f
arr[200] = 9
r = fp(arr)
r = r + arr[299]
EOF
expect_error "indirect_call_refuses" "$WORK/e2.jal" \
  "passed to a procedure as an open-array actual"

# --- 3a: UNESCAPED pointer bind to a chunked array -> allowed; the
# -------- derefs walk the chunks (escape analysis)
cat > "$WORK/e3.jal" << 'X1'
include 16f1455
pragma target clock 16_000_000
var byte arr[300]
var pointer p as var byte[300]
p is arr
var volatile byte r = 0
var word k
for 300 using k loop
   p[k] = byte(k)
end loop
k = 299
r = p[k] - 43
X1
expect_clean "unescaped_pointer_bind_walks" "$WORK/e3.jal"

# --- 3b: ESCAPED pointer bind (passed to a procedure) -> refusal
cat > "$WORK/e3b.jal" << 'X2'
include 16f1455
pragma target clock 16_000_000
var byte arr[300]
var pointer p as var byte[300]
p is arr
var volatile byte r = 0
var pointer q2 as var byte[300]
q2 is p
arr[200] = 42
q2[5] = 1
r = arr[200]
X2
expect_error "escaped_pointer_bind_refuses" "$WORK/e3b.jal" \
  "bound to a pointer"

# --- 3c: const-offset, const, and computed subscripts through a
# -------- walked bind all walk now (offset-aware scratch + const
# -------- chunk-map resolution)
cat > "$WORK/e3c.jal" << 'X3'
include 16f1455
pragma target clock 16_000_000
var byte arr[300]
var pointer p as var byte[300]
p is arr
var volatile byte r = 0
var word i
arr[200] = 42
for 4 using i loop
   p[256 + i] = 7
end loop
i = 298
r = p[i] + p[299] + p[256 + 1]
X3
expect_clean "computed_subscript_bind_walks" "$WORK/e3c.jal"

# --- 4: single-bank array through an open-array formal -> unchanged
# -------- (no chunking, no specialization needed)
cat > "$WORK/d4.jal" << 'EOF'
include 16f1455
pragma target clock 16_000_000
var byte arr[60]
var volatile byte r = 0
function _f(byte in b[]) return byte is
   return b[59]
end function
arr[59] = 7
r = _f(arr)
EOF
expect_clean "single_bank_call_clean" "$WORK/d4.jal"

# --- 5: byte*N bitfield pointee parses + binds (8x-RAM placement caveat,
# -------- scratch-runtime-proven; see fix doc)
cat > "$WORK/e5.jal" << 'X5'
include 16f1455
pragma target clock 16_000_000
var byte*3 arr[40]
var pointer p as var byte*3[40]
p is arr
var volatile byte r = 0
var word k
for 40 using k loop
   arr[k] = k & 7
end loop
k = 10
r = byte(p[k])
X5
expect_clean "bitfield_pointee_parses_binds" "$WORK/e5.jal"

# --- 6: bit*N runtime-indexed element access compiles (width <= 8) and
# -------- width > 8 refuses cleanly
cat > "$WORK/e6.jal" << 'X6'
include 16f1455
pragma target clock 16_000_000
var bit*3 arr[40]
var volatile byte r = 0
var byte k
for 40 using k loop
   arr[k] = k & 7
end loop
k = 17
r = byte(arr[k])
X6
expect_clean "bitN_runtime_elements_compile" "$WORK/e6.jal"

cat > "$WORK/e7.jal" << 'X7'
include 16f1455
pragma target clock 16_000_000
var bit*9 arr[10]
var byte k
k = 3
arr[k] = 5
X7
expect_error "bitN_width_above_8_refuses" "$WORK/e7.jal" \
  "width above 8 bits"

if [[ $fails -eq 0 ]]; then
  echo "all openarray-chunked pins passed"
  exit 0
fi
echo "$fails openarray-chunked pin(s) FAILED"
exit 1

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Negative pins for the multibank array placement refusals (elem_sz fix).
# The 16f18857 false-refusal fix (fixes/fix_16f18857_multibank_elem_sz.md)
# must NOT turn the allocator into accept-everything: a program whose
# array genuinely does not fit still gets the clean, variable-naming
# error, and a pointer-bound array still gets the holed-run refusal.
#
# Usage: unit-tests/test_multibank_array_errors.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

check_clean() { # name file
  local name=$1 file=$2 out
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$file" 2>&1)
  if echo "$out" | grep -q "0 errors, 0 warnings"; then
    echo "ok   $name"
  else
    echo "FAIL $name: expected clean compile, got:"
    echo "$out" | grep -E "error|warning" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

check() { # name file expected-substring
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

# --- over real capacity: 16f18857 has 4093 bytes free at witness time ---
cat > "$WORK/over.jal" << 'EOF'
include 16f18857
pragma target clock 32_000_000
var byte big[4094]
var word i
forever loop
  for 4094 using i loop
    big[i - 1] = byte(i)
  end loop
end loop
EOF
check "byte[4094] refused (over real 4093-byte ceiling)" "$WORK/over.jal" \
  "exceeds total available memory space"
check "refusal names the variable" "$WORK/over.jal" "Array of 'big' (4094 bytes)"

# --- pointer-bound bank-spanning array: an UNESCAPED bind now walks
# --- the chunks (escape analysis, 2026-10-26); an ESCAPED one keeps
# --- the holed-run refusal. (The pointer must be USED through the
# slot so the bind registers; byte shape keeps this pin independent
# of the pointer-bind semantics script, which covers word binds
# separately.)
cat > "$WORK/ptr.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
var byte buf[200]
var pointer p as var byte[200]
var byte x
var byte idx
procedure touch() is
   p is buf
   idx = 5
   buf[0] = 42
   x = p[idx]
   p[idx] = x
end procedure
touch()
forever loop
end loop
EOF
check_clean "unescaped pointer-bound bank-spanning array walks" \
  "$WORK/ptr.jal"

cat > "$WORK/ptr_esc.jal" << 'EOF2'
include 16f1825
pragma target clock 20_000_000
var byte buf[200]
var pointer p as var byte[200]
var pointer q as var byte[200]
var byte x
var byte idx
procedure touch() is
   p is buf
   q is p
   idx = 5
   buf[0] = 42
   x = q[idx]
end procedure
touch()
forever loop
end loop
EOF2
check "escaped pointer-bound bank-spanning array refused" \
  "$WORK/ptr_esc.jal" "is bound to a pointer"

# --- WORD twin: the shape that produced the 2026-10-15 "Attempt to assign
# to a lookup table" follow-up.  It must reach the pointer refusal, not
# pic_assign_to_ptr's lookup error -- the report was the never-written-array
# MUST-COMPILE acceptance shapes (16f18857, the non-monotonic false-refusal
# witnesses from fixes/fix_16f18857_multibank_elem_sz.md -- word[400] and
# byte[720] compile; the compile-only 16f18857 file was dropped from the
# old tests/ tree in the 2026-10-29 restructure, so pin them here):
cat > "$WORK/mb_word.jal" << 'EOF4'
include 16f18857
pragma target clock 32_000_000
var word big_words[400]
var word k
var word acc
for 8 using k loop
   big_words[k] = 1000 + k
   acc = acc + big_words[k]
end loop
forever loop
end loop
EOF4
check_clean "16f18857 word[400] multibank acceptance" \
  "$WORK/mb_word.jal"

cat > "$WORK/mb_byte.jal" << 'EOF5'
include 16f18857
pragma target clock 32_000_000
var byte big_bytes[720]
var byte k
var byte acc
for 8 using k loop
   big_bytes[k] = k
   acc = acc + big_bytes[k]
end loop
forever loop
end loop
EOF5
check_clean "16f18857 byte[720] multibank acceptance" \
  "$WORK/mb_byte.jal"

# const fold (already fixed); this pin keeps the stale misdiagnosis from
# returning. ---
cat > "$WORK/ptr_word.jal" << 'EOF'
include 16f1825
pragma target clock 20_000_000
var word buf[200]
var pointer p as var word[200]
var word x
var byte idx
procedure touch() is
   p is buf
   idx = 5
   buf[0] = 42
   x = p[idx]
   p[idx] = x
end procedure
touch()
forever loop
end loop
EOF
# word-pointee subscripts scale the index (idx*2 -> a computed temp)
# and now walk too (offset-aware scratch handles computed indices)
check_clean "unescaped pointer-bound WORD array walks" \
  "$WORK/ptr_word.jal"

# escaped WORD twin keeps the original regression intent: reach the
# pointer refusal, never the stale "Attempt to assign to a lookup
# table" misdiagnosis
cat > "$WORK/ptr_word_esc.jal" << 'EOF3'
include 16f1825
pragma target clock 20_000_000
var word buf[200]
var pointer p as var word[200]
var pointer q as var word[200]
var word x
var byte idx
procedure touch() is
   p is buf
   q is p
   idx = 5
   buf[0] = 42
   x = q[idx]
end procedure
touch()
forever loop
end loop
EOF3
check "escaped pointer-bound WORD array refused via pointer path" \
  "$WORK/ptr_word_esc.jal" "is bound to a pointer"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all multibank array error pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

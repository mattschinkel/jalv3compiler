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

# --- alias-group element straddle (fix_frame_chunk_member_maps): two
# --- frame-reused locals share one virtual slot; the chunked layout is
# --- element-aligned for the HEAD's element size only, so a WORD-array
# --- member over odd fragments would straddle chunk boundaries and its
# --- linear/walk addressing cannot be emitted -- the clean no-space
# --- refusal must fire (pre-fix this compiled 0 errors and corrupted).
cat > "$WORK/straddle.jal" << 'EOF4'
include 16f1825
pragma target clock 4000000
pragma target OSC HS
pragma target WDT DISABLED
var byte b0[75]
var byte b1[75]
var byte b2[75]
var byte b3[75]
var byte b4[75]
var byte b5[75]
var byte b6[75]
var byte b7[75]
var byte b8[75]
var byte b9[75]
var byte ba[75]
var byte bb[75]
var byte bc[48]
var volatile byte r = 0
var byte j
b0[0] = 1
b1[0] = 2
b2[0] = 3
b3[0] = 4
b4[0] = 5
b5[0] = 6
b6[0] = 7
b7[0] = 8
b8[0] = 9
b9[0] = 10
ba[0] = 11
bb[0] = 12
bc[0] = 13
bc[47] = 14
procedure p1() is
  var byte locb[40]
  var byte k
  for 40 using k loop
    locb[k] = k + 1
  end loop
  r = locb[39]
end procedure
procedure p2() is
  var word locw[20]
  var byte k
  for 20 using k loop
    locw[k] = k + 1
  end loop
  r = byte(locw[19])
end procedure
p1()
p2()
r = r + b0[0] + b1[0] + b2[0] + b3[0] + b4[0] + b5[0] + b6[0] + b7[0] + b8[0] + b9[0] + ba[0] + bb[0] + bc[0] + bc[47]
EOF4
# 2026-10-30 frame-chunk group fix: the alias-group straddle now refuses
# through the open-array-actual guard (the callee cannot walk a holed
# placement) instead of the generic capacity message -- still a clean,
# trial-safe refusal that names the member.
check "straddling word member of an alias group refused" \
  "$WORK/straddle.jal" "needs 40 contiguous bytes"
check "straddle refusal names the word member" \
  "$WORK/straddle.jal" "'locw'"

# --- the healthy counterpart: a BYTE-array member (same element size
# --- as the head) over the same fragments chunk-walks fine -- the
# --- refusal above must be the straddle guard, not a capacity lie.
sed 's/var word locw\[20\]/var byte locb2[40]/; s/locw\[k\] = k + 1/locb2[k] = k + 1/; s/byte(locw\[19\])/locb2[39]/' \
  "$WORK/straddle.jal" > "$WORK/straddle_ok.jal"
check_clean "byte member over the same fragments compiles" "$WORK/straddle_ok.jal"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all multibank array error pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

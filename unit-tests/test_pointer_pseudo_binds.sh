#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for pointer-to-pseudo-variable binds (fix_pointer_pseudo_token_guard):
# reads/writes through a scalar pointer bound to a 'get/'put pseudo must
# route to the accessors regardless of tokenizer state -- EOF tails and
# operator continuations (`p + 1`, `p == k`) used to silently fall
# through to a slot dereference of the GET code address. Also pins the
# two clean REFUSALS: binding a pseudo into a pointer-array slot (no
# accessor dispatch through slots exists) and copying a pseudo-bound
# pointer with `q = p` (the read swap consumes the pointer; the
# store-through path drops the value).
#
# Usage: unit-tests/test_pointer_pseudo_binds.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

expect_ok() { # name file -- must compile with 0 errors
  local name=$1 file=$2 out
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$file" 2>&1)
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

# ---------------------------------------------------------------- headers
PSEUDO='include 16f877
var byte shadow = 0
function pv'"'"'get return byte is
   return shadow
end function
procedure pv'"'"'put(byte in v) is
   shadow = v
end procedure
'
GETONLY='include 16f877
var byte shadow = 0
function gro'"'"'get return byte is
   return shadow
end function
'

# ------------------------------------------------- accessor routing, clean
printf '%s' "$PSEUDO" > "$WORK/eofwrite.jal"
cat >> "$WORK/eofwrite.jal" <<'EOF'
var pointer b as var byte
b is pv
shadow = 55
b = 66
EOF
expect_ok "EOF-tail write through pseudo pointer" "$WORK/eofwrite.jal"

printf '%s' "$PSEUDO" > "$WORK/opread.jal"
cat >> "$WORK/opread.jal" <<'EOF'
var pointer b as var byte
var byte x = 0
b is pv
shadow = 55
x = b + 1
if b == 55 then
   x = 2
end if
EOF
expect_ok "operator-context reads through pseudo pointer" "$WORK/opread.jal"

# ------------------------------------------------------------- refusals
printf '%s' "$GETONLY" > "$WORK/getonly.jal"
cat >> "$WORK/getonly.jal" <<'EOF'
var pointer b as var byte
b is gro
b = 42
EOF
expect_err "get-only pseudo write errors" "$WORK/getonly.jal" "No PUT procedure"

printf '%s' "$PSEUDO" > "$WORK/slot.jal"
cat >> "$WORK/slot.jal" <<'EOF'
var byte x = 0
var pointer slots[2] as var byte
slots[0] is pv
x = slots[0]
slots[0] = 55
EOF
expect_ok "pseudo bind to const pointer-array slot dispatches" "$WORK/slot.jal"

printf '%s' "$PSEUDO" > "$WORK/slot_rt_read.jal"
cat >> "$WORK/slot_rt_read.jal" <<'EOF'
var byte x = 0
var byte k = 0
var pointer slots[2] as var byte
slots[0] is pv
x = slots[k]
EOF
expect_ok "runtime-index read of pseudo slot element dispatches" \
  "$WORK/slot_rt_read.jal"

printf '%s' "$PSEUDO" > "$WORK/slot_rt_write.jal"
cat >> "$WORK/slot_rt_write.jal" <<'EOF'
var byte k = 0
var pointer slots[2] as var byte
slots[0] is pv
slots[k] = 55
EOF
expect_ok "runtime-index write of pseudo slot element dispatches" \
  "$WORK/slot_rt_write.jal"

printf '%s' "$PSEUDO" > "$WORK/slot_rt_bind.jal"
cat >> "$WORK/slot_rt_bind.jal" <<'EOF'
var byte k = 0
var pointer slots[2] as var byte
slots[k] is pv
EOF
expect_err "runtime-index pseudo bind to slot refused" \
  "$WORK/slot_rt_bind.jal" "constant index"

printf '%s' "$PSEUDO" > "$WORK/copy.jal"
cat >> "$WORK/copy.jal" <<'EOF'
var pointer p as var byte
var pointer q as var byte
p is pv
q = p
EOF
expect_err "q = p pseudo-bound copy refused" "$WORK/copy.jal" \
  "is bound to a pseudo-variable"

# ------------------------------------------ indexed pseudo arrays (jallib
# fat32_cluster / network_byte / large_array_6 idiom: 'get(word in idx),
# 'put(word in idx, byte in v))
cat > "$WORK/parr_direct.jal" <<'EOF'
include 16f877
var byte mem[8]
var byte x = 0
var word i = 3
function parr'get(word in idx) return byte is
   return mem[idx]
end function
procedure parr'put(word in idx, byte in v) is
   mem[idx] = v
end procedure
mem[3] = 7
x = parr[3]
parr[3] = 42
x = parr[i]
EOF
expect_ok "indexed pseudo array direct subscripts" "$WORK/parr_direct.jal"

cat > "$WORK/parr_scalar.jal" <<'EOF'
include 16f877
var byte mem[8]
function parr'get(word in idx) return byte is
   return mem[idx]
end function
procedure parr'put(word in idx, byte in v) is
   mem[idx] = v
end procedure
var pointer p as var byte
p is parr
EOF
expect_err "indexed pseudo bind to scalar pointer refused" "$WORK/parr_scalar.jal" \
  "cannot be bound to a scalar pointer"

cat > "$WORK/parr_ptr_array.jal" <<'EOF'
include 16f877
var byte mem[8]
var byte x = 0
var word i = 5
function parr'get(word in idx) return byte is
   return mem[idx]
end function
procedure parr'put(word in idx, byte in v) is
   mem[idx] = v
end procedure
var pointer p as var byte[8]
p is parr
x = p[3]
p[3] = 44
x = p[i]
p[i] = 66
EOF
expect_ok "pointer-to-array over pseudo array binds and dispatches" \
  "$WORK/parr_ptr_array.jal"

cat > "$WORK/parr_bare.jal" <<'EOF'
include 16f877
var byte mem[8]
function parr'get(word in idx) return byte is
   return mem[idx]
end function
procedure parr'put(word in idx, byte in v) is
   mem[idx] = v
end procedure
var pointer p as var byte[8]
p is parr
p = 42
EOF
expect_err "bare store through pseudo-array pointer refused" "$WORK/parr_bare.jal" \
  "expected"

cat > "$WORK/parr_count.jal" <<'EOF'
include 16f877
var byte mem[8]
var byte x = 0
function parr'get(word in idx) return byte is
   return mem[idx]
end function
var pointer p as var byte[8]
p is parr
x = COUNT(p)
EOF
expect_err "COUNT of pseudo-array pointer refused" "$WORK/parr_count.jal" \
  "not known"

cat > "$WORK/parr_copy.jal" <<'EOF'
include 16f877
var byte mem[8]
function parr'get(word in idx) return byte is
   return mem[idx]
end function
var pointer p as var byte[8]
var pointer q as var byte[8]
p is parr
q is p
EOF
expect_err "q is p copy of pseudo-array pointer refused" "$WORK/parr_copy.jal" \
  "bind the pseudo directly"

# ------------------------------------------------- unaffected shapes, clean
printf '%s' "$PSEUDO" > "$WORK/plaincopy.jal"
cat >> "$WORK/plaincopy.jal" <<'EOF'
var byte v1 = 0
var pointer pa as var byte
var pointer pb as var byte
pa is v1
pb is pa
EOF
expect_ok "pointer copy via is still compiles" "$WORK/plaincopy.jal"

printf '%s' "$PSEUDO" > "$WORK/plainstore.jal"
cat >> "$WORK/plainstore.jal" <<'EOF'
var byte v1 = 0
var byte v2 = 0
var pointer pa as var byte
var pointer pb as var byte
pa is v1
pb is v2
pb = pa
EOF
expect_ok "store-through q = p with both bound compiles" "$WORK/plainstore.jal"

printf '%s' "$PSEUDO" > "$WORK/plainunbound.jal"
cat >> "$WORK/plainunbound.jal" <<'EOF'
var byte v1 = 0
var pointer pa as var byte
var pointer pb as var byte
pa is v1
pb = pa
EOF
expect_err "q = p through never-bound q refused" "$WORK/plainunbound.jal" \
  "is used but never bound"

cat > "$WORK/fpslot.jal" <<'EOF'
include 16f877
function fnc(byte in x) return byte is
   return x + 1
end function
var pointer fps[2] as function(byte in) return byte
var byte r = 0
fps[0] is fnc
r = fps[0](7)
EOF
expect_ok "function-pointer slot bind unaffected" "$WORK/fpslot.jal"

# ------------------------------------- unbound / self-bind access holes
cat > "$WORK/ub_store.jal" <<'EOF'
include 16f877
var pointer p as var byte
p = 42
EOF
expect_err "store through never-bound pointer refused" "$WORK/ub_store.jal" \
  "is used but never bound"

cat > "$WORK/ub_sub.jal" <<'EOF'
include 16f877
var byte x = 0
var pointer p as var byte[8]
x = p[3]
EOF
expect_err "subscript through never-bound pointer refused" "$WORK/ub_sub.jal" \
  "is used but never bound"

cat > "$WORK/self_bind.jal" <<'EOF'
include 16f877
var byte x = 0
var pointer p as var byte
p is p
x = p
EOF
expect_err "pointer self-bind refused" "$WORK/self_bind.jal" \
  "cannot be bound to itself"

cat > "$WORK/slot_zero.jal" <<'EOF'
include 16f877
var byte x = 0
var pointer slots[2] as var byte
x = slots[1]
EOF
expect_err "pointer-array with no binds refused" "$WORK/slot_zero.jal" \
  "no element is ever bound"

cat > "$WORK/slot_partial.jal" <<'EOF'
include 16f877
var byte v1 = 0
var byte x = 0
var pointer slots[2] as var byte
slots[0] is v1
x = slots[1]
EOF
expect_err "partially-bound slot element refused" "$WORK/slot_partial.jal" \
  "is used but never bound"

# controls: runtime-indexed binds cover const-indexed uses; bound copies fine
cat > "$WORK/slot_rt_ok.jal" <<'EOF'
include 16f877
var byte v1 = 0
var byte x = 0
var byte k = 0
var pointer slots[2] as var byte
for 2 using k loop
   slots[k] is v1
end loop
x = slots[1]
EOF
expect_ok "runtime-indexed slot binds cover const uses" "$WORK/slot_rt_ok.jal"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all pointer-pseudo bind pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

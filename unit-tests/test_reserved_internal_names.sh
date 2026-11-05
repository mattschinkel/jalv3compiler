#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Compiler-owned identifiers are reserved, and a malformed _string_heap_size
# says so ONCE instead of producing a 25-diagnostic cascade.
#
# Recurring defect family (no automated coverage before this pin):
#   fixes/fix_reserved_internal_names.md -- compiler finds its own scratch by
#       NAME (`_pfile_strc_cpt`, `_pic_temp`, ...); a user declaration was
#       silently adopted, so the program and the compiler shared storage.
#   fixes/fix_string_heap_compact_outline_call_internal_name_guard.md --
#       the same class in an emitted helper name.
#
# The fix doc ends with "a negative fixture could be added to
# unit-tests/negative/ if the message ever needs pinning"; this is that pin,
# with the exact error COUNT (the one-error contract is the fix).
#
# Usage: unit-tests/test_reserved_internal_names.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
INC="$REPO/demo_lib;$REPO/third_party/jallib/lib"
fails=0

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

out_for() { echo "$WORK/$(basename "$1" .jal).out"; }

compile() { # source
  local src=$1
  timeout 60 "$CC" -s "$INC" -no-codfile "$src" >"$(out_for "$src")" 2>&1
  echo $?
}

expect_one_error() { # name source message
  local name=$1 src=$2 want=$3 rc
  rc=$(compile "$src")
  local out; out=$(out_for "$src")
  if [ "$rc" -eq 0 ]; then
    bad "$name: compiled clean (expected a reserved-name error)"
    return
  fi
  if ! grep -qF "$want" "$out"; then
    bad "$name: missing diagnostic '$want'" "$(grep -E 'reserved|heap_size' "$out")"
    return
  fi
  if ! grep -qE '^1 errors, 0 warnings$' "$out"; then
    bad "$name: error count is not exactly 1" \
      "$(grep -E '^[0-9]+ errors,' "$out")"
    return
  fi
  ok "$name: one error, message pinned"
}

# ---------------------------------------------------- exact reserved names
cat >"$WORK/r_cpt.jal" <<'EOF'
include 16f877
var byte _pfile_strc_cpt
forever loop
end loop
EOF
expect_one_error "compaction work table" "$WORK/r_cpt.jal" \
  "'_pfile_strc_cpt' is reserved for the compiler"

cat >"$WORK/r_temp.jal" <<'EOF'
include 16f877
var byte _pic_temp
forever loop
end loop
EOF
expect_one_error "PIC scratch byte" "$WORK/r_temp.jal" \
  "'_pic_temp' is reserved for the compiler"

cat >"$WORK/r_task.jal" <<'EOF'
include 16f877
var byte _task_list
forever loop
end loop
EOF
expect_one_error "task slot table" "$WORK/r_task.jal" \
  "'_task_list' is reserved for the compiler"

# ------------------------------------- reserved string-runtime objects
cat >"$WORK/r_sheap.jal" <<'EOF'
include 16f877
var byte _string_heap
forever loop
end loop
EOF
expect_one_error "string heap storage" "$WORK/r_sheap.jal" \
  "'_string_heap' is reserved for the compiler"

cat >"$WORK/r_sfree.jal" <<'EOF'
include 16f877
var byte _string_free_head
forever loop
end loop
EOF
expect_one_error "string free-list head" "$WORK/r_sfree.jal" \
  "'_string_free_head' is reserved for the compiler"

# the runtime chunk map and its walk scratch are per-formal now, and both carry
# the `_pic_chunk` prefix (fixes/fix_openarray_user_array_percall.md)
cat >"$WORK/r_chunkmap.jal" <<'EOF'
include 16f877
var byte _pic_chunkmap_7
forever loop
end loop
EOF
expect_one_error "per-formal chunk map" "$WORK/r_chunkmap.jal" \
  "uses the compiler-reserved prefix '_pic_chunk'"

cat >"$WORK/r_chunkcnt.jal" <<'EOF'
include 16f877
var byte _pic_chunk_cnt
forever loop
end loop
EOF
expect_one_error "chunk-walk scratch" "$WORK/r_chunkcnt.jal" \
  "uses the compiler-reserved prefix '_pic_chunk'"

# --------------------------------------------------------- reserved prefixes
cat >"$WORK/r_fixarg.jal" <<'EOF'
include 16f877
var byte _string_fixarg7
forever loop
end loop
EOF
expect_one_error "marshalling buffer prefix" "$WORK/r_fixarg.jal" \
  "uses the compiler-reserved prefix '_string_fixarg'"

cat >"$WORK/r_chain.jal" <<'EOF'
include 16f877
var byte _string_chain7
forever loop
end loop
EOF
expect_one_error "chain cell prefix" "$WORK/r_chain.jal" \
  "uses the compiler-reserved prefix '_string_chain'"

cat >"$WORK/r_flt.jal" <<'EOF'
include 16f877
var byte _string_flt0
forever loop
end loop
EOF
expect_one_error "float buffer prefix" "$WORK/r_flt.jal" \
  "uses the compiler-reserved prefix '_string_flt'"

cat >"$WORK/r_dec.jal" <<'EOF'
include 16f877
var byte _string_dec0
forever loop
end loop
EOF
expect_one_error "decimal text prefix" "$WORK/r_dec.jal" \
  "uses the compiler-reserved prefix '_string_dec'"

cat >"$WORK/r_num.jal" <<'EOF'
include 16f877
var byte _string_num0
forever loop
end loop
EOF
expect_one_error "integer scratch prefix" "$WORK/r_num.jal" \
  "uses the compiler-reserved prefix '_string_num'"

cat >"$WORK/r_bss.jal" <<'EOF'
include 16f877
var byte _pic_bss_x
forever loop
end loop
EOF
expect_one_error "BSS block prefix" "$WORK/r_bss.jal" \
  "uses the compiler-reserved prefix '_pic_bss_'"

cat >"$WORK/r_put.jal" <<'EOF'
include 16f877
pragma target string_heap_size 64
function _string_put(string in out d, word in i, byte in c) return bit is
   d[i] = c
   return TRUE
end function
forever loop
end loop
EOF
expect_one_error "synthesized store body name" "$WORK/r_put.jal" \
  "'_string_put' is reserved for the compiler"

# --------------------------------- removed legacy constant: ONE diagnostic
cat >"$WORK/r_heap.jal" <<'EOF'
const word _string_heap_size = 64
include 16f877
var string s
s = "hello"
forever loop
end loop
EOF
expect_one_error "legacy _string_heap_size is refused" "$WORK/r_heap.jal" \
  "_string_heap_size is set with"
if grep -qF 'pragma target string_heap_size <bytes>' \
     "$WORK/r_heap.out"; then
  ok "legacy _string_heap_size diagnostic names the pragma"
else
  bad "legacy _string_heap_size diagnostic lacks the corrective form"
fi

# ------------------------------------------------------ positive control
cat >"$WORK/ok.jal" <<'EOF'
pragma target string_heap_size 64
include 16f877
var string s
s = "hello"
forever loop
end loop
EOF
rc=$(compile "$WORK/ok.jal")
if [ "$rc" -eq 0 ] && grep -qE '^0 errors, 0 warnings$' "$(out_for "$WORK/ok.jal")"; then
  ok "pragma target string_heap_size form compiles clean"
else
  bad "positive control failed (rc=$rc)" "$(tail -4 "$(out_for "$WORK/ok.jal")")"
fi

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "reserved internal names: PASS"
  exit 0
fi
echo "reserved internal names: $fails FAILURE(S)"
exit 1

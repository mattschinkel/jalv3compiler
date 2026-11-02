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

# --------------------------------------------------------- reserved prefixes
cat >"$WORK/r_fixarg.jal" <<'EOF'
include 16f877
var byte _string_fixarg7
forever loop
end loop
EOF
expect_one_error "marshalling buffer prefix" "$WORK/r_fixarg.jal" \
  "uses the compiler-reserved prefix '_string_fixarg'"

cat >"$WORK/r_bss.jal" <<'EOF'
include 16f877
var byte _pic_bss_x
forever loop
end loop
EOF
expect_one_error "BSS block prefix" "$WORK/r_bss.jal" \
  "uses the compiler-reserved prefix '_pic_bss_'"

# --------------------------------- malformed API constant: ONE diagnostic
cat >"$WORK/r_heap.jal" <<'EOF'
var byte _string_heap_size
include 16f877
var string s
s = "hello"
forever loop
end loop
EOF
expect_one_error "malformed _string_heap_size" "$WORK/r_heap.jal" \
  "_string_heap_size must be a constant integer"
if grep -qF 'declare it as: const word _string_heap_size = <bytes>' \
     "$WORK/r_heap.out"; then
  ok "malformed _string_heap_size names the declaration to use"
else
  bad "malformed _string_heap_size diagnostic lacks the corrective form"
fi

# ------------------------------------------------------ positive control
cat >"$WORK/ok.jal" <<'EOF'
const word _string_heap_size = 64
include 16f877
var string s
s = "hello"
forever loop
end loop
EOF
rc=$(compile "$WORK/ok.jal")
if [ "$rc" -eq 0 ] && grep -qE '^0 errors, 0 warnings$' "$(out_for "$WORK/ok.jal")"; then
  ok "documented const _string_heap_size form still compiles clean"
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

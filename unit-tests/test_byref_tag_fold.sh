#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# T6 single-type byref tag fold: a byref routine called with ONE actual type
# folds its hidden tag + `_is_*` predicates to constants (the tag variable
# gets no references in the emitted image); mixed call sites keep the runtime
# tag. Observed through the generated .asm symbol table / references.
#
# Usage: unit-tests/test_byref_tag_fold.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib'

compile() {  # compile <file> <asm> <log>
  ( cd "$REPO" && "$CC" -s "$LIBS" -no-codfile -asm "$2" -hex "$WORK/o.hex" \
      "$1" ) > "$3" 2>&1
}

# --- single type: the tag folds away --------------------------------------
cat > "$WORK/single.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
procedure p(byref in out arr[]) is
   if _is_string() then
      arr[0] = "A"
   end if
end procedure
s = "ab"
p(s)
forever loop end loop
EOF
if compile "$WORK/single.jal" "$WORK/single.asm" "$WORK/single.capture" \
    && grep -q '^0 errors' "$WORK/single.capture" \
    && ! grep -q '_tag' "$WORK/single.asm"; then
  echo "ok   a single-type byref routine folds its tag away"
else
  echo "FAIL: single-type tag fold"; sed 's/^/     /' "$WORK/single.capture" | tail -8
  fails=$((fails + 1))
fi

# --- mixed types: the runtime tag stays -----------------------------------
cat > "$WORK/mixed.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
var byte b[2]
procedure p(byref in out arr[]) is
   if _is_string() then
      arr[0] = "A"
   end if
end procedure
s = "ab"
b[0] = 1
p(s)
p(b)
forever loop end loop
EOF
if compile "$WORK/mixed.jal" "$WORK/mixed.asm" "$WORK/mixed.capture" \
    && grep -q '^0 errors' "$WORK/mixed.capture" \
    && grep -q '_tag' "$WORK/mixed.asm"; then
  echo "ok   mixed-type call sites keep the runtime tag"
else
  echo "FAIL: mixed-type tag"; sed 's/^/     /' "$WORK/mixed.capture" | tail -8
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails byref-tag-fold check(s)"
  exit 1
fi
echo "PASS: byref single-type tag fold"

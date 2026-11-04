#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Public `string_*` helper calls go through the outlined strings_dynamic
# forwarder: ONE shared body in flash plus a short CALL per site, instead of
# expanding the whole builtin body (~740 words/site on a 16F1825) at every
# call site.
#
# Pin shape:
#   1. the committed 12-call fixture compiles AND fits 8192 words -- pre-fix
#      it is 10341 words and fails with "PIC image too big!";
#   2. exactly one `l_string_append` body and exactly twelve `call`s;
#   3. the marginal cost of a call site is small (< 600 words for ten extra
#      sites; one inline body is ~740);
#   4. `string_from_array` outlines the same way (one body, N calls);
#   5. the INTERNAL `_string_append` spelling stays inline (no `l_string_append`
#      label at all) -- the forwarder body uses that spelling, so outlining it
#      would call the wrapper from inside its own body.
#
# Usage: unit-tests/test_string_outline_calls.sh [path-to-jalv2]
set -u

COMPILER="${1:-./bin/beta/jalv3-x86-64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LIBS="$ROOT/demo_lib;$ROOT/third_party/jallib/lib"
FIXTURE="$ROOT/unit-tests/fixtures/string_outline_16f1825.jal"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
words=0

ok()  { pass=$((pass + 1)); echo "PASS $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1 ${2:-}"; }

[ -x "$COMPILER" ] || { echo "compiler not executable: $COMPILER" >&2; exit 2; }

code_area() { # asm-or-log text
  printf '%s\n' "$1" | sed -n 's/^Code area: \([0-9][0-9]*\) .*/\1/p' | tail -1
}

compile() { # name source path -> sets OUT, RC, ASM
  local name="$1" src="$2"
  ASM="$TMP/$name.asm"
  OUT="$("$COMPILER" -s "$LIBS" -no-codfile -asm "$ASM" "$src" 2>&1)"
  RC=$?
}

# ---------------------------------------------------------------- 1 + 2 + 3
compile fixture "$FIXTURE"
if [ "$RC" -ne 0 ] || ! printf '%s\n' "$OUT" | tail -1 \
    | grep -Eq '^0 errors, [0-9]+ warnings?$'; then
  bad "fixture compiles" "(rc=$RC)"
  printf '%s\n' "$OUT" | tail -6
else
  words="$(code_area "$OUT")"
  if [ -n "$words" ] && [ "$words" -le 3000 ]; then
    ok "fixture fits ($words words)"
  else
    bad "fixture fits" "(code area ${words:-?} words; limit 3000)"
  fi
  labels="$(grep -c '^l_string_append$' "$ASM")"
  calls="$(grep -c '^[[:space:]]*call[[:space:]][[:space:]]*l_string_append$' "$ASM")"
  if [ "$labels" -eq 1 ] && [ "$calls" -eq 12 ]; then
    ok "one shared append body, twelve calls"
  else
    bad "one shared append body, twelve calls" "(labels=$labels calls=$calls)"
  fi
fi

# marginal: two calls vs twelve (ten extra sites)
mini="$TMP/mini.jal"
{
  echo 'include 16f1825'
  echo 'pragma target clock 32_000_000'
  echo 'const word _string_heap_size = 100'
  echo 'pragma string index byte'
  echo 'include strings_dynamic'
  echo 'var string a'
  echo 'var string b'
  echo 'var bit r'
  echo 'a = "x"'
  echo 'b = "y"'
  echo 'r = string_append(a, b)'
  echo 'r = string_append(a, b)'
} > "$mini"
compile mini "$mini"
w2="$(code_area "$OUT")"
if [ "$RC" -eq 0 ] && [ -n "$w2" ]; then
  if [ "$words" -gt 0 ] && [ $((words - w2)) -lt 600 ]; then
    ok "marginal cost of ten call sites ($((words - w2)) words)"
  else
    bad "marginal cost of ten call sites" "($((words - ${w2:-0})) words; limit 600)"
  fi
fi

# ---------------------------------------------------------------- 4
fa="$TMP/from_array.jal"
{
  echo 'include 16f1825'
  echo 'pragma target clock 32_000_000'
  echo 'const word _string_heap_size = 60'
  echo 'pragma string index byte'
  echo 'include strings_dynamic'
  echo 'var string a'
  echo 'var byte arr[4] = "abcd"'
  echo 'var bit r'
  echo 'a = "x"'
  echo 'r = string_from_array(a, arr)'
  echo 'r = string_from_array(a, arr)'
  echo 'r = string_from_array(a, arr)'
  echo 'r = string_from_array(a, arr)'
} > "$fa"
compile from_array "$fa"
if [ "$RC" -eq 0 ]; then
  labels="$(grep -c '^l_string_from_array$' "$ASM")"
  calls="$(grep -c '^[[:space:]]*call[[:space:]][[:space:]]*l_string_from_array$' "$ASM")"
  if [ "$labels" -eq 1 ] && [ "$calls" -eq 4 ]; then
    ok "one shared from_array body, four calls"
  else
    bad "one shared from_array body, four calls" "(labels=$labels calls=$calls)"
  fi
else
  bad "from_array compiles" "(rc=$RC)"
fi

# ---------------------------------------------------------------- 5
# Two-operand dynamic concat is outlined too; the literal-operand and
# three-operand sites must stay inline (only the three dynamic sites call).
cat="$TMP/concat.jal"
{
  echo 'include 16f1825'
  echo 'pragma target clock 32_000_000'
  echo 'const word _string_heap_size = 100'
  echo 'pragma string index byte'
  echo 'include strings_dynamic'
  echo 'var string a'
  echo 'var string b'
  echo 'var string d1'
  echo 'var string d2'
  echo 'var string d3'
  echo 'var string e1'
  echo 'var string e2'
  echo 'var word k = 0'
  echo 'a = "abc"'
  echo 'b = "def"'
  echo 'd1 = a & b'
  echo 'd2 = a & b'
  echo 'd3 = a & b'
  echo 'e1 = a & "XY"'
  echo 'e2 = a & b & a'
  echo 'k = COUNT(d1) + COUNT(d2) + COUNT(d3) + COUNT(e1) + COUNT(e2)'
} > "$cat"
compile concat "$cat"
if [ "$RC" -eq 0 ]; then
  labels="$(grep -c '^l_string_concat$' "$ASM")"
  calls="$(grep -c '^[[:space:]]*call[[:space:]][[:space:]]*l_string_concat$' "$ASM")"
  if [ "$labels" -eq 1 ] && [ "$calls" -eq 3 ]; then
    ok "one shared concat body, three calls (literal/chain inline)"
  else
    bad "one shared concat body, three calls" "(labels=$labels calls=$calls)"
  fi
else
  bad "concat compiles" "(rc=$RC)"
fi

# ---------------------------------------------------------------- 6
internal="$TMP/internal.jal"
{
  echo 'include 16f1825'
  echo 'pragma target clock 32_000_000'
  echo 'const word _string_heap_size = 60'
  echo 'pragma string index byte'
  echo 'include strings_dynamic'
  echo 'var string a'
  echo 'var string b'
  echo 'var bit r'
  echo 'a = "x"'
  echo 'b = "y"'
  echo 'r = _string_append(a, b)'
} > "$internal"
compile internal "$internal"
if [ "$RC" -eq 0 ]; then
  labels="$(grep -c '^l_string_append$' "$ASM")"
  if [ "$labels" -eq 0 ]; then
    ok "internal _string_append stays inline (no recursion)"
  else
    bad "internal _string_append stays inline" "(labels=$labels)"
  fi
else
  bad "internal _string_append compiles" "(rc=$RC)"
fi

echo "string outline calls: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

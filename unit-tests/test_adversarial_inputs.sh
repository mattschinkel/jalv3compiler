#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Adversarial (shape-pathological) inputs must produce DIAGNOSTICS or
# successful compiles -- never a signal.
#
# WHY THIS EXISTS
#   fuzz_jal_inputs.py mutates and truncates REAL sources, so it can only
#   produce inputs that look locally like JAL.  It structurally cannot
#   generate a 50 000-deep parenthesis run, a 2 000-block procedure or a
#   1 MB array, because no mutation of a real file reaches those shapes.
#   Every defect this script pins was found by hand, not by the fuzzer:
#
#   1. unbounded parser recursion -- 50 000 nested '(' (a 100 kB file)
#      overflowed the C stack and killed the compiler with SIGSEGV
#      (value_get -> jal_parse_expr mutual recursion, jal_expr.c);
#   2. an arbitrary 1000-block cap in the PIC data-area map printed
#      "ERROR: Infinite loop detected" to stdout on a LEGAL program and
#      then exited 0 (pic.c);
#   3. the same map clamped addresses at 1024, so --clear silently failed
#      to clear data above 0x3ff on parts with more RAM than that.
#
#   Shapes 1 and 2 are legal JAL, so they are also checked for a CLEAN
#   compile below; a "fix" that merely turns the crash into a spurious
#   error would be a regression, not a repair.
#   fixes/fix_bounded_parser_recursion.md, fixes/fix_data_area_map_guards.md
#
# PASS = rc is not a signal (>=128) and, when the compile fails, an
#        "N errors, M warnings" summary is present.
#
# Usage: unit-tests/test_adversarial_inputs.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jalv2_adversarial-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0
INC="-s $REPO/demo_lib -s $REPO/third_party/jallib/lib"

# Compile one file.  $1 name, $2 file, $3 "clean" if a 0-error compile is
# required, $4 optional expected substring.
check() {
  local name=$1 file=$2 want_clean=${3:-} want=${4:-} out rc
  out=$("$CC" $INC -no-codfile "$file" 2>&1)
  rc=$?
  if [[ "$rc" -ge 128 ]]; then
    echo "FAIL $name: compiler died (rc=$rc, signal $((rc - 128)))"
    echo "$out" | tail -4 | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  if [[ "$want_clean" == "clean" && "$rc" -ne 0 ]]; then
    echo "FAIL $name: legal input must compile clean, got rc=$rc"
    echo "$out" | tail -4 | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  if [[ "$rc" -ne 0 ]] && ! grep -qE '^[0-9]+ errors, [0-9]+ warnings' <<<"$out"; then
    echo "FAIL $name: no 'N errors, M warnings' summary (compiler abort?)"
    echo "$out" | tail -4 | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  if [[ -n "$want" ]] && ! grep -q "$want" <<<"$out"; then
    echo "FAIL $name: expected '$want' in:"
    echo "$out" | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  echo "ok   $name"
}

gen() { python3 - "$@" ; }

# --- 1. expression nesting --------------------------------------------------
# 200 deep is legal JAL and must stay legal: the recursion cap is 256.
gen "$WORK/paren_deep_legal.jal" <<'PY'
import sys
n = 200
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\n'
    'a = ' + '(' * n + '1' + ')' * n + '\nforever loop\nend loop\n')
PY
check "expression nesting 200 (legal)" "$WORK/paren_deep_legal.jal" clean

gen "$WORK/paren_deep_illegal.jal" <<'PY'
import sys
n = 50000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\n'
    'a = ' + '(' * n + '1' + ')' * n + '\nforever loop\nend loop\n')
PY
check "expression nesting 50000 (recursion cap)" "$WORK/paren_deep_illegal.jal" \
  "" "nested too deeply"

# Nested casts and subscripts reach the parser by other doors; both must be
# bounded by the same counter.
gen "$WORK/cast_deep.jal" <<'PY'
import sys
n = 20000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\na = 1\n'
    'a = ' + 'byte(' * n + 'a' + ')' * n + '\nforever loop\nend loop\n')
PY
check "nested casts 20000" "$WORK/cast_deep.jal"

gen "$WORK/subscript_deep.jal" <<'PY'
import sys
n = 5000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar byte arr[8]\n'
    'arr[0] = 1\n'
    'arr[' * n + '0' + ']' * n + ' = 2\nforever loop\nend loop\n')
PY
check "nested subscripts 5000" "$WORK/subscript_deep.jal"

# --- 2. block / variable counts --------------------------------------------
# 2000 nested IF blocks is legal and exceeds the old 1000-block cap that
# made pic.c print "ERROR: Infinite loop detected" and exit 0.
gen "$WORK/blocks_2000.jal" <<'PY'
import sys
n = 2000
body = ''.join('if a == %d then\n' % i for i in range(n))
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\na = 1\n'
    + body + 'a = 2\n' + 'end if\n' * n + 'forever loop\nend loop\n')
PY
check "2000 nested if blocks" "$WORK/blocks_2000.jal" clean
if grep -q "Infinite loop detected" <<<"$( \
    "$CC" $INC -no-codfile "$WORK/blocks_2000.jal" 2>&1)"; then
  echo "FAIL 2000 nested if blocks: spurious 'Infinite loop detected'"
  fails=$((fails + 1))
fi

gen "$WORK/vars_3000.jal" <<'PY'
import sys
n = 3000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nprocedure p()\n'
    + ''.join('  var byte v%d\n  v%d = %d\n' % (i, i, i % 256)
              for i in range(n))
    + 'end procedure\np()\nforever loop\nend loop\n')
PY
check "3000 locals in one procedure" "$WORK/vars_3000.jal"

# --- 3. data-area shapes ----------------------------------------------------
gen "$WORK/array_huge.jal" <<'PY'
import sys
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar byte big[100000]\n'
    'big[0] = 1\nforever loop\nend loop\n')
PY
check "100000-byte array" "$WORK/array_huge.jal"

# --clear walks the data-area map: this is the shape that exposed the
# 1024-byte clamp.  Legal input, must stay clean.
gen "$WORK/clear_big.jal" <<'PY'
import sys
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\n'
    'var byte big[1500]\nvar byte tail[900]\n'
    'big[0] = 1\ntail[0] = 2\nforever loop\nend loop\n')
PY
out=$("$CC" $INC -no-codfile -clear "$WORK/clear_big.jal" 2>&1); rc=$?
if [[ "$rc" -ge 128 ]]; then
  echo "FAIL --clear over 1024 bytes: compiler died (rc=$rc)"
  fails=$((fails + 1))
elif [[ "$rc" -ne 0 ]]; then
  echo "FAIL --clear over 1024 bytes: rc=$rc"
  echo "$out" | tail -4 | sed 's/^/     /'
  fails=$((fails + 1))
else
  echo "ok   --clear over 1024 bytes"
fi

# --- 4. lexical shapes ------------------------------------------------------
gen "$WORK/long_ident.jal" <<'PY'
import sys
name = 'v' * 9000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\n'
    'var byte %s\n%s = 1\nforever loop\nend loop\n' % (name, name))
PY
check "9000-character identifier" "$WORK/long_ident.jal"

gen "$WORK/long_expr.jal" <<'PY'
import sys
n = 20000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\na = a'
    + '+1' * n + '\nforever loop\nend loop\n')
PY
check "20000-term expression" "$WORK/long_expr.jal"

gen "$WORK/long_string.jal" <<'PY'
import sys
s = 'x' * 100000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\n'
    'const byte msg[] = "%s"\nvar byte b\nb = msg[0]\n'
    'forever loop\nend loop\n' % s)
PY
check "100000-character string literal" "$WORK/long_string.jal"

# --- 5. degenerate files ----------------------------------------------------
: > "$WORK/empty.jal"
check "empty file" "$WORK/empty.jal"

printf '; only a comment, no source at all\n' > "$WORK/comment_only.jal"
check "comment-only file" "$WORK/comment_only.jal"

python3 - "$WORK/binary.jal" <<'PY'
import sys, os
open(sys.argv[1], 'wb').write(bytes(range(256)) * 40)
PY
check "binary garbage" "$WORK/binary.jal"

printf 'include 18f4620\nvar byte a\na = 1\n' > "$WORK/no_newline.jal"
printf 'include 18f4620' > "$WORK/truncated_include.jal"
check "truncated include" "$WORK/truncated_include.jal"

echo ""
if [[ "$fails" -ne 0 ]]; then
  echo "RESULT: FAIL ($fails adversarial-input checks failed)"
  exit 1
fi
echo "RESULT: PASS (adversarial inputs: diagnostics, no signals)"

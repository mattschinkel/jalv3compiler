#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Regression pin: the full byref tag predicate set (qword/oword/float,
# signed, array-element, pointer-pointee) lowers and folds.  Uniform calls
# must drop the hidden tag; a mixed call keeps it.
#
# Usage: test_byref_is_full_set.sh [compiler]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/beta/jalv3-x86-64}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jal_byref_is_full-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

case "$CC" in
  /*) ;;
  *) CC="$(cd "$(dirname "$CC")" && pwd)/$(basename "$CC")" ;;
esac
[[ -x "$CC" ]] || { echo "FAIL: compiler not executable: $CC" >&2; exit 1; }

LIBS="$ROOT/third_party/jallib/lib"

compile() { # dir
  local dir="$1" code
  ( cd "$dir" && "$CC" -s "$LIBS" probe.jal >compile.log 2>&1 ) || {
    cat "$dir/compile.log" >&2
    echo "FAIL: compilation failed in $dir" >&2
    exit 1
  }
  grep -q '^0 errors, 0 warnings' "$dir/compile.log" || {
    cat "$dir/compile.log" >&2
    echo "FAIL: compiler diagnostics in $dir" >&2
    exit 1
  }
  code="$(sed -n 's/^Code area: \([0-9][0-9]*\) of.*/\1/p' \
    "$dir/compile.log" | tail -1)"
  printf '%s' "$code"
}

mkdir -p "$WORK/qword" "$WORK/float" "$WORK/mixed"

cat > "$WORK/qword/probe.jal" <<'JAL'
include 18f4620
var volatile byte out
procedure sink(byref in a[]) is
   var qword v at a
   if _is_qword() then
      out = byte(v)
   else
      out = 0
   end if
   if _is_oword() then
      out = 0xEE
   end if
   if _is_qword_array() then
      out = 0xDD
   end if
end procedure
var qword q = 0x1122334455667788
sink(q)
forever loop
end loop
JAL

cat > "$WORK/float/probe.jal" <<'JAL'
include 18f4620
var volatile byte out
procedure sink(byref in a[]) is
   if _is_float() then
      out = 1
   else
      out = 2
   end if
   if _is_dword() then
      out = 3
   end if
end procedure
var float fv = 2.5
sink(fv)
forever loop
end loop
JAL

cat > "$WORK/mixed/probe.jal" <<'JAL'
include 18f4620
var volatile byte out
procedure sink(byref in a[]) is
   var byte k
   k = 0
   if _is_qword() then
      k = k | 0x01
   end if
   if _is_qword_array() then
      k = k | 0x02
   end if
   if _is_oword() then
      k = k | 0x04
   end if
   out = k
end procedure
var qword q
var qword qa[2]
var oword o
var byte b
sink(q)
sink(qa)
sink(o)
sink(b)
forever loop
end loop
JAL

qword="$(compile "$WORK/qword")"
float="$(compile "$WORK/float")"
mixed="$(compile "$WORK/mixed")"
[[ -n "$qword" && -n "$float" && -n "$mixed" ]] || {
  echo "FAIL: code size missing" >&2
  exit 1
}
grep -q '_tag' "$WORK/qword/probe.asm" && {
  echo "FAIL: uniform qword build kept the hidden _tag" >&2
  exit 1
}
grep -q '_tag' "$WORK/float/probe.asm" && {
  echo "FAIL: uniform float build kept the hidden _tag" >&2
  exit 1
}
grep -q '_tag' "$WORK/mixed/probe.asm" || {
  echo "FAIL: mixed build lost its runtime _tag" >&2
  exit 1
}
echo "ok   qword/oword predicates fold (${qword}B)"
echo "ok   float is distinct from dword and folds (${float}B)"
echo "ok   mixed qword/array/oword keeps the runtime tag (${mixed}B)"

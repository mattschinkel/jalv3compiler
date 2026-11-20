#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Regression pin: a pointer actual whose pointee is wider than a byte now
# passes through a byref byte view.  Uniform qword pointers fold the tag;
# a mixed qword/byte pointer call keeps it.
#
# Usage: test_byref_ptr_wide.sh [compiler]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/beta/jalv3-x86-64}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jal_byref_ptr_wide-XXXXXX")"
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

mkdir -p "$WORK/qword" "$WORK/mixed" "$WORK/p14"

cat > "$WORK/qword/probe.jal" <<'JAL'
include 18f4620

var volatile byte out

procedure qread(byref in a[]) is
   var qword v at a
   if _is_qword_pointer() then
      out = byte(v)
   else
      out = 0
   end if
   if byte(COUNT(a)) == 8 then
      out = out + 1
   end if
end procedure

var qword qa[1] = {0x1122334455667788}
var pointer pq as var qword

pq is qa[0]
qread(pq)
forever loop
end loop
JAL

cat > "$WORK/mixed/probe.jal" <<'JAL'
include 18f4620

var volatile byte out

procedure kind(byref in a[]) is
   if _is_qword_pointer() then
      out = out | 0x01
   end if
   if _is_byte_pointer() then
      out = out | 0x02
   end if
end procedure

var qword qa[1]
var byte ba[1]
var pointer pq as var qword
var pointer pb as var byte

pq is qa[0]
pb is ba[0]
kind(pq)
kind(pb)
forever loop
end loop
JAL

cat > "$WORK/p14/probe.jal" <<'JAL'
include 16f887

var volatile byte out

procedure qread(byref in a[]) is
   var qword v at a
   if _is_qword_pointer() then
      out = byte(v)
   else
      out = 0
   end if
end procedure

var qword qa[1] = {0x1122334455667788}
var pointer pq as var qword

pq is qa[0]
qread(pq)
forever loop
end loop
JAL

qword="$(compile "$WORK/qword")"
mixed="$(compile "$WORK/mixed")"
p14="$(compile "$WORK/p14")"
[[ -n "$qword" && -n "$mixed" && -n "$p14" ]] || {
  echo "FAIL: code size missing" >&2
  exit 1
}
grep -q '_tag' "$WORK/qword/probe.asm" && {
  echo "FAIL: uniform qword pointer build kept the hidden _tag" >&2
  exit 1
}
grep -q '_tag' "$WORK/p14/probe.asm" && {
  echo "FAIL: uniform 14-bit qword pointer build kept the hidden _tag" >&2
  exit 1
}
grep -q '_tag' "$WORK/mixed/probe.asm" || {
  echo "FAIL: mixed qword/byte pointer build lost its runtime _tag" >&2
  exit 1
}
echo "ok   qword pointer byref folds on 18F (${qword}B)"
echo "ok   qword pointer byref folds on 14-bit (${p14}B)"
echo "ok   mixed qword/byte pointer keeps the tag (${mixed}B)"

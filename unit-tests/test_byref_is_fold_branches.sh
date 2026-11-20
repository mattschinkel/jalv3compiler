#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Regression pin: constant `_is_*()` relations fold all the way to an
# unconditional/absent branch, including several predicates at different
# nesting levels and after uses of the byref formal.  A uniform call must cost
# the same as the equivalent hand-written body; mixed actual types keep the
# runtime tag and its branches.
#
# Usage: test_byref_is_fold_branches.sh [compiler]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/beta/jalv3-x86-64}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jal_byref_is_fold-XXXXXX")"
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

mkdir -p "$WORK/uniform" "$WORK/nopred" "$WORK/mixed"

cat > "$WORK/uniform/probe.jal" <<'JAL'
include 18f4620

var volatile byte out
var volatile byte out2

procedure sink(byref in a[]) is
   var byte x
   var byte y
   x = a[0]
   if _is_array() then
      y = a[1]
      if _is_string() then
         out = 0xAA
      else
         out = x
      end if
      out2 = y
   else
      out = 0xBB
   end if
end procedure

const byte m1[] = {0, 66}

out = 0x11
out2 = 0x22
sink(m1)
forever loop
end loop
JAL

cat > "$WORK/nopred/probe.jal" <<'JAL'
include 18f4620

var volatile byte out
var volatile byte out2

procedure sink(byref in a[]) is
   var byte x
   var byte y
   x = a[0]
   y = a[1]
   out = x
   out2 = y
end procedure

const byte m1[] = {0, 66}

out = 0x11
out2 = 0x22
sink(m1)
forever loop
end loop
JAL

cat > "$WORK/mixed/probe.jal" <<'JAL'
include 18f4620

var volatile byte out
var volatile byte out2

procedure sink(byref in a[]) is
   var byte x
   var byte y
   x = a[0]
   if _is_array() then
      y = a[1]
      if _is_string() then
         out = 0xAA
      else
         out = x
      end if
      out2 = y
   else
      out = 0xBB
   end if
end procedure

const byte m1[] = {0, 66}
var byte wv

out = 0x11
out2 = 0x22
sink(m1)
out = 0x33
sink(wv)
forever loop
end loop
JAL

uniform="$(compile "$WORK/uniform")"
nopred="$(compile "$WORK/nopred")"
mixed="$(compile "$WORK/mixed")"
[[ -n "$uniform" && -n "$nopred" && -n "$mixed" ]] || {
  echo "FAIL: code size missing" >&2
  exit 1
}

[[ "$uniform" == "$nopred" ]] || {
  echo "FAIL: folded nested _is_* code is ${uniform}B, hand-written body ${nopred}B" >&2
  exit 1
}
[[ "$mixed" -gt "$uniform" ]] || {
  echo "FAIL: mixed-type code ${mixed}B did not keep the runtime tag over ${uniform}B" >&2
  exit 1
}
grep -q '_tag' "$WORK/mixed/probe.asm" || {
  echo "FAIL: mixed-type build lost its runtime _tag" >&2
  exit 1
}
grep -q '_tag' "$WORK/uniform/probe.asm" && {
  echo "FAIL: uniform build still emits a _tag reference" >&2
  exit 1
}

echo "ok   nested uniform _is_* folds to the hand-written body (${uniform}B)"
echo "ok   mixed types keep the runtime tag (${mixed}B)"

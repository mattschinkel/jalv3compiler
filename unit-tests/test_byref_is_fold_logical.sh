#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Regression pin: a folded `_is_*' used as an operand of `&`/`|` with a
# runtime condition must disappear -- `_is_array() & (x == 0)' compiles to
# the same code as `x == 0'.  A mixed-actual twin keeps the runtime tag.
#
# Usage: test_byref_is_fold_logical.sh [compiler]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/beta/jalv3-x86-64}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jal_byref_is_logical-XXXXXX")"
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

mkdir -p "$WORK/fold" "$WORK/hand" "$WORK/mixed"

cat > "$WORK/fold/probe.jal" <<'JAL'
include 18f4620

var volatile byte out

procedure sink(byref in a[]) is
   var byte x
   x = a[0]
   if _is_array() & (x == 0) then
      out = 1
   else
      out = 2
   end if
end procedure

const byte m1[] = {0, 66}

out = 0x11
sink(m1)
forever loop
end loop
JAL

cat > "$WORK/hand/probe.jal" <<'JAL'
include 18f4620

var volatile byte out

procedure sink(byref in a[]) is
   var byte x
   x = a[0]
   if x == 0 then
      out = 1
   else
      out = 2
   end if
end procedure

const byte m1[] = {0, 66}

out = 0x11
sink(m1)
forever loop
end loop
JAL

cat > "$WORK/mixed/probe.jal" <<'JAL'
include 18f4620

var volatile byte out

procedure sink(byref in a[]) is
   var byte x
   x = a[0]
   if _is_array() & (x == 0) then
      out = 1
   else
      out = 2
   end if
end procedure

const byte m1[] = {0, 66}
var byte wv

out = 0x11
sink(m1)
out = 0x33
sink(wv)
forever loop
end loop
JAL

fold="$(compile "$WORK/fold")"
hand="$(compile "$WORK/hand")"
mixed="$(compile "$WORK/mixed")"
[[ -n "$fold" && -n "$hand" && -n "$mixed" ]] || {
  echo "FAIL: code size missing" >&2
  exit 1
}

[[ "$fold" == "$hand" ]] || {
  echo "FAIL: folded _is_*&cond is ${fold}B, hand-written cond is ${hand}B" >&2
  exit 1
}
[[ "$mixed" -gt "$fold" ]] || {
  echo "FAIL: mixed-type code ${mixed}B did not keep the runtime tag over ${fold}B" >&2
  exit 1
}
grep -q '_tag' "$WORK/mixed/probe.asm" || {
  echo "FAIL: mixed-type build lost its runtime _tag" >&2
  exit 1
}
grep -q '_tag' "$WORK/fold/probe.asm" && {
  echo "FAIL: uniform logical fold still emits a _tag reference" >&2
  exit 1
}

echo "ok   _is_array() & cond folds to cond (${fold}B)"
echo "ok   mixed types keep the runtime tag (${mixed}B)"

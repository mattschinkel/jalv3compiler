#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Regression pin: a const actual passed to a byref formal emits ONE lookup
# table per const, no matter how many call sites reference it.  A byref
# `_byrefargN` view has no ROM of its own; without dedup the PIC back end
# copies the master's table under every call-site alias, so two
# print_string() calls to the same 11-byte const cost 11 extra words (and a
# 32-byte const called three times costs 64).
#
# Usage: test_byref_const_lookup_dedup.sh [compiler]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/beta/jalv3-x86-64}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jal_byref_dedup-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

case "$CC" in
  /*) ;;
  *) CC="$(cd "$(dirname "$CC")" && pwd)/$(basename "$CC")" ;;
esac
[[ -x "$CC" ]] || fail "compiler not executable: $CC"

compile() { # dir libs
  local dir="$1" libs="$2" log="$1/compile.log" code
  ( cd "$dir" && "$CC" -s "$libs" probe.jal >compile.log 2>&1 ) || {
    cat "$dir/compile.log" >&2
    fail "compilation failed in $dir"
  }
  grep -q '^0 errors, 0 warnings' "$log" || {
    cat "$log" >&2
    fail "compiler diagnostics in $dir"
  }
  code="$(sed -n 's/^Code area: \([0-9][0-9]*\) of.*/\1/p' "$log" | tail -1)"
  printf '%s' "$code"
}

run_case() { # name device
  local name="$1" device="$2" base="$WORK/$1"
  local d byref_code plain_code labels aliases

  mkdir -p "$base/byref" "$base/plain"
  for d in byref plain; do
    cat >"$base/$d/probe.jal" <<JAL
include $device
include print

const byte msg[] = "Hello, JAL!"
var byte tx

print_string(tx, msg)
print_string(tx, msg)
forever loop
end loop
JAL
  done

  byref_code="$(compile "$base/byref" "$ROOT/demo_lib;$ROOT/third_party/jallib/lib")"
  plain_code="$(compile "$base/plain" "$ROOT/third_party/jallib/lib")"
  [[ -n "$byref_code" && -n "$plain_code" ]] || fail "$name: code size missing"
  [[ "$byref_code" == "$plain_code" ]] || \
    fail "$name: byref code $byref_code != plain $plain_code (const table duplicated per call site)"

  labels="$(grep -c '^l__data_msg' "$base/byref/probe.asm" || true)"
  aliases="$(grep -c 'l__data__byrefarg' "$base/byref/probe.asm" || true)"
  [[ "$labels" == "1" ]] || fail "$name: expected one l__data_msg table, found $labels"
  [[ "$aliases" == "0" ]] || fail "$name: alias data label(s) still emitted: $aliases"

  echo "ok   $name (code=$byref_code, one const table)"
}

run_case 16f877 16f877
run_case 18f4620 18f4620

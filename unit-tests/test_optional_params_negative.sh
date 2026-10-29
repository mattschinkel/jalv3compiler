#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Negative shapes for optional parameters (fixes/fix_optional_params.md):
# each snippet must FAIL to compile with the expected diagnostic.
# Usage: unit-tests/test_optional_params_negative.sh [compiler]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/jalv3-x86-64}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

check() { # <name> <expected-substring> <body...>
  local name="$1" want="$2" body="$3"
  {
    echo "include 16f877"
    printf '%s\n' "$body"
    echo "forever loop end loop"
  } > "$TMP/$name.jal"
  local out
  out="$("$CC" -s "$ROOT/third_party/jallib/lib;$ROOT/lib" "$TMP/$name.jal" 2>&1)"
  if grep -qF "$want" <<<"$out"; then
    echo "OK   $name"
  else
    echo "FAIL $name (wanted: $want)"
    grep -E "error|expected" <<<"$out" | head -3 | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

check out_default  "default is only allowed on IN parameters" \
  'procedure x(optional byte out o default 5) is end procedure'
check order        "optional parameters must come after all required" \
  'procedure x(optional byte in a default 1, byte in b) is end procedure'
check non_const    "default must be a constant expression" \
  $'var volatile byte vv = 3\nprocedure x(optional byte in a default vv) is end procedure'
check missing_dflt "default expected after optional parameter" \
  'procedure x(optional byte in a) is end procedure'
check too_few      "Too few parameters" \
  $'procedure x(byte in a, byte in b) is end procedure\nx(1)'
check pseudo_get   "optional parameters are not allowed here" \
  $'var volatile byte ud = 0\nfunction q\'get(optional byte in x default 1) return byte is\n   return 5\nend procedure'

exit "$fails"

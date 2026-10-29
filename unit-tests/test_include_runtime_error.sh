#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# include-inside-runtime-conditional (fixes/fix_include_runtime_error.md):
# error shapes must fail with the message; const/defined() shapes must be clean.
# Usage: unit-tests/test_include_runtime_error.sh [compiler]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/jalv3-x86-64}"
TMP="$(mktemp -d)"
# self-contained: the include target the test bodies reference
echo "var byte injected = 42" > "$TMP/mylib_inc.jal"
LIBS="$TMP;$ROOT/third_party/jallib/lib;$ROOT/demo_lib"
trap 'rm -rf "$TMP"' EXIT
fails=0

bad() { # <name> <body>
  local name="$1" body="$2" out
  { echo "include 16f877"; printf '%s\n' "$body"; \
    echo "forever loop end loop"; } > "$TMP/$name.jal"
  out="$("$CC" -s "$LIBS" "$TMP/$name.jal" 2>&1)"
  if grep -qF "include inside a runtime conditional or loop" <<<"$out"; then
    echo "OK   $name"
  else
    echo "FAIL $name (no error)"; grep -E "error" <<<"$out" | head -2 | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

good() { # <name> <body>
  local name="$1" body="$2" out
  { echo "include 16f877"; printf '%s\n' "$body"; \
    echo "forever loop end loop"; } > "$TMP/$name.jal"
  out="$("$CC" -s "$LIBS" "$TMP/$name.jal" 2>&1)"
  if grep -qE "^0 errors, [0-9]+ warnings" <<<"$out"; then
    echo "OK   $name (clean)"
  else
    echo "FAIL $name (expected clean)"; grep -E "error" <<<"$out" | head -2 | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

bad runtime_if    $'var volatile byte sel = 0\nif sel == 1 then\ninclude mylib_inc\nend if'
bad for_body      $'var byte i\nfor 3 using i loop\ninclude mylib_inc\nend loop'
bad while_body    $'var byte k = 3\nwhile k > 0 loop\ninclude mylib_inc\nk = k - 1\nend loop'
bad repeat_body   $'var byte k = 3\nrepeat\ninclude mylib_inc\nk = k - 1\nuntil k == 0'
bad mixed_elsif   $'var volatile byte sel = 0\nconst bit CF = false\nif CF then\n   var byte n1 = 1\nelsif sel == 2 then\ninclude mylib_inc\nend if'
bad else_runtime  $'var volatile byte sel = 0\nif sel == 3 then\n   var byte a1 = 1\nelse\ninclude mylib_inc\nend if'
bad nested_rt     $'const bit CT = true\nif CT then\n   var volatile byte s2 = 0\n   if s2 == 1 then\n   include mylib_inc\n   end if\nend if'
good defined_gd   $'if defined(portb) then\ninclude mylib_inc\nend if'
good const_true   $'const bit CT = true\nif CT then\ninclude mylib_inc\nend if'
good const_false  $'const bit CF = false\nif CF then\ninclude mylib_inc\nend if'
good else_const   $'const bit CF = false\nif CF then\n   var byte n1 = 1\nelse\ninclude mylib_inc\nend if'

exit "$fails"

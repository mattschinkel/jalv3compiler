#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the demo_lib/print.jal shadow's opt-in guard
# (fix_print_jal_shadow): _PRINT_FLOAT_EXACT routes print_float
# through the 128-bit exact renderer, which small parts cannot hold.
# The opt-in must FAIL those compiles with the clean named memory error
# (never a silent fallback), while the DEFAULT compact body and the
# dead-procedure case must compile clean everywhere the stock printer
# fits.
#
# Usage: unit-tests/test_print_shadow_negative.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

run() { # file -> full compiler output
  "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$1" 2>&1
}

expect_error() { # name file substring
  local name=$1 file=$2 want=$3 out
  out=$(run "$file")
  if echo "$out" | grep -q "$want"; then
    if echo "$out" | grep -qE "[1-9][0-9]* errors"; then
      echo "ok   $name"
    else
      echo "FAIL $name: matched '$want' but no error count > 0:"
      echo "$out" | tail -3 | sed 's/^/     /'
      fails=$((fails + 1))
    fi
  else
    echo "FAIL $name: expected '$want' in:"
    echo "$out" | grep -v "^generating\|^writing\|tokens\|^jal " | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

expect_clean() { # name file
  local name=$1 file=$2 out
  out=$(run "$file")
  if echo "$out" | grep -q "0 errors, 0 warnings"; then
    echo "ok   $name"
  else
    echo "FAIL $name: expected 0 errors, 0 warnings, got:"
    echo "$out" | grep -E "error|warning" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# --- 1: opt-in + a print_float call on 16F877 (368 B RAM) -> the
# -------- exact renderer's own locals must fail the compile cleanly,
# -------- naming the renderer scratch (proves WHICH body failed).
cat > "$WORK/e1.jal" << 'EOF'
include 16f877
pragma target clock 20_000_000
const _PRINT_FLOAT_EXACT = TRUE
include print
var byte sink
procedure cap'put(byte in x) is sink = x end procedure
print_float(cap, 1.5)
EOF
expect_error "exact_optin_16f877_fails_named" "$WORK/e1.jal" "_fs_itmp"

# --- 2: opt-in declared but print_float never called -> dead procedure
# -------- costs nothing (local buffer, dead-code eliminated).
cat > "$WORK/e2.jal" << 'EOF'
include 16f877
pragma target clock 20_000_000
const _PRINT_FLOAT_EXACT = TRUE
include print
var byte sink
procedure cap'put(byte in x) is sink = x end procedure
print_byte_dec(cap, 42)
EOF
expect_clean "exact_optin_dead_proc_free" "$WORK/e2.jal"

# --- 3: DEFAULT body + print_float call on 16F877 -> compact fits.
cat > "$WORK/e3.jal" << 'EOF'
include 16f877
pragma target clock 20_000_000
include print
var byte sink
procedure cap'put(byte in x) is sink = x end procedure
print_float(cap, 1.5)
EOF
expect_clean "default_compact_fits_16f877" "$WORK/e3.jal"

# --- 4: DEFAULT body + print_float call on 16F1825 (the corpus sample
# -------- part whose HEX is the only intended manifest drift).
cat > "$WORK/e4.jal" << 'EOF'
include 16f1825
pragma target clock 32_000_000
include print
var byte sink
procedure cap'put(byte in x) is sink = x end procedure
print_float(cap, 1.5)
EOF
expect_clean "default_compact_fits_16f1825" "$WORK/e4.jal"

if [[ $fails -eq 0 ]]; then
  echo "all print-shadow pins passed"
  exit 0
fi
echo "$fails print-shadow pin(s) FAILED"
exit 1

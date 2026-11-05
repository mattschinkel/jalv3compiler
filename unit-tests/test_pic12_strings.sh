#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins the PIC_12 string gate (fixes/fix_pic12_string_support.md).
#
# A 12-bit core has no FSR/INDF and no PCLATH, so the string runtime cannot
# be built for it.  Before this gate, `var string s; s = "xy"` on a 12F508
# produced 90 diagnostics -- one "_irp not found" per instruction of the heap
# initialiser, plus "_pclath not found" -- because the error surfaced at
# resolve time, after the whole string runtime had been emitted.
#
# The gate reports ONCE, at the string construct, and keeps codegen from
# running (the parse-time error makes the compiler skip the p-code dump), so
# the user sees one line and gets no HEX.
#
# Boundary that must NOT move: a string LITERAL used as data (the device
# files themselves carry `const byte PICTYPE[] = "12F508"`, and jallib uses
# the same shape everywhere) is compiled as a const byte array and stays
# legal -- the runtime, not the literal, is what a 12-bit core cannot host.
# COUNT() of a literal folds at compile time and stays legal for the same
# reason.
#
# Usage: unit-tests/test_pic12_strings.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
INC="-s $REPO/demo_lib -s $REPO/third_party/jallib/lib"
MSG="strings are not supported on the 12-bit core"
fails=0

run() { # file -> compiler output; exit status kept in $rc
  out=$("$CC" $INC "$1" 2>&1)
  rc=$?
}

expect_gate() { # name file
  local name=$1 file=$2 hits
  run "$file"
  hits=$(printf '%s\n' "$out" | grep -c "$MSG")
  if [[ "$rc" -eq 0 ]]; then
    echo "FAIL $name: expected a non-zero exit, got 0"
    printf '%s\n' "$out" | tail -5 | sed 's/^/     /'
    fails=$((fails + 1))
  elif [[ "$hits" -ne 1 ]]; then
    echo "FAIL $name: expected exactly ONE '$MSG', got $hits:"
    printf '%s\n' "$out" | grep -E "error|warning" | head -8 | sed 's/^/     /'
    fails=$((fails + 1))
  elif printf '%s\n' "$out" | grep -qE "_irp' not found|_pclath' not found"; then
    echo "FAIL $name: resolver cascade leaked through the gate:"
    printf '%s\n' "$out" | grep -E "not found" | head -4 | sed 's/^/     /'
    fails=$((fails + 1))
  elif [[ -f "${file%.jal}.hex" ]]; then
    echo "FAIL $name: a gated compile must not write a HEX"
    fails=$((fails + 1))
  else
    echo "ok   $name"
  fi
}

expect_no_gate() { # name file [must-contain]
  local name=$1 file=$2 want=${3:-} hits
  run "$file"
  hits=$(printf '%s\n' "$out" | grep -c "$MSG")
  if [[ "$hits" -ne 0 ]]; then
    echo "FAIL $name: 12-bit gate fired on a non-12-bit target:"
    printf '%s\n' "$out" | grep "$MSG" | sed 's/^/     /'
    fails=$((fails + 1))
  elif [[ -n "$want" ]] && ! printf '%s\n' "$out" | grep -q "$want"; then
    echo "FAIL $name: expected '$want' in:"
    printf '%s\n' "$out" | grep -E "error" | head -5 | sed 's/^/     /'
    fails=$((fails + 1))
  else
    echo "ok   $name"
  fi
}

expect_clean() { # name file
  local name=$1 file=$2
  run "$file"
  if [[ "$rc" -eq 0 ]] && printf '%s\n' "$out" | grep -q "0 errors, 0 warnings"; then
    echo "ok   $name"
  else
    echo "FAIL $name: expected 0 errors, 0 warnings:"
    printf '%s\n' "$out" | grep -E "error|warning" | head -6 | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

pic12_head() { # file
  cat > "$1" << 'EOF'
include 12f508
pragma target clock 4_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target WDT DISABLED
pragma target MCLR EXTERNAL
EOF
}

# --- 1: the dynamic string TYPE is refused ---------------------------
pic12_head "$WORK/dyn.jal"
cat >> "$WORK/dyn.jal" << 'EOF'
var string s
forever loop
  s = "ab"
end loop
EOF
expect_gate "var string on 12f508" "$WORK/dyn.jal"

# --- 2: the fixed/const string type is refused ----------------------
pic12_head "$WORK/fixed.jal"
cat >> "$WORK/fixed.jal" << 'EOF'
const string tag[4] = "ab"
forever loop
end loop
EOF
expect_gate "const string[N] on 12f508" "$WORK/fixed.jal"

# --- 3: a string comparison is refused ------------------------------
pic12_head "$WORK/rel.jal"
cat >> "$WORK/rel.jal" << 'EOF'
var byte b
b = 0
if "ab" == "ab" then
  b = 1
end if
forever loop
  b = b + 1
end loop
EOF
expect_gate "string relation on 12f508" "$WORK/rel.jal"

# --- 4: including the strings library is refused --------------------
pic12_head "$WORK/lib.jal"
cat >> "$WORK/lib.jal" << 'EOF'
pragma target string_heap_size 16
include strings_dynamic
forever loop
end loop
EOF
expect_gate "include strings_dynamic on 12f508" "$WORK/lib.jal"

# --- 5..7: every other core is untouched ----------------------------
cat > "$WORK/14h.jal" << 'EOF'
include 16f1825
pragma target clock 32_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target PLLEN DISABLED
pragma target CLKOUTEN DISABLED
pragma target WDT DISABLED
pragma target MCLR EXTERNAL
pragma target LVP DISABLED
pragma target DEBUG DISABLED
pragma target BROWNOUT DISABLED
pragma target FCMEN DISABLED
pragma target IESO DISABLED
pragma string index byte
include strings_dynamic
var string s
procedure demo() is
   s = "ab"
end procedure
demo()
forever loop
end loop
EOF
expect_no_gate "16f1825 (14-bit hybrid) still accepts strings" "$WORK/14h.jal"

cat > "$WORK/14.jal" << 'EOF'
include 16f887
pragma target clock 8_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target WDT CONTROL
pragma target DEBUG DISABLED
pragma target BROWNOUT DISABLED
pragma target FCMEN DISABLED
pragma target IESO DISABLED
pragma target LVP DISABLED
pragma target MCLR EXTERNAL
WDTCON_SWDTEN = OFF
OSCCON_SCS = 0
enable_digital_io()
pragma target string_heap_size 16
pragma string index byte
include strings_dynamic
var string s
procedure demo() is
   s = "ab"
end procedure
demo()
forever loop
end loop
EOF
expect_no_gate "16f887 (14-bit) still accepts strings" "$WORK/14.jal"

cat > "$WORK/16.jal" << 'EOF'
include 18f4620
pragma target clock 20_000_000
pragma target OSC HS
pragma target WDT DISABLED
pragma target XINST DISABLED
pragma target DEBUG DISABLED
pragma target LVP DISABLED
pragma target MCLR EXTERNAL
pragma target string_heap_size 16
pragma string index byte
include strings_dynamic
var string s
procedure demo() is
   s = "ab"
end procedure
demo()
forever loop
end loop
EOF
expect_no_gate "18f4620 (16-bit) still accepts strings" "$WORK/16.jal"

# --- 8: a literal used as DATA stays legal on a 12-bit core ---------
# (the device files ship this shape themselves: PICTYPE[] = "12F508")
pic12_head "$WORK/data.jal"
cat >> "$WORK/data.jal" << 'EOF'
const byte ptype[] = "12F508"
var byte led
forever loop
  led = ptype[5]
end loop
EOF
expect_clean "const byte[] = literal still compiles on 12f508" "$WORK/data.jal"

# --- 9: COUNT() of a literal folds at compile time and stays legal --
pic12_head "$WORK/count.jal"
cat >> "$WORK/count.jal" << 'EOF'
var byte n
n = COUNT("hello")
forever loop
  n = n + 1
end loop
EOF
expect_clean "COUNT(literal) still folds on 12f508" "$WORK/count.jal"

echo ""
if [[ "$fails" -gt 0 ]]; then
  echo "RESULT: FAIL ($fails case(s))"
  exit 1
fi
echo "RESULT: PASS"
exit 0

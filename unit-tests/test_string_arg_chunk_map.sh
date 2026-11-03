#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Pins for open-array formals fed MULTI-BANK actuals
# (fixes/fix_openarray_formal_chunk_map.md,
#  docs/investigations/runtime_chunk_arguments_design_20261102.md).
#
# The callee body is emitted ONCE, so its subscripts used to key off the
# FORMAL's chunk map; `pic_var_chunk_map_propagate` copies the actual's map
# onto the formal, guarded by "the formal has no map yet", so a formal serving
# two DIFFERENT multi-bank actuals walked only the FIRST one's chunks.
#
# User-visible face (Rob's 16F1825 program, 2026-11-02): a `_string_heap_size`
# larger than a RAM bank makes the compiler's `_strargN` string-argument
# buffer multi-bank, so
#     print_string(serial, my_string)
#     print_string(serial, svalue)        -- a second heap-string argument
# printed the FIRST string's text (`My stri\t\t` / `My string\0`).
#
# The fix hands the ACTIVE layout to the callee at run time: the call site
# fills `_chunkmap` and points `_strarg_map_ptr` at it, and the callee's
# dynamic subscript walks it (see pic_fsr_chunk_walk_emit_globals).  So the
# multi-bank configuration now COMPILES AND RUNS; the runtime proof is
# sims/gpsim/16f1825_gpsim_string_heap_over_bank.jal, and this script pins the
# compile-side contract:
#   * heap over one bank + two heap-string argument sites -> clean compile AND
#     the runtime map is wired into the output (`_chunkmap`, walk);
#   * heap that fits one bank -> clean compile with no map at all;
#   * one string-argument site with a multi-bank heap -> clean;
#   * several big USER buffers fed to one formal (pristine jallib sd_card /
#     fat32 do this) -> still clean: user arrays keep the baked layout.
#
# Usage: unit-tests/test_string_arg_chunk_map.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jal_chunkmap-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

# takes HEAP two heap strings, both printed through print_string
write_prog() { # file heap
  cat > "$1" <<JAL
include 16f1825
pragma target clock 16_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target PLLEN DISABLED
pragma target WDT DISABLED
pragma target PWRTE ENABLED
pragma target BROWNOUT DISABLED
pragma target FCMEN DISABLED
pragma target IESO DISABLED
pragma target LVP DISABLED
pragma target MCLR EXTERNAL
enable_digital_io()
const serial_hw_baudrate = 115200
alias pin_RX_direction is pin_c5_direction
alias pin_TX_direction is pin_c4_direction
include serial_hardware
serial_hw_init()
alias serial is serial_hw_data
include print
include delay
const word _string_heap_size = $2
var string a = "My string"
var string b = "My string1"
forever loop
   print_string(serial, a)
   print_crlf(serial)
   print_string(serial, b)
   print_crlf(serial)
   delay_1s(1)
end loop
JAL
}

run_cc() { # out log file
  "$CC" -s "$REPO/demo_lib;$REPO/third_party/jallib/lib" -no-codfile -no-fuse \
    -hex "$WORK/out.hex" -asm "$WORK/out.asm" "$3" >"$1" 2>&1
  echo $? >"$2"
}

# 1. heap over one bank (100 B) + two string-argument sites -> clean compile
#    and the runtime chunk map is wired into the generated code.
write_prog "$WORK/big.jal" 100
run_cc "$WORK/big.txt" "$WORK/big.rc" "$WORK/big.jal"
rc=$(cat "$WORK/big.rc")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/big.txt" \
   && grep -q "_chunkmap" "$WORK/out.asm" \
   && grep -q "_strarg_map_ptr" "$WORK/out.asm"; then
  ok "multi-bank argument buffers compile, with the runtime chunk map wired"
else
  bad "multi-bank argument buffers compile, with the runtime chunk map wired" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/big.txt" | head -2)"
fi

# 2. heap that fits one bank -> clean compile (the supported configuration).
write_prog "$WORK/small.jal" 48
run_cc "$WORK/small.txt" "$WORK/small.rc" "$WORK/small.jal"
rc=$(cat "$WORK/small.rc")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/small.txt"; then
  ok "single-bank argument buffer compiles clean"
else
  bad "single-bank argument buffer compiles clean" "rc=$rc" \
      "$(grep -aE 'error|errors,' "$WORK/small.txt" | head -2)"
fi

# 2b. ONE string-argument site is legal even with a multi-bank heap: only a
#     SECOND, different actual for the same formal is ambiguous.  The first
#     version of the guard refused this case too -- it could not tell "another
#     actual's map is installed" from "this same actual was propagated in an
#     earlier placement round"; pic_variable_chunk_map_t.owner now records it.
cat > "$WORK/one.jal" <<'JAL'
include 16f1825
pragma target clock 16_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target PLLEN DISABLED
pragma target WDT DISABLED
pragma target PWRTE ENABLED
pragma target BROWNOUT DISABLED
pragma target FCMEN DISABLED
pragma target IESO DISABLED
pragma target LVP DISABLED
pragma target MCLR EXTERNAL
enable_digital_io()
const serial_hw_baudrate = 115200
alias pin_RX_direction is pin_c5_direction
alias pin_TX_direction is pin_c4_direction
include serial_hardware
serial_hw_init()
alias serial is serial_hw_data
include print
const word _string_heap_size = 100
var string a = "My string"
forever loop
   print_string(serial, a)
   print_crlf(serial)
end loop
JAL
run_cc "$WORK/one.txt" "$WORK/one.rc" "$WORK/one.jal"
rc=$(cat "$WORK/one.rc")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/one.txt"; then
  ok "a single string-argument site compiles clean even with a multi-bank heap"
else
  bad "a single string-argument site compiles clean even with a multi-bank heap" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/one.txt" | head -2)"
fi

# 3. user buffers: several big arrays to one formal is the SAME latent
#    ambiguity but pre-existing and pervasive (sd_card/fat32); the narrowed
#    diagnostic must not fire, or the corpus turns red.
cat > "$WORK/userbuf.jal" <<'JAL'
include 16f1825
pragma target clock 16_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target PLLEN DISABLED
pragma target WDT DISABLED
pragma target PWRTE ENABLED
pragma target BROWNOUT DISABLED
pragma target FCMEN DISABLED
pragma target IESO DISABLED
pragma target LVP DISABLED
pragma target MCLR EXTERNAL
enable_digital_io()
var byte buf1[100]
var byte buf2[100]
var byte sink

procedure take(byte out dev, byte in data[]) is
   var word len = count(data)
   var byte i
   for len using i loop
      dev = data[i]
   end loop
end procedure

forever loop
   take(sink, buf1)
   take(sink, buf2)
end loop
JAL
run_cc "$WORK/user.txt" "$WORK/user.rc" "$WORK/userbuf.jal"
nerr=$(grep -c "open-array formal" "$WORK/user.txt")
rc=$(cat "$WORK/user.rc")
if [ "$rc" -eq 0 ] && [ "$nerr" -eq 0 ]; then
  ok "two user buffers to one formal still compile (out of scope, corpus)"
else
  bad "two user buffers to one formal still compile (out of scope, corpus)" \
      "rc=$rc refusals=$nerr" \
      "$(grep -aE 'error|errors,' "$WORK/user.txt" | head -2)"
fi

if [ "$fails" -ne 0 ]; then
  echo "RESULT: FAIL ($fails)"
  exit 1
fi
echo "RESULT: PASS"
exit 0

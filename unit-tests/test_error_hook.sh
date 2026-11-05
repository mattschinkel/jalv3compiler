#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Pins for the `_error_hook` runtime error handler (fixes/fix_error_hook.md).
#
# The compiler detects the hook by NAME at each raise site: define
# `procedure _error_hook()` and it is called there, with `_error_source` and
# the domain cell (`_string_error`) set and cleared when it returns.  Without
# a hook nothing changes (the historical poll behaviour), so the corpus must
# stay byte-identical; that is the L2 manifest's job.  This script pins the
# compile-side contract:
#   * no hook            -> clean compile, and no `_error_source` cell/raise
#                           dispatch is emitted (zero cost);
#   * hook, no params    -> clean compile, the dispatch IS emitted;
#   * hook with a param  -> ONE clear diagnostic naming the required shape;
#   * hook defined late  -> ONE warning naming the missed sites.
#
# Usage: unit-tests/test_error_hook.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jal_errorhook-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

run_cc() { # file out.txt rc.txt
  "$CC" -s "$REPO/demo_lib;$REPO/third_party/jallib/lib" -no-codfile \
    -hex "$WORK/out.hex" -asm "$WORK/out.asm" "$1" >"$2" 2>&1
  echo $? >"$3"
}

prog_head() {
  cat <<JAL
include 16f1825
pragma target clock 16_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target WDT DISABLED
enable_digital_io()
var volatile byte seen
pragma target string_heap_size 100
JAL
}

# 1. no hook: clean compile, no dispatch, no _error_source cell
{ prog_head; cat <<'JAL'
var string sa = "AAAA"
var string sb = "BBBB"
var string sc
forever loop
   sc = sc & sa & sb
end loop
JAL
} > "$WORK/nohook.jal"
run_cc "$WORK/nohook.jal" "$WORK/nohook.txt" "$WORK/nohook.rc"
rc=$(cat "$WORK/nohook.rc")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/nohook.txt" \
   && ! grep -q "_error_source" "$WORK/out.asm"; then
  ok "no _error_hook: clean compile, no dispatch emitted"
else
  bad "no _error_hook: clean compile, no dispatch emitted" "rc=$rc" \
      "$(grep -aE 'error|errors,' "$WORK/nohook.txt" | head -2)"
fi

# 2. hook with no parameters: clean compile, dispatch present
{ prog_head; cat <<'JAL'
procedure _error_hook() is
   if _error_source == _ERROR_SOURCE_STRING then
      seen = 1
   end if
end procedure
var string sa = "AAAA"
var string sb = "BBBB"
var string sc
forever loop
   sc = sc & sa & sb
end loop
JAL
} > "$WORK/hook.jal"
run_cc "$WORK/hook.jal" "$WORK/hook.txt" "$WORK/hook.rc"
rc=$(cat "$WORK/hook.rc")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/hook.txt" \
   && grep -q "_error_source" "$WORK/out.asm" \
   && grep -q "call     l__error_hook" "$WORK/out.asm"; then
  ok "hook with no parameters: clean compile and the dispatch is emitted"
else
  bad "hook with no parameters: clean compile and the dispatch is emitted" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/hook.txt" | head -2)"
fi

# 3. hook with a parameter: exactly one diagnostic naming the shape
{ prog_head; cat <<'JAL'
procedure _error_hook(byte in code) is
   seen = code
end procedure
var string a = "AAAA"
forever loop
   a = a & "B"
end loop
JAL
} > "$WORK/badsig.jal"
run_cc "$WORK/badsig.jal" "$WORK/badsig.txt" "$WORK/badsig.rc"
n=$(grep -c "must be declared with no parameters" "$WORK/badsig.txt")
if [ "$n" -eq 1 ]; then
  ok "a parameterised _error_hook is refused once with the required shape"
else
  bad "a parameterised _error_hook is refused once with the required shape" \
      "diagnostics=$n" "$(grep -a 'error_hook' "$WORK/badsig.txt" | head -3)"
fi

# 4. hook defined after string code: one warning, naming the missed sites
{ prog_head; cat <<'JAL'
var string sa = "AAAA"
var string sb = "BBBB"
var string sc
sc = sc & sa & sb
procedure _error_hook() is
   seen = 1
end procedure
forever loop
end loop
JAL
} > "$WORK/late.jal"
run_cc "$WORK/late.jal" "$WORK/late.txt" "$WORK/late.rc"
# the 8th line of late.jal is the first string declaration (prog_head is 7
# lines), i.e. where the first raise site that cannot dispatch is emitted
n=$(grep -c "a runtime error may occur before _error_hook was defined" "$WORK/late.txt")
h=$(grep -c "late\.jal:8: warning: .*define _error_hook above line 8\." "$WORK/late.txt")
if [ "$n" -eq 1 ] && [ "$h" -eq 1 ]; then
  ok "a late _error_hook warns once, on the first site it cannot cover"
else
  bad "a late _error_hook warns once, on the first site it cannot cover" \
      "warnings=$n at_line_8=$h" \
      "$(grep -a 'error_hook' "$WORK/late.txt" | head -3)"
fi

if [ "$fails" -ne 0 ]; then
  echo "RESULT: FAIL ($fails)"
  exit 1
fi
echo "RESULT: PASS"
exit 0

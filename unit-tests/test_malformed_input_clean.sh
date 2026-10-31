#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Malformed input must produce DIAGNOSTICS, never a signal.
#
# Every other gate proves the compiler handles WELL-FORMED input. This one
# pins the error-recovery path: a half-built expression used to reach the
# p-code builder with a NULL operand and trip
#   pf_expr.c: pfile_cmd_op_add: Assertion `val1 != VALUE_NONE' failed
# i.e. a one-line typo aborted the process (SIGABRT, exit 134) instead of
# reporting the syntax error. Two independent entries into that path are
# pinned here (fixes/fix_malformed_input_clean.md):
#   1. `pragma target OSC` with an unknown mode left the bad token in the
#      stream, so the pragma line was re-parsed as a phantom `case` statement;
#   2. `case of` with no expression builds the case compare with no left
#      operand at all.
#
# PASS = non-zero exit carrying an "N errors" summary, and rc is NOT a
# signal (134 abort / 139 segv). A clean compile (rc 0) is ALSO a failure
# here: accepting a malformed program is worse than crashing on it.
#
# Usage: unit-tests/test_malformed_input_clean.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jalv2_malformed-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0

check_fails_clean() { # name file [expected-substring]
  local name=$1 file=$2 want=${3:-} out rc
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" \
      -no-codfile "$file" 2>&1)
  rc=$?
  if [[ "$rc" -eq 134 || "$rc" -eq 139 || "$rc" -ge 128 ]]; then
    echo "FAIL $name: compiler died (rc=$rc, signal $((rc - 128)))"
    echo "$out" | tail -4 | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  if [[ "$rc" -eq 0 ]]; then
    echo "FAIL $name: malformed input compiled CLEAN (rc=0)"
    fails=$((fails + 1))
    return
  fi
  if ! grep -qE '^[0-9]+ errors, [0-9]+ warnings' <<<"$out"; then
    echo "FAIL $name: no 'N errors, M warnings' summary (compiler abort?)"
    echo "$out" | tail -4 | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  if [[ -n "$want" ]] && ! grep -q "$want" <<<"$out"; then
    echo "FAIL $name: expected '$want' in:"
    echo "$out" | sed 's/^/     /'
    fails=$((fails + 1))
    return
  fi
  echo "ok   $name"
}

# --- 1. the fuzzer reproducer: bad `pragma target OSC` mode -----------------
# The mode keyword is left unconsumed by the pragma handler, so the line was
# re-parsed as a statement; the CASE machinery then built a compare with a
# NULL left operand.
printf 'pragma target OSC case hs\n' > "$WORK/osc_bad_mode.jal"
check_fails_clean "bad pragma target OSC mode" "$WORK/osc_bad_mode.jal" \
  "unknown oscillator mode"

# Same shape on a real device that routes through the OSC-mode keyword table
# (PIC32; classic cores resolve `osc` as a FUSE group and take the branch
# pinned just below). This is the original fuzz find.
cat > "$WORK/osc_bad_mode_dev.jal" << 'EOF'
include 32mx270f256b
pragma target clock 40_000_000
pragma target OSC case hs
var byte x
x = 1
EOF
check_fails_clean "bad pragma target OSC on a PIC32 device" \
  "$WORK/osc_bad_mode_dev.jal" "unknown oscillator mode"

# Classic cores resolve `osc` through the fuse-group table instead.
cat > "$WORK/osc_bad_mode_classic.jal" << 'EOF'
include 16f877
pragma target clock 20_000_000
pragma target OSC case hs
var byte x
x = 1
forever loop
end loop
EOF
check_fails_clean "bad pragma target OSC on a classic device" \
  "$WORK/osc_bad_mode_classic.jal" "unknown pragma target: osc"

# --- 2. `case` with no controlling expression -------------------------------
# Jal reaches pfile_cmd_op_add(OPERATOR_EQ, dst, val1=NULL, val2=<entry>):
# the missing-operand guard must report and substitute a value, not assert.
cat > "$WORK/case_no_expr.jal" << 'EOF'
include 16f877
var byte x
x = 0
case of
  1 : x = 1
end
forever loop
end loop
EOF
check_fails_clean "case with no expression" "$WORK/case_no_expr.jal"

# --- 3. truncated source (fuzzer's truncation mutation) ---------------------
printf 'include 16f877\nvar byte x\nif x == 1 then\n' > "$WORK/truncated.jal"
check_fails_clean "truncated source" "$WORK/truncated.jal"

# --- 4. unterminated string ------------------------------------------------
printf 'include 16f877\nvar byte x\nx = "abc\n' > "$WORK/open_string.jal"
check_fails_clean "unterminated string" "$WORK/open_string.jal"

echo ""
if [[ "$fails" -ne 0 ]]; then
  echo "RESULT: FAIL ($fails malformed-input checks failed)"
  exit 1
fi
echo "RESULT: PASS (malformed input fails clean, no signals)"
exit 0

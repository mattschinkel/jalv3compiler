#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Driver-level contract that the corpus/sim gates cannot see: the compiler's
# command line must terminate, and its HEX output must be installed
# atomically and never clobber a target it does not own.
#
# Recurring defect family (no automated coverage before this pin):
#   fixes/fix_cli_help_infinite_loop.md        -- --help/--version spun forever
#   fixes/fix_atomic_hex_rename_install.md     -- failed compile truncated a good HEX
#   fixes/fix_hex_output_dev_null_guard.md     -- rename/remove destroyed /dev/null
#   fixes/fix_empty_hex_error_count.md         -- empty output still said 0 errors
#
# The non-regular-target case uses a FIFO, NOT /dev/null: a regression in the
# guard would replace/unlink the device node, and a test that causes the
# incident it guards against is unacceptable.  A FIFO is non-regular, is
# written and discarded by a background reader, and is safe to lose.
#
# Usage: unit-tests/test_cli_hex_contract.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC="$REPO/demo_lib;$REPO/third_party/jallib/lib"

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

# Any leftover atomic temp is a bug in every outcome below.
no_temps() { # name
  local found
  found=$(find "$WORK" -maxdepth 1 -type f \
            \( -name '*.part' -o -name '*.tmp*' \) -print)
  if [ -n "$found" ]; then
    bad "$1: atomic temp file(s) left behind" $found
  else
    ok "$1: no .part/.tmp leftovers"
  fi
}

# ------------------------------------------------------------------ CLI
timeout 10 "$CC" --help >"$WORK/help" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && grep -q '^Usage:' "$WORK/help" \
   && [ "$(grep -c '^Usage:' "$WORK/help")" -eq 1 ] \
   && [ "$(wc -l <"$WORK/help")" -lt 2000 ]; then
  ok "--help terminates once with usage text"
else
  bad "--help" "rc=$rc lines=$(wc -l <"$WORK/help") usage_count=$(grep -c '^Usage:' "$WORK/help")"
fi

timeout 10 "$CC" --version >"$WORK/version" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && [ "$(wc -l <"$WORK/version")" -le 2 ] \
   && head -1 "$WORK/version" | grep -q '^jal '; then
  ok "--version terminates with one banner line"
else
  bad "--version" "rc=$rc lines=$(wc -l <"$WORK/version")"
fi

timeout 10 "$CC" --not-an-option >"$WORK/bogus" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q 'unknown option: --not-an-option' "$WORK/bogus"; then
  ok "unknown option is rejected without spinning"
else
  bad "unknown option" "rc=$rc" "$(tail -3 "$WORK/bogus")"
fi

timeout 10 "$CC" >"$WORK/noargs" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q 'no source file' "$WORK/noargs"; then
  ok "no-argument invocation fails with a diagnostic"
else
  bad "no arguments" "rc=$rc" "$(tail -3 "$WORK/noargs")"
fi

# ---------------------------------------------------------------- fixtures
printf 'include 16f877\nvar byte x\nx = 1\nforever loop\nend loop\n' \
  >"$WORK/ok.jal"
printf 'include 16f877\nvar byte x\nx = nosuchvar\nforever loop\nend loop\n' \
  >"$WORK/bad.jal"

compile() { # logfile hex jal
  local log=$1 hex=$2 jal=$3
  timeout 60 "$CC" -s "$INC" -no-codfile -hex "$hex" "$jal" \
    >"$log" 2>&1
  return $?
}

# ------------------------------------------------------ successful install
compile "$WORK/success.log" "$WORK/out.hex" "$WORK/ok.jal"
rc=$?
if [ "$rc" -eq 0 ] && [ -s "$WORK/out.hex" ] \
   && head -1 "$WORK/out.hex" | grep -q '^:'; then
  ok "successful compile installs a non-empty Intel HEX"
else
  bad "successful install" "rc=$rc" "$(tail -4 "$WORK/success.log")"
fi
no_temps "successful install"

# -------------------------------------------- failure preserves a good HEX
cp "$WORK/out.hex" "$WORK/good.hex"
compile "$WORK/failure.log" "$WORK/out.hex" "$WORK/bad.jal"
rc=$?
if [ "$rc" -ne 0 ] && grep -q 'errors,' "$WORK/failure.log"; then
  ok "failing compile exits nonzero with an error summary"
else
  bad "failing compile" "rc=$rc" "$(tail -4 "$WORK/failure.log")"
fi
if cmp -s "$WORK/out.hex" "$WORK/good.hex"; then
  ok "failing compile leaves the previous good HEX byte-identical"
else
  bad "failed compile overwrote the previous good HEX"
fi
no_temps "failed compile (existing target)"

# --------------------------------------- failure creates nothing when absent
compile "$WORK/new.log" "$WORK/new.hex" "$WORK/bad.jal" >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 0 ] && [ ! -e "$WORK/new.hex" ]; then
  ok "failing compile with no previous HEX leaves no HEX"
else
  bad "failing compile created/kept $WORK/new.hex" "rc=$rc"
fi
no_temps "failed compile (no target)"

# --------------------------------------------------- unusable output directory
compile "$WORK/nodir.log" "$WORK/no_such_dir/out.hex" "$WORK/ok.jal"
rc=$?
if [ "$rc" -ne 0 ] \
   && grep -Eq 'could not install hex output|Cannot open' "$WORK/nodir.log" \
   && [ ! -d "$WORK/no_such_dir" ]; then
  ok "unusable output directory fails cleanly without creating it"
else
  bad "unusable output directory" "rc=$rc" "$(tail -4 "$WORK/nodir.log")"
fi
no_temps "unusable output directory"

# ------------------------------- non-regular destination survives both paths
# A FIFO is non-regular (so the atomic path must be bypassed) and harmless to
# replace.  The reader drains the HEX; without it the compiler's open blocks.
run_to_fifo() { # log jal
  ( timeout 5 cat "$WORK/sink" >"$WORK/captured" ) &
  local reader=$!
  timeout 60 "$CC" -s "$INC" -no-codfile -hex "$WORK/sink" "$2" \
    >"$1" 2>&1
  local rc=$?
  wait "$reader" 2>/dev/null
  return $rc
}

if command -v mkfifo >/dev/null 2>&1; then
  mkfifo "$WORK/sink"
  run_to_fifo "$WORK/fifo_ok.log" "$WORK/ok.jal"
  rc=$?
  if [ "$rc" -eq 0 ] && [ -p "$WORK/sink" ] \
     && head -1 "$WORK/captured" | grep -q '^:'; then
    ok "successful compile writes through a non-regular target and keeps it"
  else
    bad "non-regular target (success)" "rc=$rc fifo=$([ -p "$WORK/sink" ] && echo yes || echo no)" \
      "$(tail -4 "$WORK/fifo_ok.log")"
  fi
  run_to_fifo "$WORK/fifo_bad.log" "$WORK/bad.jal"
  rc=$?
  if [ "$rc" -ne 0 ] && [ -p "$WORK/sink" ]; then
    ok "failing compile does not remove a non-regular target"
  else
    bad "non-regular target (failure)" "rc=$rc fifo=$([ -p "$WORK/sink" ] && echo yes || echo no)" \
      "$(tail -4 "$WORK/fifo_bad.log")"
  fi
  no_temps "non-regular target"
else
  echo "skip non-regular-target cases (mkfifo unavailable)"
fi

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "cli/hex contract: PASS"
  exit 0
fi
echo "cli/hex contract: $fails FAILURE(S)"
exit 1

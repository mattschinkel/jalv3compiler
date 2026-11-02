#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Address-space exhaustion is an ENVIRONMENT failure: one line on stderr,
# exit 1, no half-built-program diagnostics, no signal, no leftover HEX.
#
# Recurring defect family (no automated coverage before this pin):
#   fixes/fix_oom_policy.md             -- ENOMEM logged then every caller
#                                          carried on: 125 diagnostics, then
#                                          SIGABRT in the value detector
#   fixes/fix_compiler_oom_null_guards.md, fixes/fix_defensive_hardening.md
#
# The fix doc pinned this with a manual `ulimit -v` probe; this script makes
# it a gate.  A tight limit is deliberately applied to a corpus giant, then
# the run must show ONLY `error: out of memory; compilation stopped`.
#
# The 2026-11-02 audit lowered the tested range to 6 MB: at 7.5 MB a failed
# `pfile_tag_alloc` reached strcmp(NULL) (SIGSEGV) and at 7 MB a failed
# `variable_def_alloc` surfaced as `constant expression expected`.  Both are
# now fatal at the allocation boundary, so the whole 5-12 MB range reports
# one line (the loader floor below ~5 MB is skipped as rc=127).
#
# Portability: `ulimit -v` is enforced on Linux (the CI/bench platform).  If
# the limit is unavailable or the compiler fits at every tested limit, the
# check is reported as a SKIP, not a failure.  An enforced limit that yields
# a signal or a diagnostic cascade is a FAIL.
#
# Usage: unit-tests/test_oom_policy.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
INC="$REPO/demo_lib;$REPO/third_party/jallib/lib"
SAMPLE="$REPO/demo_sample/18f67j50_pointers.jal"

fails=0
clean_seen=0
success_seen=0
unclean=()

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

if [ ! -f "$SAMPLE" ]; then
  echo "skip OOM policy: $SAMPLE not present"
  exit 0
fi

# The exact clean outcome, as one predicate so every limit is judged the same.
judge() { # limit rc log
  local k=$1 rc=$2 log=$3
  local oom other summary
  oom=$(grep -c 'out of memory; compilation stopped' "$log" 2>/dev/null || true)
  other=$(grep -cE '\.jal:[0-9]+:' "$log" 2>/dev/null || true)
  summary=$(grep -cE '^[0-9]+ errors,' "$log" 2>/dev/null || true)
  if [ "$oom" -eq 1 ] && [ "$other" -eq 0 ] && [ "$summary" -eq 0 ] \
     && [ "$rc" -eq 1 ]; then
    return 0
  fi
  return 1
}

for k in 6000 7000 7500 8000 12000; do
  hex="$WORK/oom_$k.hex"
  log="$WORK/oom_$k.log"
  rm -f "$hex" "$hex.part"
  ( ulimit -v "$k" 2>/dev/null ) || {
    echo "note limit ${k}KB unsupported"
    continue
  }
  (
    ulimit -v "$k" 2>/dev/null
    exec "$CC" -s "$INC" -no-codfile -hex "$hex" "$SAMPLE"
  ) >"$log" 2>&1
  rc=$?

  if [ "$rc" -ge 128 ]; then
    bad "OOM at ${k}KB terminated by a signal" \
      "rc=$rc" "$(tail -4 "$log")"
    continue
  fi
  if [ "$rc" -eq 127 ]; then
    # dynamic loader could not map libc at this limit: below the floor, not
    # a compiler-memory event.
    echo "note ${k}KB: loader floor (rc=127), trying a looser limit"
    continue
  fi
  if [ "$rc" -eq 0 ]; then
    success_seen=1
    echo "note ${k}KB: compiler fit (rc=0), trying a tighter limit"
    continue
  fi

  if judge "$k" "$rc" "$log"; then
    clean_seen=1
    ok "OOM at ${k}KB: one fatal line, exit 1, no diagnostic cascade"
    if [ -e "$hex" ] || [ -e "$hex.part" ]; then
      bad "OOM at ${k}KB left an output file" "$(ls -l "$hex"* 2>/dev/null)"
    else
      ok "OOM at ${k}KB left no HEX or .part file"
    fi
  else
    unclean+=("${k}KB")
    bad "OOM at ${k}KB was not a clean fatal exit" \
      "rc=$rc oom_lines=$(grep -c 'out of memory' "$log")" \
      "$(grep -E 'error|errors,|\.jal:[0-9]+:' "$log" | head -6)"
  fi
done

echo "----"
if [ "$fails" -gt 0 ]; then
  echo "oom policy: $fails FAILURE(S)"
  exit 1
fi
if [ "$clean_seen" -eq 1 ]; then
  echo "oom policy: PASS"
  exit 0
fi
if [ "$success_seen" -eq 1 ]; then
  echo "skip OOM policy: the compiler fit at every enforced limit (ulimit -v ineffective?)"
  exit 0
fi
echo "FAIL OOM policy: no enforced limit produced a clean OOM"
exit 1

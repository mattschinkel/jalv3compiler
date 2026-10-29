#!/usr/bin/env bash
# Task-statement compile invariants (dev tooling 2026-10-05).
#
# Pins the front-end properties that runtime bodies cannot:
#   1. a task referencing itself through a task-control statement
#      (_task_suspend me() inside its own body) must NOT become
#      REENTRANT -- the statements are scheduler operations, not
#      invocations, and must not create CALL edges (the bug class from
#      fixes/fix_classic_task_control.md).
#   2. _task_yield compiles warning-free; the deprecated `suspend`
#      alias compiles with exactly one warning per use.
#   3. bare `start` is not a keyword (legal identifier since its rename)
#      -- `start foo()` must be a clean error, not a task start.
#
# Usage: sims/test_task_statement_invariants.sh [compiler]
# Exit 0 = all invariants hold.
#
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER="${1:-$ROOT/bin/jalv3-x86-64}"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

fails=0
ok()   { echo "OK   $1"; }
bad()  { echo "FAIL $1"; fails=$((fails+1)); }

# --- 1. self-referencing task stays non-REENTRANT ------------------------
cat > "$TMPDIR/self.jal" <<'EOF'
include 16f877
pragma task 2
var volatile byte x = 0
task _w() is
   _task_suspend _w()
   x = 1
   _task_restart _w()
end task
_task_start _w()
_task_resume _w()
_task_kill _w()
forever loop
end loop
EOF
if "$COMPILER" -s "$ROOT/demo_lib;$ROOT/third_party/jallib/lib" -no-codfile \
     -asm "$TMPDIR/self.asm" "$TMPDIR/self.jal" >"$TMPDIR/self.log" 2>&1;
then
  if grep -qE "^v__r__w\b|_r__w +EQU" "$TMPDIR/self.asm"; then
    bad "self-referencing task went REENTRANT (reentrancy counter emitted)"
  else
    ok "self-referencing task stays non-REENTRANT"
  fi
else
  bad "self-reference body failed to compile:"$'\n'"$(tail -3 "$TMPDIR/self.log")"
fi

# --- 2. _task_yield clean; deprecated `suspend` warns once per use -------
cat > "$TMPDIR/yield.jal" <<'EOF'
include 16f877
pragma task 2
var volatile byte x = 0
task _w() is
   _task_yield
   x = 1
   suspend
   x = 2
end task
_task_start _w()
forever loop
end loop
EOF
out="$("$COMPILER" -s "$ROOT/demo_lib;$ROOT/third_party/jallib/lib" -no-codfile \
  "$TMPDIR/yield.jal" 2>&1)"
if echo "$out" | tail -1 | grep -q "0 errors"; then
  n=$(echo "$out" | grep -c "keyword is deprecated")
  if [ "$n" -eq 1 ]; then
    ok "deprecated suspend warns exactly once per use"
  else
    bad "expected 1 deprecation warning, got $n:"$'\n'"$out"
  fi
else
  bad "yield/alias body failed to compile:"$'\n'"$out"
fi
# _task_yield alone must be warning-free
cat > "$TMPDIR/clean.jal" <<'EOF'
include 16f877
pragma task 2
var volatile byte x = 0
task _w() is
   _task_yield
   x = 1
end task
_task_start _w()
forever loop
end loop
EOF
out="$("$COMPILER" -s "$ROOT/demo_lib;$ROOT/third_party/jallib/lib" -no-codfile \
  "$TMPDIR/clean.jal" 2>&1)"
if echo "$out" | tail -1 | grep -q "0 errors, 0 warnings"; then
  ok "_task_yield compiles warning-free"
else
  bad "_task_yield body not warning-free:"$'\n'"$out"
fi

# --- 3. bare `start` is not a task statement ------------------------------
cat > "$TMPDIR/start.jal" <<'EOF'
include 16f877
var volatile byte x = 0
task _w() is
   x = 1
end task
start _w()
forever loop
end loop
EOF
out="$("$COMPILER" -s "$ROOT/demo_lib;$ROOT/third_party/jallib/lib" -no-codfile \
  "$TMPDIR/start.jal" 2>&1)"
if echo "$out" | tail -1 | grep -qE "^[1-9][0-9]* errors"; then
  ok "bare start errors (not a keyword)"
else
  bad "bare start did not error:"$'\n'"$out"
fi

echo "---"
if [ "$fails" -eq 0 ]; then
  echo "RESULT: ALL INVARIANTS HOLD"
  exit 0
fi
echo "RESULT: $fails INVARIANT(S) FAILED"
exit 1

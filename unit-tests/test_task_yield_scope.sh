#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the `_task_yield` scope rule: a yield is only meaningful at the
# task body's frame depth. Every core resumes a task by jumping to its saved
# address without restoring a stack context, so a yield inside a procedure
# called from the task would leave the caller's frame behind (and, with the
# ESP32 second core, resume on the other core's stack). The parser refuses
# it; this pin keeps the refusal on all three task back ends.
#
# Usage: unit-tests/test_task_yield_scope.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

# a procedure that yields, called from a task body
body='procedure _ys() is
   _task_yield
end procedure
task _yw() is
   _ys()
   _task_yield
end task
_task_start _yw()
forever loop
end loop
'

check_reject() {
  local name=$1 inc=$2
  printf '%s\n%s' "$inc" "$body" > "$WORK/$name.jal"
  out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/$name.jal" 2>&1)
  if echo "$out" | grep -q "_task_yield can only be used in a TASK"; then
    echo "ok   $name: yield inside a procedure is refused"
  else
    echo "FAIL $name: nested yield was not refused:"
    echo "$out" | grep -E "error" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

check_reject esp32  'include esp32_xtensa_lx6'
check_reject pic32  'include 32mz1024efh064'
check_reject classic 'include 16f877
pragma target clock 20_000_000'

# the same body with the yield where it belongs still compiles clean
cat > "$WORK/ok.jal" <<'EOF'
include esp32_xtensa_lx6
procedure _ys() is
   var byte bump = 0
   bump = bump + 1
end procedure
task _yw() is
   _ys()
   _task_yield
end task
_task_start _yw()
forever loop
end loop
EOF
out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/ok.jal" 2>&1)
if echo "$out" | grep -q "0 errors, 0 warnings"; then
  echo "ok   esp32: a body-level yield with a called procedure compiles"
else
  echo "FAIL esp32 body-level yield control:"
  echo "$out" | grep -E "error|warning" | sed 's/^/     /'
  fails=$((fails + 1))
fi

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all task-yield scope pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

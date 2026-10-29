#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the task-local-variable lift (fix_task_locals_lift): the
# var-in-task guard fires ONLY on classic cores (shared software frame
# area -- locals would collide across tasks); the 32-bit back ends
# give every local a unique linear address, so task bodies may declare
# variables there (runtime-proven by the task_locals QEMU bodies).
#
# Usage: unit-tests/test_task_locals.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

body='pragma task 2
task _tw() is
   var byte local_counter = 0
   local_counter = local_counter + 1
   _task_yield
end task
_task_start _tw()
forever loop
end loop
'

# classic: still a clean single-error refusal
printf 'include 16f877\npragma target clock 20_000_000\n%s' "$body" > "$WORK/cl.jal"
out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/cl.jal" 2>&1)
if echo "$out" | grep -q "variables cannot be declared inside a task body on this target"; then
  echo "ok   classic: var-in-task stays an error (shared frame area)"
else
  echo "FAIL classic var-in-task guard:"
  echo "$out" | grep "error" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# esp32: compiles clean (unique linear homes)
printf 'include esp32_xtensa_lx6\n%s' "$body" > "$WORK/es.jal"
out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/es.jal" 2>&1)
if echo "$out" | grep -q "0 errors, 0 warnings"; then
  echo "ok   esp32: var-in-task compiles (unique linear homes)"
else
  echo "FAIL esp32 var-in-task:"
  echo "$out" | grep -E "error|warning" | sed 's/^/     /'
  fails=$((fails + 1))
fi

# pic32: compiles clean
printf 'include 32mz1024efh064\n%s' "$body" > "$WORK/p32.jal"
out=$("$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$WORK/p32.jal" 2>&1)
if echo "$out" | grep -q "0 errors, 0 warnings"; then
  echo "ok   pic32: var-in-task compiles (unique linear homes)"
else
  echo "FAIL pic32 var-in-task:"
  echo "$out" | grep -E "error|warning" | sed 's/^/     /'
  fails=$((fails + 1))
fi

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all task-locals pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

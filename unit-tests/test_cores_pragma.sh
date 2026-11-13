#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pins for the multi-core surface:
#   * `pragma target enable_cores N` (1/2 on ESP32, chip-limited; 1 or more;
#     a constant), replacing `pragma target second_core`;
#   * `_task_start [core(<expr>)] name() [core(<expr>)]` -- constant or
#     runtime expression, one clause, only with enable_cores 2;
#   * `_task_core(name())` reads the last core;
#   * the removed `pragma task` gets one tombstone diagnostic (slots are
#     sized from the task declarations).
#
# Usage: unit-tests/test_cores_pragma.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

ESP_INC='include esp32_xtensa_lx6'
TASK_DECL='task painter() is
   _task_yield
end task'
PARK='forever loop
end loop'

# prog FILE HEADER STARTS: device includes + pragmas/declarations, the task
# declaration, the starts/uses, and the parking loop
prog() {
  printf '%s\n%s\n%s\n%s\n' "$2" "$TASK_DECL" "${3:-}" "$PARK" > "$1"
}

compile() {
  "$CC" -s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib" "$1" 2>&1
}

expect_ok() { # name file
  local out
  out=$(compile "$2")
  if echo "$out" | grep -q "0 errors, 0 warnings"; then
    echo "ok   $1"
  else
    echo "FAIL $1: expected a clean compile"
    echo "$out" | grep -E "error" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

expect_err() { # name file pattern
  local out
  out=$(compile "$2")
  if echo "$out" | grep -q -- "$3"; then
    echo "ok   $1"
  else
    echo "FAIL $1: missing diagnostic '$3'"
    echo "$out" | grep -E "error" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

# ---- pragma target enable_cores -------------------------------------------
prog "$WORK/e2.jal" "$ESP_INC
pragma target enable_cores 2"
expect_ok "esp32: enable_cores 2" "$WORK/e2.jal"

prog "$WORK/e1.jal" "$ESP_INC
pragma target enable_cores 1"
expect_ok "esp32: enable_cores 1" "$WORK/e1.jal"

prog "$WORK/e3.jal" "$ESP_INC
pragma target enable_cores 3"
expect_err "esp32: enable_cores 3 refused" "$WORK/e3.jal" "is not possible"

prog "$WORK/e0.jal" "$ESP_INC
pragma target enable_cores 0"
expect_err "esp32: enable_cores 0 refused" "$WORK/e0.jal" "expects 1 or more"

prog "$WORK/ev.jal" "$ESP_INC
var byte n = 2
pragma target enable_cores n"
expect_err "esp32: enable_cores non-constant refused" "$WORK/ev.jal" \
  "constant expression expected"

prog "$WORK/p2.jal" "include 16f877
pragma target clock 20_000_000
pragma target enable_cores 2"
expect_err "classic: enable_cores 2 refused" "$WORK/p2.jal" "has 1 core"

prog "$WORK/p1.jal" "include 16f877
pragma target clock 20_000_000
pragma target enable_cores 1"
expect_ok "classic: enable_cores 1" "$WORK/p1.jal"

# ---- removed pragmas ------------------------------------------------------
prog "$WORK/sc.jal" "$ESP_INC
pragma target second_core on"
expect_err "removed: pragma target second_core" "$WORK/sc.jal" "was removed"

prog "$WORK/pt.jal" "$ESP_INC
pragma task 2"
expect_err "removed: pragma task" "$WORK/pt.jal" "was removed"

# ---- _task_start core(...) ------------------------------------------------
prog "$WORK/c1.jal" "$ESP_INC
pragma target enable_cores 2" "_task_start core(2) painter()"
expect_ok "core(<const>) before the task" "$WORK/c1.jal"

prog "$WORK/c2.jal" "$ESP_INC
pragma target enable_cores 2" "_task_start painter() core(2)"
expect_ok "core(<const>) after the task" "$WORK/c2.jal"

prog "$WORK/c3.jal" "$ESP_INC
pragma target enable_cores 2
var byte c = 2" "_task_start painter() core(c)"
expect_ok "core(variable)" "$WORK/c3.jal"

prog "$WORK/c0.jal" "$ESP_INC
pragma target enable_cores 2" "_task_start core(0) painter()"
expect_ok "core(0) = any core" "$WORK/c0.jal"

prog "$WORK/c4.jal" "$ESP_INC
pragma target enable_cores 2" "_task_start core(3) painter()"
expect_err "core(3) refused with 2 cores" "$WORK/c4.jal" "is not available"

prog "$WORK/c5.jal" "$ESP_INC" "_task_start core(1) painter()"
expect_err "core(...) needs enable_cores 2" "$WORK/c5.jal" \
  "needs .pragma target enable_cores 2."

prog "$WORK/c6.jal" "$ESP_INC
pragma target enable_cores 2" "_task_start core(2) painter() core(1)"
expect_err "one core clause per start" "$WORK/c6.jal" "may only appear once"

prog "$WORK/c7.jal" "include 16f877
pragma target clock 20_000_000" "_task_start core(1) painter()"
expect_err "classic: core(...) refused" "$WORK/c7.jal" \
  "needs .pragma target enable_cores 2."

# ---- _task_core(name()) ---------------------------------------------------
prog "$WORK/tc1.jal" "$ESP_INC
pragma target enable_cores 2
var byte got" "got = _task_core(painter())
_task_start core(2) painter()"
expect_ok "_task_core(name())" "$WORK/tc1.jal"

prog "$WORK/tc2.jal" "$ESP_INC
var byte got" "got = _task_core(painter())
_task_start painter()"
expect_err "_task_core needs enable_cores 2" "$WORK/tc2.jal" \
  "_task_core needs"

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "PASS: all multi-core surface pins hold"
else
  echo "FAIL: $fails pin(s) broke"
  exit 1
fi

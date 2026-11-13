#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Phase 4: ISR source wiring on the QEMU targets.
#
#   1. the UART-source ISR bodies dispatch (ESP32 UART, ESP32 on CPU
#      interrupt 5, PIC32 UART1);
#   2. each has a NEGATIVE CONTROL that must NOT dispatch: the ESP32
#      matrix routed to a different CPU interrupt than the handler is
#      armed on (CPU 1 body), the ESP32 source muted (CPU 5 body), and
#      the PIC32 source left at priority 0 (disabled).
#
# The positive and negative halves share the same body, so a vacuous
# runner (one that passes whatever it is given) fails the negative half,
# and a broken wiring that never dispatches fails the positive half.
#
# Usage: unit-tests/test_isr_source_wiring.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

run_esp32() {  # run_esp32 <file> <log>
  ( cd "$REPO" && timeout 120 bash sims/jal_esp32_validate.sh \
      --compiler "$CC" "$1" ) > "$2" 2>&1
}

run_pic32() {  # run_pic32 <file> <log>
  ( cd "$REPO" && timeout 120 bash sims/jal_sim_validate.sh \
      --compiler "$CC" "$1" ) > "$2" 2>&1
}

expect_pass() {  # expect_pass <label> <file> <runner> <log>
  local label="$1" file="$2" runner="$3" log="$4"
  if "$runner" "$file" "$log" && grep -q 'RESULT: PASS' "$log"; then
    echo "ok   $label dispatches"
  else
    echo "FAIL: $label did not dispatch"
    tail -4 "$log" | sed 's/^/     /'
    fails=$((fails + 1))
  fi
}

expect_no_dispatch() {  # expect_no_dispatch <label> <file> <runner> <log>
  local label="$1" file="$2" runner="$3" log="$4"
  if "$runner" "$file" "$log" && grep -q 'RESULT: PASS' "$log"; then
    echo "FAIL: $label dispatched (the negative control is vacuous)"
    fails=$((fails + 1))
  else
    echo "ok   $label does not dispatch"
  fi
}

# --- 1. the positive bodies ------------------------------------------------
expect_pass "ESP32 UART-source ISR" \
  "$REPO/sims/esp32/esp32_body_isr_uart.jal" run_esp32 "$WORK/e1.log"
expect_pass "ESP32 ISR on CPU interrupt 5" \
  "$REPO/sims/esp32/esp32_body_isr_cpu5.jal" run_esp32 "$WORK/e2.log"
expect_pass "PIC32 UART1-source ISR" \
  "$REPO/sims/pic32/32mx270f256b_isr_uart.jal" run_pic32 "$WORK/p1.log"

# --- 2. the negative controls ---------------------------------------------
sed 's/^DPORT_PRO_UART_INTR_MAP = 1$/DPORT_PRO_UART_INTR_MAP = 2/' \
  "$REPO/sims/esp32/esp32_body_isr_uart.jal" > "$WORK/e1_neg.jal"
sed 's/^UART0_INT_ENA = 0x00004000$/UART0_INT_ENA = 0x00000000/' \
  "$REPO/sims/esp32/esp32_body_isr_cpu5.jal" > "$WORK/e2_neg.jal"
sed 's/^IPC8 = 0x1C$/IPC8 = 0x00/' \
  "$REPO/sims/pic32/32mx270f256b_isr_uart.jal" > "$WORK/p1_neg.jal"

if ! cmp -s "$REPO/sims/esp32/esp32_body_isr_uart.jal" "$WORK/e1_neg.jal" \
    && ! cmp -s "$REPO/sims/esp32/esp32_body_isr_cpu5.jal" "$WORK/e2_neg.jal" \
    && ! cmp -s "$REPO/sims/pic32/32mx270f256b_isr_uart.jal" "$WORK/p1_neg.jal"; then
  expect_no_dispatch "ESP32 matrix routed to CPU 2 (armed on 1)" \
    "$WORK/e1_neg.jal" run_esp32 "$WORK/e1n.log"
  expect_no_dispatch "ESP32 source muted (armed on CPU 5)" \
    "$WORK/e2_neg.jal" run_esp32 "$WORK/e2n.log"
  expect_no_dispatch "PIC32 source at priority 0" \
    "$WORK/p1_neg.jal" run_pic32 "$WORK/p1n.log"
else
  echo "FAIL: a negative-control mutation did not apply"
  fails=$((fails + 1))
fi

if (( fails )); then
  echo "FAIL: $fails ISR wiring check(s)"
  exit 1
fi
echo "PASS: ISR source wiring"

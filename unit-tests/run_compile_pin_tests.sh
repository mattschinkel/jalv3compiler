#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Aggregate gate for the compile-only regression pins under scripts/.
#
# These scripts existed as documented "pins" in fix docs and AGENTS.md
# (each one guards a specific diagnostic or codegen shape) but NOTHING
# ran them: the tiered ladder only invoked the task canaries, the three
# runtime suites, the negative table and the MX clock words.  A pin that
# is never executed silently rots -- two of them had already broken
# (stale sample/include paths) before this runner was added.
#
# Usage: unit-tests/run_compile_pin_tests.sh [compiler]
# Exit:  0 = every pin passed, 1 = at least one pin failed/missing.
#
# The compiler defaults to bin/jalv3-x86-64; the gate runner passes its
# pinned compiler snapshot so a mid-run rebuild cannot mix binaries.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/jalv3-x86-64}"
PIN_TIMEOUT="${JALV2_PIN_TIMEOUT:-600}"
LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jalv2_pins-XXXXXX")"
trap 'rm -rf "$LOG_DIR"' EXIT

fails=()
checked=0

if [[ ! -x "$CC" ]]; then
  echo "error: compiler not executable: $CC" >&2
  exit 2
fi

report() { # name log rc
  local name="$1" log="$2" rc="$3"
  if [[ "$rc" -eq 0 ]]; then
    echo "ok   $name"
  else
    echo "FAIL $name (rc=$rc)"
    tail -60 "$log" | sed 's/^/     /'
    fails+=("$name")
  fi
}

# run_sh NAME SCRIPT [args...]
run_sh() {
  local name="$1" script="$2" log
  shift 2
  checked=$((checked + 1))
  if [[ ! -f "$ROOT/$script" ]]; then
    echo "FAIL $name: missing $script"
    fails+=("$name")
    return
  fi
  log="$LOG_DIR/$name.log"
  timeout "$PIN_TIMEOUT" "$ROOT/$script" "$@" >"$log" 2>&1
  report "$name" "$log" "$?"
}

# run_py NAME SCRIPT [args...]
run_py() {
  local name="$1" script="$2" log
  shift 2
  checked=$((checked + 1))
  if [[ ! -f "$ROOT/$script" ]]; then
    echo "FAIL $name: missing $script"
    fails+=("$name")
    return
  fi
  log="$LOG_DIR/$name.log"
  timeout "$PIN_TIMEOUT" python3 "$ROOT/$script" "$@" >"$log" 2>&1
  report "$name" "$log" "$?"
}

# run_movlb: the MOVLB pin takes the compiler through $JALV2.
run_movlb() {
  local log="$LOG_DIR/movlb.log"
  checked=$((checked + 1))
  timeout "$PIN_TIMEOUT" env JALV2="$CC" \
    bash "$ROOT/unit-tests/test_skip_verify_movlb_regression.sh" >"$log" 2>&1
  report "skip_verify_movlb" "$log" "$?"
}

cd "$ROOT" || exit 2

echo "compile-pin gate: compiler=$CC"

# String / parser diagnostics
run_sh string_error_paths          unit-tests/test_string_error_paths.sh "$CC"
run_sh string_heap_sizing          unit-tests/test_string_heap_sizing.sh "$CC"
run_sh string_formal_literals      unit-tests/test_string_formal_literals.sh "$CC"
run_sh optional_params_negative    unit-tests/test_optional_params_negative.sh "$CC"
run_sh optional_empty_string       unit-tests/test_optional_empty_string.sh "$CC"
run_py proc_entry_bsr              unit-tests/test_proc_entry_bsr.py --compiler "$CC"
run_sh include_runtime_error       unit-tests/test_include_runtime_error.sh "$CC"
run_sh print_shadow_negative      unit-tests/test_print_shadow_negative.sh "$CC"
run_sh openarray_chunked_pins    unit-tests/test_openarray_chunked_pins.sh "$CC"
run_sh label_leak_clean            unit-tests/test_label_leak_clean.sh "$CC"
run_py helper_entry_bsr            unit-tests/test_helper_entry_bsr.py --compiler "$CC"

# Malformed input must fail with DIAGNOSTICS, never a signal (SIGABRT/SIGSEGV).
# The fixture pin covers the two known entries into the p-code builder's
# missing-operand path; the fuzz run is the broad sweep. Both are DETERMINISTIC
# (fixed seed + fixed input count) and the fuzz job fails on any crash or hang.
# fixes/fix_malformed_input_clean.md.
run_sh malformed_input_clean       unit-tests/test_malformed_input_clean.sh "$CC"
run_py malformed_input_fuzz        unit-tests/fuzz_jal_inputs.py --compiler "$CC" \
  --inputs 4000 --seed 20261030 --minutes 30 --out-dir tmp/fuzz_gate
# A4: the SAME sweep through the in-process seam (JAL_MULTI), which compiles a
# whole batch in one process. It is also this gate's only coverage of the
# multi-file path and of jal_reset_between_compiles() under load.
run_py malformed_input_fuzz_multi  unit-tests/fuzz_jal_inputs.py --compiler "$CC" \
  --inputs 2000 --seed 20261030 --minutes 30 --in-process --batch 100 \
  --out-dir tmp/fuzz_gate_multi

# Storage / allocation / target rules
run_sh pointer_bind_semantics      unit-tests/test_pointer_bind_semantics.sh "$CC"
run_sh pointer_pseudo_binds        unit-tests/test_pointer_pseudo_binds.sh "$CC"
run_sh multibank_array_errors      unit-tests/test_multibank_array_errors.sh "$CC"
run_sh two_compiles               unit-tests/test_two_compiles.sh "$CC"
run_sh task_locals                 unit-tests/test_task_locals.sh "$CC"
run_movlb
run_py pic18_v6_access_bank        unit-tests/test_pic18_v6_access_bank.py --compiler "$CC"
run_py esp32_sfr_bit_alignment    unit-tests/test_esp32_sfr_bit_alignment.py --compiler "$CC"

# PIC32 implementation-defined shapes that QEMU cannot model
run_py pic32_bootflash_fuses       unit-tests/test_pic32_bootflash_fuses.py --compiler "$CC"
run_py pic32_isr_shapes            unit-tests/test_pic32_isr_shapes.py --compiler "$CC"
run_py pic32mx_mvec_layout         unit-tests/test_pic32mx_mvec_layout.py --compiler "$CC"
# (test_pic32mx_clock_words.py is its own L1 gate in unit-tests/run_gates.sh.)

echo ""
if [[ "${#fails[@]}" -gt 0 ]]; then
  echo "RESULT: FAIL (${#fails[@]} of $checked pin groups failed)"
  printf '  failed: %s\n' "${fails[@]}"
  exit 1
fi
echo "RESULT: PASS ($checked pin groups)"
exit 0

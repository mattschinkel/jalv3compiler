#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Aggregate gate for the compile-only regression pins under scripts/.
#
# These scripts existed as documented "pins" in fix docs and the project notes
# (each one guards a specific diagnostic or codegen shape) but NOTHING
# ran them: the tiered ladder only invoked the task canaries, the three
# runtime suites, the negative table and the MX clock words.  A pin that
# is never executed silently rots -- two of them had already broken
# (stale sample/include paths) before this runner was added.
#
# Usage: unit-tests/run_compile_pin_tests.sh [compiler]
# Exit:  0 = every pin passed, 1 = at least one pin failed/missing.
#
# The compiler defaults to bin/beta/jalv3-x86-64; the gate runner passes its
# pinned compiler snapshot so a mid-run rebuild cannot mix binaries.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${1:-$ROOT/bin/beta/jalv3-x86-64}"
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
    # In CI the step log needs a token to read; an annotation is visible
    # through the public API, so name the group and the first failure line.
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
      local detail
      detail=$(grep -m1 -E 'FAIL|error:|Error|assert' "$log" 2>/dev/null \
               | tr '\r\n' '  ' | cut -c1-300)
      echo "::error title=compile pin group failed::${name} (rc=${rc}) ${detail}"
    fi
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

# GATE MODE (T3): every baseline pin starts ENGINE-OFF, so an ambient
# JALV2_FAT_POINTERS (e.g. while measuring the default flip) cannot change a
# baseline.  Mode-specific pins set their own value per command; the gate
# below asserts the baseline scripts each state their mode.
JALV2_FAT_POINTERS=0
export JALV2_FAT_POINTERS
for _s in test_openarray_chunked_pins.sh test_multibank_array_errors.sh \
          test_alloc_capacity.sh test_access_recipe.sh run_plan_gate.sh \
          run_access_matrix.sh; do
  if ! grep -q 'JALV2_FAT_POINTERS' "$ROOT/unit-tests/$_s"; then
    echo "FAIL: gate-mode pin (T3): $_s does not state JALV2_FAT_POINTERS" >&2
    exit 2
  fi
done

cd "$ROOT" || exit 2

echo "compile-pin gate: compiler=$CC"

# String / parser diagnostics
run_sh string_error_paths          unit-tests/test_string_error_paths.sh "$CC"
# Typed front-end semantic subscript layer (the retired JALV2_FRONT_IR
# switch is gone; the layer is the only lowering): the clone witnesses run
# under gpsim and must keep passing as the single-arm pin.
run_sh front_ir_parity             unit-tests/test_front_ir_parity.sh "$CC"
# The same sem-on runtime equivalence on the flat 32-bit (PIC32) back end.
run_sh front_ir_pic32              unit-tests/test_front_ir_parity_pic32.sh "$CC"
run_sh front_ir_esp32              unit-tests/test_front_ir_parity_esp32.sh "$CC"
# The finer alias attribution: the distinct-global clone is allowed, the
# same-string case is still refused.
run_sh front_ir_clone_builtin      unit-tests/test_front_ir_clone_builtin.sh "$CC"
# Public `string_*` helper calls are outlined through the strings_dynamic
# forwarder (one shared body, ~21 words/site) instead of expanding the whole
# builtin body (~740 words/site). fixes/fix_string_outline_calls.md.
run_sh string_outline_calls         unit-tests/test_string_outline_calls.sh "$CC"
# Phase 4b: EVERY user-level string store must lower to the shared body (the
# ~500-word inline expansion stays legal only inside the forwarder itself, per
# the measurement recorded in docs/investigations/full_ir_phase4_decision_*).
run_sh string_store_outline_coverage unit-tests/test_string_store_outline_coverage.sh "$CC"
# A 'put/'get accessor used inside its own body compiles to a self-call (an
# unbounded hardware-stack recursion, silently) -- refused with one message.
run_sh accessor_self_recursion     unit-tests/test_accessor_self_recursion.sh "$CC"
run_sh pic12_strings               unit-tests/test_pic12_strings.sh "$CC"
run_sh string_heap_sizing          unit-tests/test_string_heap_sizing.sh "$CC"
run_sh string_formal_literals      unit-tests/test_string_formal_literals.sh "$CC"
# Open-array formal CHUNK-MAP ambiguity: a formal serving two different
# multi-bank actuals walks only the first one's chunks (silently read the
# first string's text); the compiler's `_strargN` buffer is the case the
# author can act on (fixes/fix_openarray_formal_chunk_map.md).
run_sh string_arg_chunk_map        unit-tests/test_string_arg_chunk_map.sh "$CC"
# The USER-array half of the same defect: a formal fed two different multi-bank
# user arrays addressed the second one with the first one's bank math.  Now
# every such formal owns a map that each call site fills (and none is created
# for a single layout -- the inertness the corpus relies on).
# fixes/fix_openarray_user_array_percall.md
run_sh openarray_user_array_percall unit-tests/test_openarray_user_array_percall.sh "$CC"
# fixes/fix_openarray_out_formals.md
run_sh openarray_out_params unit-tests/test_openarray_out_params.sh "$CC"
# fixes/fix_openarray_pointer_store.md
run_sh openarray_pointer_store unit-tests/test_openarray_pointer_store.sh "$CC"
# fixes/fix_open_bit_formal_out.md
run_sh open_bit_out_refused unit-tests/test_open_bit_out_refused.sh "$CC"
run_sh error_hook                  unit-tests/test_error_hook.sh "$CC"
run_sh optional_params_negative    unit-tests/test_optional_params_negative.sh "$CC"
run_sh optional_empty_string       unit-tests/test_optional_empty_string.sh "$CC"
run_py proc_entry_bsr              unit-tests/test_proc_entry_bsr.py --compiler "$CC"
run_sh include_runtime_error       unit-tests/test_include_runtime_error.sh "$CC"
run_sh print_shadow_negative      unit-tests/test_print_shadow_negative.sh "$CC"
run_sh openarray_chunked_pins    unit-tests/test_openarray_chunked_pins.sh "$CC"
run_sh label_leak_clean            unit-tests/test_label_leak_clean.sh "$CC"
run_py helper_entry_bsr            unit-tests/test_helper_entry_bsr.py --compiler "$CC"
# Compiler-owned names are reserved and a malformed _string_heap_size reports
# ONCE, not as a cascade (fix_reserved_internal_names, the internal-name guard
# family). The exact error count is part of the contract.
run_sh reserved_internal_names    unit-tests/test_reserved_internal_names.sh "$CC"

# Malformed input must fail with DIAGNOSTICS, never a signal (SIGABRT/SIGSEGV).
# The fixture pin covers the two known entries into the p-code builder's
# missing-operand path; the fuzz run is the broad sweep. Both are DETERMINISTIC
# (fixed seed + fixed input count) and the fuzz job fails on any crash or hang.
# fixes/fix_malformed_input_clean.md.
run_sh malformed_input_clean       unit-tests/test_malformed_input_clean.sh "$CC"
# Shape-pathological inputs the mutating fuzzer cannot generate (deep
# nesting, 2000 blocks, 1 MB arrays, binary junk). Every entry pins a
# defect found by hand: unbounded parser recursion (SIGSEGV), the 1000-block
# map cap that printed ERROR and exited 0, and the 1024-byte clamp that
# silently skipped BSS clearing. fixes/fix_bounded_parser_recursion.md,
# fixes/fix_data_area_map_guards.md.
run_sh adversarial_inputs          unit-tests/test_adversarial_inputs.sh "$CC"
# Address-space exhaustion is a clean fatal exit (one line, exit 1, no signal,
# no half-built-program diagnostics, no leftover HEX): fix_oom_policy +
# fix_compiler_oom_null_guards. Enforced with ulimit -v on a corpus giant.
run_sh oom_policy                  unit-tests/test_oom_policy.sh "$CC"
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
# The bind enforces the pointee signature: a scalar pointee takes a variable
# of the same signature (or an element / scalar pseudo-variable), never an
# array or a string.  fixes/fix_pointer_bind_signature.md.
run_sh pointer_bind_signature      unit-tests/test_pointer_bind_signature.sh "$CC"
run_sh pointer_pseudo_binds        unit-tests/test_pointer_pseudo_binds.sh "$CC"
run_sh multibank_array_errors      unit-tests/test_multibank_array_errors.sh "$CC"
run_sh pic_data_alloc_padding      unit-tests/test_pic_data_alloc_padding.sh "$CC"
run_sh pic_shared_window_pins      unit-tests/test_pic_shared_window_pins.sh "$CC"
run_sh pointer_array_capacity      unit-tests/test_pointer_array_capacity.sh "$CC"
run_sh pointer_array_chunk_init    unit-tests/test_pointer_array_chunk_init.sh "$CC"
run_sh fat_pointer_tags           unit-tests/test_fat_pointer_tags.sh "$CC"
run_sh fat_pointer_deref          unit-tests/test_fat_pointer_deref.sh "$CC"
run_sh alloc_segment_audit        unit-tests/test_alloc_segment_audit.sh "$CC"
run_sh alloc_capacity             unit-tests/test_alloc_capacity.sh "$CC"
run_sh access_audit               unit-tests/test_access_audit.sh "$CC"
# ENG S1: -explain-access / -dump-plan CLI validation (line grammar, path,
# unreadable source, unwritable dump with atomic output, happy paths).
run_sh access_cli                 unit-tests/test_access_cli.sh "$CC"
run_sh openarray_pair_over_cap    unit-tests/test_openarray_pair_over_cap.sh "$CC"
# DX F1: strict JALV2_* on/off parsing (FALSE/space/typos) and --features.
run_sh env_switches              unit-tests/test_env_switches.sh "$CC"
# Dynamic-string append growth on a chunked heap (spare capacity, geometric
# growth, one compact-and-retry).
run_sh string_heap_append        unit-tests/test_string_heap_append.sh "$CC"
# F5 section 4: the walk-path null guard (an unbound pointer traps instead
# of silently reading the target's first byte).
run_sh unbound_walk_guard        unit-tests/test_unbound_walk_guard.sh "$CC"
# U3 Phase 1: the open-array pairs live on the CALL command; the acceptance
# comparator re-derives them from the parameters (with drop/alter fault hooks
# proving it is non-vacuous).
run_sh u3_pair_attach             unit-tests/test_u3_pair_attach.sh "$CC"
run_sh pic12_indirect_refusal     unit-tests/test_pic12_indirect_refusal.sh "$CC"
run_sh two_compiles               unit-tests/test_two_compiles.sh "$CC"
# Header dependency tracking (the mixed-object lesson): a header edit must
# rebuild the CURRENT flavor's object, and building another flavor must not
# rewrite the dependency rules. fixes/fix_build_dep_flavor_collision.md
run_sh build_deps                  unit-tests/test_build_deps.sh
# Stack-depth walker edge classes: small CFG shapes with an unambiguous depth,
# plus the at-limit / over-limit diagnostics (fixes/fix_pic_stack_depth_*.md).
run_sh stack_depth_cfg             unit-tests/test_stack_depth_cfg.sh "$CC"
run_py in_process_units            unit-tests/test_inprocess_units.py --compiler "$CC"
run_sh task_locals                 unit-tests/test_task_locals.sh "$CC"
run_movlb
run_py pic18_v6_access_bank        unit-tests/test_pic18_v6_access_bank.py --compiler "$CC"
run_py esp32_sfr_bit_alignment    unit-tests/test_esp32_sfr_bit_alignment.py --compiler "$CC"
run_py esp32_isr_cpu_int          unit-tests/test_esp32_isr_cpu_int.py --compiler "$CC"
run_py esp32_sfr_bitfield         unit-tests/test_esp32_sfr_bitfield.py --compiler "$CC"
# Driver/output contract the corpus and simulators cannot see: CLI requests
# terminate, HEX install is atomic (a failed compile preserves the previous
# good image), and a non-regular destination is never renamed over or removed
# (fix_cli_help_infinite_loop, fix_atomic_hex_rename_install,
# fix_hex_output_dev_null_guard). Uses a FIFO, never /dev/null.
# COD (debug info) output: the only path that exercises src/libutils/cod_file.c.
run_sh codfile_output             unit-tests/test_codfile_output.sh "$CC"
run_sh cli_hex_contract           unit-tests/test_cli_hex_contract.sh "$CC"

# PIC32 implementation-defined shapes that QEMU cannot model
run_py pic32_bootflash_fuses       unit-tests/test_pic32_bootflash_fuses.py --compiler "$CC"
run_py pic32_isr_shapes            unit-tests/test_pic32_isr_shapes.py --compiler "$CC"
run_py pic32mx_mvec_layout         unit-tests/test_pic32mx_mvec_layout.py --compiler "$CC"

# Source comment blocks vs the code they document (no build needed).
run_py comment_docs                unit-tests/check_comment_docs.py
# Step 4: FAT recipe dispatch, fatal default, and plan generations.
run_sh access_recipes              unit-tests/test_access_recipes.sh
# Step 2: the plan-diff gate over the pinned classic subset.
run_sh access_plan_gate            unit-tests/run_plan_gate.sh "$CC"
# E3: the neutral dispatcher, both back ends' table self-checks, and the
# PIC32 pointer-read spike's byte-identical pin.
run_sh access_recipe               unit-tests/test_access_recipe.sh "$CC"
# Every simulator validate block must pin something a stall cannot satisfy:
# a checkpoint-counter expectation or at least one nonzero value.  The
# runtime harness cannot see the difference (18f4620_float_str_battery passed
# `-n 1` with only `expect v__fb_bad 0` until 2026-11-01).  No build needed.
run_py validate_contract           unit-tests/check_validate_contract.py
# No NEW validate-block exemption (skip_gpsim / checkpoint_verify: off /
# allow_compiler_warnings); pinned in unit-tests/validate_exemptions_baseline.txt.
run_py validate_exemptions         unit-tests/check_validate_exemptions.py
# The harness's own decision points (crash-without-summary = failure, spawn
# error vs sample failure, asm-noise normalization, --compare with no
# baseline exits 2). Every bug in fixes/fix_unittests_*.md was a green harness
# measuring nothing; this is the self-test that does not need a compiler.
run_py harness_contract            unit-tests/test_harness_contract.py
# (test_pic32mx_clock_words.py is its own L1 gate in unit-tests/run_gates.sh.)

echo ""
if [[ "${#fails[@]}" -gt 0 ]]; then
  echo "RESULT: FAIL (${#fails[@]} of $checked pin groups failed)"
  printf '  failed: %s\n' "${fails[@]}"
  exit 1
fi
echo "RESULT: PASS ($checked pin groups)"
exit 0

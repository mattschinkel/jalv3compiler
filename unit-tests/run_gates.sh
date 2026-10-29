#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Tiered gate runner for the jalv2 tree (2026-10-06 — docs/test_gate_tiers_plan.md).
#
# Levels — each implies the ones below; the run STOPS at the first failing
# gate (fix it before continuing to the next level):
#   L0  build + syntax smoke + the fix's own witnesses          (~1 min)
#   L1  + task invariants/golden + RUNTIME suites targeted by
#       what changed (gpsim first whenever codegen can move)
#       + the compile-only pin scripts                            (~3–9 min)
#   L2  + unit --current + compare + HEX-manifest A/B           (~8–12 min)
#   L3  + second QEMU suite + ASan corpus + sample-kit sweep;
#       (a green unpinned L2 run escalates here automatically)
#       unit/compare ARCHIVE reports + CHANGELOG here (landing) (~15–25 min)
#
# Usage:
#   unit-tests/run_gates.sh --level 0 --witness tmp/probe.jal [--witness ...]
#                                # witnesses must compile 0 errors; a file
#                                # with an @sim_validate block runs its QEMU gate
#   unit-tests/run_gates.sh --auto  # level + suite subset derived from git status
#   unit-tests/run_gates.sh         # default level 2; a green UNPINNED L2 run
#                                 # escalates to L3 automatically (landing)
#                                 # -- explicit --level 2 is an iteration ceiling
#   unit-tests/run_gates.sh --dry-run --auto
#   unit-tests/run_gates.sh --level 2 --update-manifest
#                                # refresh the HEX A/B manifest after the run
#                                # (commit it with INTENTIONAL codegen changes)
#   unit-tests/run_gates.sh --skip-gpsim      # 32-bit-only changes
#   unit-tests/run_gates.sh --quick           # legacy alias for --level 1
#   unit-tests/run_gates.sh --stop            # terminate the ACTIVE gate run
#                                         #   (whole process tree + any
#                                         #   inherited-lock-fd holders;
#                                         #   safe while a run is in flight)
#   unit-tests/run_gates.sh --compiler PATH   # compiler for L0/L1 (unit/compare
#                                          # always test ./bin/jalv3-x86-64)
#   unit-tests/run_gates.sh --no-san          # L3 without the ASan corpus
#
# HEX A/B: unit-tests/manifest/hex_manifest.txt via unit-tests/hex_manifest.py
# — committed with every compiler change, so ONE corpus run is both the test
# and the byte-identity check (the old second-corpus A/B is gone).
#
# Environment: levels < 3 force JALV2_NO_REPORT_ARCHIVE /
# JALV2_NO_CHANGELOG_ENTRY (iterations stay quiet); level 3 is the landing
# run that archives.
#
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
set -uo pipefail

# --- INPUT PINNING (2026-10-07) ---------------------------------------------
# A gate run must be immune to edits made while it runs: bash reads this
# script LAZILY, so editing the file mid-run shifts bytes under the
# interpreter (seen as bogus syntax errors at untouched lines).  Copy
# ourselves to a temp file ONCE and exec the copy -- the running instance
# then reads frozen bytes no matter what happens to the original.
if [[ -z "${JALV2_GATE_SNAPSHOT:-}" ]]; then
  JALV2_GATE_SNAPSHOT="$(mktemp "${TMPDIR:-/tmp}/jalv2_gate-run-XXXXXX.sh")"
  JALV2_GATE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  export JALV2_GATE_SNAPSHOT JALV2_GATE_ROOT
  cp "${BASH_SOURCE[0]}" "$JALV2_GATE_SNAPSHOT"
  exec bash "$JALV2_GATE_SNAPSHOT" "$@"
fi
ROOT="${JALV2_GATE_ROOT:?snapshot root missing}"
cd "$ROOT"

# While this run is active its inputs are PINNED: the script snapshot
# above, and the compiler snapshot taken after the L0 build.  Editing
# unit-tests/run_gates.sh or rebuilding bin/jalv2* mid-run can no longer
# corrupt THIS run -- but if you INTENDED a change to apply, stop first:
#   unit-tests/run_gates.sh --stop
echo "gates: run $$ active — inputs pinned (script snapshot + compiler snapshot after L0)"

# ------------------------------------------------------------- stop support
# A killed gate run orphans its children (suite runners, compilers): they
# keep climbing the ladder with a mixed binary and hold the inherited
# lock fd, so the next run refuses to start.  `--stop` (and the TERM/INT
# trap below) kills the ACTIVE run's whole process tree, then any process
# still holding an fd on the lockfile, and verifies the lock is free.

GATES_LOCK_PATH="${TMPDIR:-/tmp}/jalv2_run_gates.lock"

# kill a PID and every descendant (children first, recursive)
gates_kill_tree() {
  local parent="$1" kid
  for kid in $(pgrep -P "$parent" 2>/dev/null); do
    gates_kill_tree "$kid"
  done
  kill -TERM "$parent" 2>/dev/null
}

# any process holding an fd on the lockfile (inherited-fd zombies)
gates_lock_holders() {
  local pid
  for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    if ls -la "/proc/$pid/fd" 2>/dev/null | grep -q jalv2_run_gates; then
      echo "$pid"
    fi
  done
}

gates_do_stop() {
  local holder pid
  holder="$(cat "$GATES_LOCK_PATH" 2>/dev/null)"
  if [[ -z "$holder" ]] || ! kill -0 "$holder" 2>/dev/null; then
    # the holder died; the lock may still be pinned by inherited fds
    echo "run_gates --stop: holder PID ${holder:-unknown} is gone;"
    echo "  clearing inherited-fd holders"
  else
    echo "run_gates --stop: stopping gate run PID $holder"
    echo "  $(ps -o args= -p "$holder" 2>/dev/null | cut -c1-100)"
    gates_kill_tree "$holder"
  fi
  sleep 1
  for pid in $(gates_lock_holders); do
    echo "  force-killing lock-fd holder PID $pid:" \
         "$(ps -o args= -p "$pid" 2>/dev/null | cut -c1-80)"
    kill -9 "$pid" 2>/dev/null
  done
  sleep 1
  local left
  left="$(gates_lock_holders | tr '\n' ' ')"
  if [[ -n "$left" ]]; then
    echo "error: lock still held by: $left" >&2
    exit 1
  fi
  echo "run_gates --stop: gate processes stopped, lock free"
  exit 0
}

# --stop dispatches BEFORE the single-instance lock (it must work while
# another run holds the lock -- that is its whole job).  Handled here,
# after the functions, before anything that could refuse to run.
for gates_arg in "$@"; do
  if [[ "$gates_arg" == "--stop" ]]; then
    gates_do_stop
  fi
done

# ------------------------------------------------------------- single instance
# ONE gate run at a time (user rule 2026-10-06): levels must never run twice
# or alongside each other -- the hex manifest A/B, the golden canaries and
# the ASan build dir are all single-writer.  flock auto-releases if the
# holder dies, so there is no stale-lock cleanup.
GATES_LOCK="$GATES_LOCK_PATH"
# append-mode open: a CONTENDER must not truncate the holder's PID out of
# the lockfile before its flock fails
exec 9>>"$GATES_LOCK"
if ! flock -n 9; then
  holder_pid="$(cat "$GATES_LOCK" 2>/dev/null)"
  echo "error: another gate run is already active" >&2
  if [[ -n "$holder_pid" ]]; then
    echo "       holder: PID $holder_pid -- $(ps -o args= -p "$holder_pid" 2>/dev/null | cut -c1-100)" >&2
    echo "       wait for it to finish (or kill PID $holder_pid) before starting another" >&2
  else
    echo "       wait for the active run to finish before starting another" >&2
  fi
  exit 3
fi
echo "$$" > "$GATES_LOCK"

# a plain kill (or Ctrl-C) now tears down the whole tree instead of
# orphaning the running suites
gates_self_stop() {
  echo "run_gates: stop signal — terminating child processes" >&2
  gates_kill_tree "$$"
  exit 130
}
trap gates_self_stop TERM INT

LEVEL=2
LEVEL_EXPLICIT=0
AUTO=0
DRY=0
SKIP_GPSIM=0
WITH_SAN=1
UPDATE_MANIFEST=0
COMPILER="$ROOT/bin/jalv3-x86-64"
WITNESSES=()
LEGACY_QUICK=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --level)           LEVEL="$2"; LEVEL_EXPLICIT=1; shift 2 ;;
    --auto)            AUTO=1; shift ;;
    --dry-run)         DRY=1; shift ;;
    --skip-gpsim)      SKIP_GPSIM=1; shift ;;
    --san)             WITH_SAN=1; shift ;;
    --no-san)          WITH_SAN=0; shift ;;
    --compiler)        COMPILER="$2"; shift 2 ;;
    --witness)         WITNESSES+=("$2"); shift 2 ;;
    --update-manifest) UPDATE_MANIFEST=1; shift ;;
    --quick)           LEGACY_QUICK=1; shift ;;
    -h|--help)         awk '/^set /{exit} NR>=2{print}' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done

if [[ "$LEGACY_QUICK" == "1" && "$LEVEL_EXPLICIT" != "1" ]]; then
  LEVEL=1
fi

case "$LEVEL" in
  0|1|2|3) ;;
  *) echo "error: --level must be 0..3" >&2; exit 2 ;;
esac
# worker count: JALV2_TEST_JOBS overrides the default 16 (same env var and
# default as the unit/gpsim/QEMU suite runners; CLI flags always win)
# (e.g. share the host: JALV2_TEST_JOBS=8 unit-tests/run_gates.sh ...)
JOBS="${JALV2_TEST_JOBS:-16}"

if [[ ! -x "$COMPILER" ]]; then
  echo "error: compiler not found: $COMPILER" >&2
  exit 2
fi
if [[ ! -f "$ROOT/third_party/jallib/lib/18f67j50.jal" ]]; then
  echo "error: pristine jallib tree missing (even L0's syntax smoke includes a jallib device file)." >&2
  echo "       run: unit-tests/fetch_jallib.sh   (or: make fetch-jallib)" >&2
  exit 2
fi

# ---------------------------------------------------------------- change class
CHANGE_FRONT=0; CHANGE_PIC12=0; CHANGE_PIC32=0; CHANGE_ESP32=0
CHANGE_BUILD=0; CHANGE_OTHER=0

classify_changes() {
  local f
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    case "$f" in
      src/jal/*|src/libcore/*)             CHANGE_FRONT=1 ;;
      src/libpic12/*)                      CHANGE_PIC12=1 ;;
      src/libpic32/*)                      CHANGE_PIC32=1 ;;
      src/libesp32/*)                      CHANGE_ESP32=1 ;;
      src/Makefile*|src/*/Makefile|src/*/build.sh) CHANGE_BUILD=1 ;;
      *)                                   CHANGE_OTHER=1 ;;
    esac
  done < <(git status --porcelain 2>/dev/null | cut -c4-)
}

is_lifetime_family() {
  git status --porcelain 2>/dev/null | cut -c4- \
    | grep -qE 'src/(libcore/(pfile|pf_expr|pf_cmd|pf_op|variable|vardef|hash)|jal/jal_string|libesp32/esp_var|libpic32/mips_var)'
}

suggest_level() {
  # 0: no compiler-source code changed (docs/lib/samples/build system only)
  if (( !CHANGE_FRONT && !CHANGE_PIC12 && !CHANGE_PIC32 && !CHANGE_ESP32 )); then
    echo 0
  elif is_lifetime_family; then
    echo 3
  else
    echo 2
  fi
}

# runtime suite selection for L1 (gpsim first whenever codegen can move)
SEL_GPSIM=0; SEL_PIC32=0; SEL_ESP32=0
select_suites() {
  if (( AUTO )); then
    (( CHANGE_FRONT || CHANGE_PIC12 )) && SEL_GPSIM=1
    (( CHANGE_FRONT || CHANGE_PIC32 )) && SEL_PIC32=1
    (( CHANGE_ESP32 )) && SEL_ESP32=1
    # front-end L1 runs gpsim + ONE QEMU suite (pic32); the esp32 suite is
    # the L3 second suite (see gate below)
  else
    SEL_GPSIM=1; SEL_PIC32=1; SEL_ESP32=1
  fi
  (( SKIP_GPSIM )) && SEL_GPSIM=0
  return 0
}

classify_changes
select_suites
if (( AUTO && !LEVEL_EXPLICIT )); then
  LEVEL="$(suggest_level)"
fi

echo "gates: level $LEVEL$([[ $AUTO == 1 ]] && echo ' (auto)') — " \
     "suites: gpsim=$((SEL_GPSIM)) pic32=$((SEL_PIC32)) esp32=$((SEL_ESP32))"
if (( AUTO )); then
  echo "change class: front=$((CHANGE_FRONT)) pic12=$((CHANGE_PIC12)) " \
       "pic32=$((CHANGE_PIC32)) esp32=$((CHANGE_ESP32)) build=$((CHANGE_BUILD)) " \
       "other=$((CHANGE_OTHER))"
fi

# ---------------------------------------------------------------- runner
FAILED=0
declare -a SUMMARY

print_summary() {
  echo ""
  echo "=============================================================="
  printf '%s\n' "${SUMMARY[@]}"
  echo "=============================================================="
  if [[ "$FAILED" != "0" ]]; then
    echo "RESULT: FAIL"
  else
    echo "RESULT: ALL GATES PASS (level $LEVEL)"
  fi
}

gate() {  # gate <name> <command...>
  local name="$1"; shift
  echo ""
  echo "=============================================================="
  echo "=== gate: $name"
  echo "=============================================================="
  # background+wait so the TERM/INT trap fires IMMEDIATELY (a foreground
  # child defers bash trap handling until it exits -- the stop-signal
  # would sit in the queue for the whole suite)
  "$@" &
  GATE_CHILD=$!
  if wait "$GATE_CHILD"; then
    SUMMARY+=("PASS  $name")
  else
    SUMMARY+=("FAIL  $name")
    FAILED=1
    echo "gate FAILED: $name — stopping (fix this before the next level)" >&2
    print_summary
    exit 1
  fi
}

skip() { SUMMARY+=("SKIP  $1"); }

witness_check() {  # compile 0 errors; run the QEMU gate when the block exists
  local w="$1"
  local out
  if ! out=$("$COMPILER" -s 'demo_lib;third_party/jallib/lib;tests' -no-codfile \
        "$w" 2>&1); then
    echo "$out" | tail -5
    return 1
  fi
  if ! tail -1 <<<"$out" | grep -q "^0 errors"; then
    tail -5 <<<"$out"
    return 1
  fi
  if grep -q '@sim_validate begin' "$w"; then
    echo "witness carries an @sim_validate block — running its QEMU gate"
    ./sims/jal_sim_validate.sh --compiler "$COMPILER" "$w"
  else
    echo "OK: $(tail -1 <<<"$out")"
  fi
}

sample_kit_sweep() {
  local fails=0 s base out
  for s in demo_sample/*.jal; do
    base="$(basename "$s")"
    # the summary line must EXIST (a missing tally = compiler abort) and
    # start "0 errors" — but it is not always the LAST line: ESP32 prints
    # its usage report after the tally, so grep instead of tail -1
    out=$("$COMPILER" -s 'demo_lib;third_party/jallib/lib' -no-codfile "$s" 2>&1)
    if grep -qE '^0 errors, [0-9]+ warnings' <<<"$out"; then
      echo "  OK   $base"
    else
      echo "  FAIL $base ($(grep -Eo '[0-9]+ errors?, [0-9]+ warnings?' <<<"$out" | tail -1))"
      fails=$((fails + 1))
    fi
  done
  return "$fails"
}

UNIT_ENV=()
if (( LEVEL < 3 )); then
  UNIT_ENV=(env JALV2_NO_REPORT_ARCHIVE=1 JALV2_NO_CHANGELOG_ENTRY=1)
fi

if (( DRY )); then
  echo ""
  echo "dry run — would execute:"
  echo "  L0 build (incremental make)"
  echo "  L0 syntax smoke"
  for w in "${WITNESSES[@]}"; do echo "  L0 witness: $w"; done
  if (( LEVEL >= 1 )); then
    echo "  L1 task statement invariants"
    echo "  L1 task codegen golden canary"
    (( SEL_GPSIM )) && echo "  L1 gpsim (classic runtime)"
    (( SEL_PIC32 )) && echo "  L1 pic32 qemu"
    (( SEL_ESP32 )) && echo "  L1 esp32 qemu"
    echo "  L1 compile-pin regressions (unit-tests/run_compile_pin_tests.sh)"
    (( SEL_GPSIM || SEL_PIC32 || SEL_ESP32 )) || echo "  L1 (no runtime suites for this change class)"
  fi
  if (( LEVEL >= 2 )); then
    if (( LEVEL < 3 )); then
      echo "  L2 unit --current (no archive)"
      echo "  L2 compare vs baseline (no archive)"
    else
      echo "  L2 unit --current (archiving)"
      echo "  L2 compare vs baseline (archiving)"
    fi
    if (( UPDATE_MANIFEST )); then
      echo "  L2 hex manifest store"
    else
      echo "  L2 hex manifest A/B diff"
    fi
  fi
  if (( LEVEL >= 3 )); then
    echo "  L3 second qemu suite (when L1 was narrowed)"
    (( WITH_SAN )) && echo "  L3 ASan corpus (build if missing)"
    echo "  L3 sample-kit sweep"
  fi
  exit 0
fi

# ---------------------------------------------------------------- L0
if [[ "$COMPILER" == "$ROOT/bin/jalv3-x86-64" ]]; then
  gate "L0 build (incremental)" make -C src ARCH=x86-64
else
  skip "L0 build (custom compiler: $COMPILER)"
fi

# pin the compiler for the rest of the run: suites exec it hundreds of
# times, and a mid-run rebuild of bin/jalv3-x86-64 (a "quick fix" while
# tests are in flight) would silently mix binaries.  Every gate below
# uses the snapshot; rebuilds of the tree cannot reach this run.
GATE_SNAP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jalv2_gate-bin-XXXXXX")"
GATE_SNAP_CC="$GATE_SNAP_DIR/jalv3-x86-64"
if cp "$COMPILER" "$GATE_SNAP_CC" 2>/dev/null; then
  COMPILER="$GATE_SNAP_CC"
  export COMPILER
  echo "gates: compiler pinned to snapshot $GATE_SNAP_CC"
fi
trap 'rm -rf "$GATE_SNAP_DIR" "${JALV2_GATE_SNAPSHOT:-}"' EXIT
gate "L0 syntax smoke" \
  bash -c '"$0" -s third_party/jallib/lib "$1" 2>&1 | tail -1 | grep -q "^0 errors"' \
  "$COMPILER" "$ROOT/unit-tests/syntax_test.jal"
for w in "${WITNESSES[@]}"; do
  [[ -f "$w" ]] || { echo "witness not found: $w" >&2; exit 2; }
  gate "L0 witness $w" witness_check "$w"
done

# ---------------------------------------------------------------- L1
if (( LEVEL >= 1 )); then
  gate "L1 task statement invariants" \
    "$ROOT/sims/test_task_statement_invariants.sh" "$COMPILER"
  gate "L1 task codegen golden canary" \
    "$ROOT/sims/test_task_codegen_golden.sh" "$COMPILER"
  RAN_PIC32=0; RAN_ESP32=0
  if (( SEL_GPSIM )); then
    gate "L1 gpsim (classic runtime)" \
      python3 sims/run_regression_suite.py --jobs "$JOBS" --quiet
  fi
  if (( SEL_PIC32 )); then
    gate "L1 pic32 qemu" \
      python3 sims/run_pic32_regression.py --run --compiler "$COMPILER"
    gate "L1 pic32mx clock/fuse words" \
      python3 sims/test_pic32mx_clock_words.py --compiler "$COMPILER"
    gate "L1 pic32mz config-pragma words" \
      python3 sims/test_pic32mz_config_pragma_words.py --compiler "$COMPILER"
    RAN_PIC32=1
  fi
  if (( SEL_ESP32 )); then
    gate "L1 esp32 qemu" \
      python3 sims/run_esp32_regression.py --run --compiler "$COMPILER"
    RAN_ESP32=1
  fi
  if (( !SEL_GPSIM && !SEL_PIC32 && !SEL_ESP32 )); then
    skip "L1 runtime suites (none for this change class)"
  fi
  # Compile-only pins (negative diagnostics + implementation-defined codegen
  # shapes).  They run AFTER the runtime suites: gpsim-first discipline, and
  # a compile-clean run proves nothing about behavior.  This gate is what
  # actually executes the scripts documented as "pins" in fixes/ (nothing
  # did before 2026-10-22 -- see fixes/fix_test_suite_gap_audit.md).
  gate "L1 compile-pin regressions" \
    "$ROOT/unit-tests/run_compile_pin_tests.sh" "$COMPILER"
fi

# ---------------------------------------------------------------- L2
if (( LEVEL >= 2 )); then
  gate "L2 negative tests" \
    python3 unit-tests/run_negative_tests.py --compiler "$COMPILER" --jobs "$JOBS"
  gate "L2 unit --current" \
    "${UNIT_ENV[@]}" python3 unit-tests/unittests.py --current --compiler "$COMPILER" --parallel "$JOBS"
  gate "L2 compare vs baseline" \
    "${UNIT_ENV[@]}" python3 unit-tests/unittests.py --compare --parallel "$JOBS"
  if (( UPDATE_MANIFEST )); then
    gate "L2 hex manifest store" python3 unit-tests/hex_manifest.py store
  elif [[ -f "$ROOT/unit-tests/manifest/hex_manifest.txt" ]]; then
    gate "L2 hex manifest A/B (0 drift)" python3 unit-tests/hex_manifest.py diff
  else
    skip "L2 hex manifest (none yet — run with --update-manifest to start A/B tracking)"
  fi
fi

# ---------------------------------------------------------------- escalation
# Landing discipline (user rule 2026-10-06): a green UNPINNED level-2 run
# continues to L3 automatically — "if L2 passes, L3 should run". Explicit
# --level 2 stays an iteration ceiling, --quick stays quick, and auto-suggested
# 0/1 runs (docs-only / fast iteration) do not escalate. The already-run L2
# unit/compare gates ran with the quiet env; an escalated pass therefore
# validates without archiving — a publishing landing run is an explicit
# --level 3.
if (( LEVEL == 2 && FAILED == 0 && !LEVEL_EXPLICIT )); then
  echo ""
  echo "level 2 passed — escalating to L3 (landing gate; no explicit --level)"
  LEVEL=3
fi

# ---------------------------------------------------------------- L3
if (( LEVEL >= 3 )); then
  if (( SEL_PIC32 && !RAN_ESP32 )) || (( SEL_ESP32 && !RAN_PIC32 )); then
    if (( SEL_PIC32 && !RAN_ESP32 )); then
      gate "L3 esp32 qemu (second suite)" \
        python3 sims/run_esp32_regression.py --run --compiler "$COMPILER"
    else
      gate "L3 pic32 qemu (second suite)" \
        python3 sims/run_pic32_regression.py --run --compiler "$COMPILER"
    fi
  else
    skip "L3 second qemu suite (L1 already ran both)"
  fi
  if (( WITH_SAN )); then
    # ALWAYS a clean build: the .obj-x86-64-san dirs can silently carry
    # objects from before a header change (fix_e31b4499's mixed-object
    # lesson — a stale san tree segfaults in pf_token_get on every sample,
    # and the binary's mtime says nothing about the objects inside it).
    # SAN uses isolated obj dirs, so the primary build is untouched.
    gate "L3 sanitizer build (clean)" make -C src SAN=1 ARCH=x86-64 clean
    gate "L3 sanitizer build" make -C src SAN=1 ARCH=x86-64
    # pin the -san binary for the corpus gate below (same mid-run-rebuild
    # immunity as the primary compiler snapshot)
    if cp "$ROOT/bin/jalv3-x86-64-san" "$GATE_SNAP_DIR/jalv3-x86-64-san" \
        2>/dev/null; then
      SAN_SNAP="$GATE_SNAP_DIR/jalv3-x86-64-san"
      echo "gates: sanitizer compiler pinned to snapshot"
    fi
    gate "L3 sanitizer corpus" \
      env JALV2_NO_REPORT_ARCHIVE=1 JALV2_NO_CHANGELOG_ENTRY=1 \
        ASAN_OPTIONS=detect_leaks=0 JALV2_UNIT_TEST_COMPILE_TIMEOUT=600 \
        python3 unit-tests/unittests.py --current \
        --compiler "${SAN_SNAP:-$ROOT/bin/jalv3-x86-64-san}" \
        --parallel "$JOBS"
  else
    skip "L3 sanitizer corpus (--no-san)"
  fi
  gate "L3 sample-kit sweep" sample_kit_sweep
fi

print_summary
exit "$FAILED"

#!/usr/bin/env bash
#
# Plan-diff gate (Step 2): compile a pinned classic subset with
# -dump-plan, sort the rows, and diff against the committed baseline.
# The source line is stripped (a non-key annotation, A15) and a differing
# duplicate row fails before the diff (A18/C6).  `--update` refreshes the
# baseline (the run_gates --update-plan-baseline path).
#
# Usage: run_plan_gate.sh [compiler] [--update]
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)

# GATE MODE (T3): these baselines are the ENGINE-OFF behavior; pin it
# explicitly so an ambient JALV2_FAT_POINTERS (e.g. after the default flip)
# cannot change them.  Mode-specific arms set their own value per command.
JALV2_FAT_POINTERS=0
export JALV2_FAT_POINTERS

CC="bin/beta/jalv3-x86-64"
UPDATE=0
for arg in "$@"; do
    case "$arg" in
        --update) UPDATE=1 ;;
        *)        CC="$arg" ;;
    esac
done
LIBS='demo_lib;third_party/jallib/lib;tests;sims/shared'
BASE="$ROOT/unit-tests/plan-baselines/classic-pic.txt"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/plan-gate.XXXXXX") || exit 1
ALL="$WORK/all.rows"
: > "$ALL"

run_one() {
    prog="$1"
    log="$WORK/$(printf '%s' "$prog" | tr '/' '_').plan"
    if ! ( cd "$ROOT" && JALV2_ACCESS_VERIFY=1 \
        "$CC" -s "$LIBS" -dump-plan "$log" \
        -no-codfile -hex "$WORK/o.hex" "$prog" >/dev/null 2>&1 ); then
        echo "PLAN GATE FAIL: compile $prog"
        return 1
    fi
    if [ -s "$log" ]; then
        # E2/A15: the source line is a non-key annotation; the baseline
        # key is program,generation,site,var,ordinal,kind,facts
        grep -v '^#' "$log" | cut -d, -f1-14 >> "$ALL"
    fi
    return 0
}

fail=0
for prog in \
    demo_sample/12f508_blink.jal \
    demo_sample/16f887_bit_arrays.jal \
    demo_sample/16f887_stopwatch_probe.jal \
    demo_sample/16f18857_pointers_1.jal \
    demo_sample/16f18857_pointers_2.jal \
    sims/gpsim/18f4620_gpsim_pointer_array_200.jal \
    sims/gpsim/18f4620_gpsim_ptr_store_inout.jal \
    sims/gpsim/18f4620_gpsim_string_heap_over_bank.jal \
    sims/picsim/16f18857_picsim_pointer_array_200.jal
do
    run_one "$prog" || fail=1
done

# the fat witness needs its gate env pinned
log="$WORK/fat.plan"
if ! ( cd "$ROOT" && JALV2_ACCESS_VERIFY=1 \
    JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
    "$CC" -s "$LIBS" -dump-plan "$log" -no-codfile -hex "$WORK/o.hex" \
    sims/picsim/16f18857_picsim_fat_pointer.jal >/dev/null 2>&1 ); then
    echo "PLAN GATE FAIL: compile sims/picsim/16f18857_picsim_fat_pointer.jal"
    fail=1
fi
[ -s "$log" ] && grep -v '^#' "$log" | cut -d, -f1-14 >> "$ALL"

# A18/C6: a differing duplicate row is fatal; identical duplicates collapse
if ! python3 -I "$ROOT/unit-tests/plan_dupcheck.py" --log "$ALL"; then
    fail=1
fi
if [ "$UPDATE" = 1 ] && [ "$fail" != 0 ]; then
    echo "plan gate: refusing to update the baseline (compile or duplicate failure)" >&2
    rm -rf "$WORK"
    exit 1
fi

sort -u "$ALL" > "$WORK/now.txt"
if [ "$UPDATE" = 1 ]; then
    mkdir -p "$(dirname "$BASE")"
    cp "$WORK/now.txt" "$BASE"
    echo "plan baseline updated: $BASE ($(wc -l < "$BASE") rows)"
    rm -rf "$WORK"
    exit "$fail"
fi
if [ ! -f "$BASE" ]; then
    echo "PLAN GATE FAIL: missing baseline $BASE (run --update)"
    rm -rf "$WORK"
    exit 1
fi
if diff -u "$BASE" "$WORK/now.txt" > "$WORK/diff.txt"; then
    echo "PLAN GATE PASS ($(wc -l < "$WORK/now.txt") unique rows)"
else
    echo "PLAN GATE FAIL (JAL-PLAN-DIFF)"
    head -40 "$WORK/diff.txt"
    fail=1
fi
rm -rf "$WORK"
exit "$fail"

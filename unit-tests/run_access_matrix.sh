#!/usr/bin/env bash
#
# Build the access-plan kind x site matrix over the deterministic
# classic-PIC corpus.  One recorder log per compile (A9), merged after
# the run; the merged log is only taken from programs that compiled
# cleanly, so partial rows never enter the matrix.
#
# Modes:
#   (none)     rebuild the committed matrix and report
#   --check    A12: rebuild into a temp dir, diff against the committed
#              matrix (JAL-MATRIX-DIFF), assert the pinned recipe-row
#              coverage (JAL-MATRIX-COVER), and prove the layout trial
#              selects the window layout on its witness (JAL-MATRIX-WIN)
#   --update   refresh the committed matrix and coverage baseline
#
# Usage: run_access_matrix.sh [compiler] [--check|--update]
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COMPILER="$ROOT/bin/beta/jalv3-x86-64"

# GATE MODE (T3): these baselines are the ENGINE-OFF behavior; pin it
# explicitly so an ambient JALV2_FAT_POINTERS (e.g. after the default flip)
# cannot change them.  Mode-specific arms set their own value per command.
JALV2_FAT_POINTERS=0
export JALV2_FAT_POINTERS

MODE=run
for arg in "$@"; do
    case "$arg" in
        --check)  MODE=check ;;
        --update) MODE=update ;;
        *)        COMPILER="$arg" ;;
    esac
done
LIBS='demo_lib;third_party/jallib/lib;tests'
# the coverage pass compiles more of the corpus than the matrix does:
# sims/shared resolves the remaining pointer samples
COV_LIBS="$LIBS;sims/shared"
OUT_DIR="$ROOT/unit-tests/plan-matrix"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/access-matrix.XXXXXX") || exit 1
MERGED="$WORK/all.log"
COVERAGE="$WORK/coverage.txt"
FIXTURES="$ROOT/unit-tests/access_fixtures.txt"
case "$MODE" in
    check) OUT="$WORK/out" ;;
    *)     OUT="$OUT_DIR" ;;
esac
mkdir -p "$OUT" || exit 1
: > "$MERGED"
: > "$COVERAGE"

FAILED="$WORK/failed.txt"
ok=0
fail=0
fixture_ok=0
: > "$FAILED"

run_one() {
    prog="$1"
    log="$WORK/$(printf '%s' "$prog" | tr '/' '_').log"
    if ( cd "$ROOT" && \
        JALV2_ACCESS_RECORD=1 JALV2_ACCESS_RECORD_OUT="$log" \
        "$COMPILER" -s "$LIBS" -no-codfile -hex "$WORK/out.hex" "$prog" \
        >/dev/null 2>&1 ); then
        ok=$((ok + 1))
        if [ -s "$log" ]; then
            cat "$log" >> "$MERGED"
        fi
    else
        fail=$((fail + 1))
        printf '%s\n' "$prog" >> "$FAILED"
    fi
}

corpus() {
    ( cd "$ROOT" && find sims/gpsim sims/picsim demo_sample -name '*.jal' \
        2>/dev/null | sort | grep -viE 'esp32|pic32|32mx|32mz|fuzz|negative' )
}

if [ -f "$FIXTURES" ]; then
    while IFS= read -r prog; do
        [ -n "$prog" ] || continue
        before=$ok
        run_one "$prog"
        if [ "$ok" -gt "$before" ]; then
            fixture_ok=$((fixture_ok + 1))
        fi
    done < "$FIXTURES"
fi

while IFS= read -r prog; do
    [ -n "$prog" ] || continue
    run_one "$prog"
done < <(corpus)

cp "$FAILED" "$OUT/classic-pic.failed.txt"
python3 -I "$ROOT/unit-tests/access_matrix.py" \
    --log "$MERGED" --out "$OUT/classic-pic.md" || exit 1

rows=$(grep -vc '^#' "$MERGED" 2>/dev/null || true)
echo "programs: ok=$ok fail=$fail; fixtures=$fixture_ok; recorded rows=$rows"

# ---- A12 coverage: union the per-compile recipe-row bitmaps ----------------
cov_compile() {  # cov_compile <prog> [fat]
    local prog="$1" fat="${2:-}" err="$WORK/cov.err"

    ( cd "$ROOT" || exit 1
      if [ -n "$fat" ]; then
          JALV2_ACCESS_COVERAGE=1 JALV2_FAT_POINTERS=1 JALV2_FAT_FORCE=1 \
              "$COMPILER" -s "$COV_LIBS" -no-codfile -hex "$WORK/cov.hex" \
              "$prog" >/dev/null 2>"$err"
      else
          JALV2_ACCESS_COVERAGE=1 \
              "$COMPILER" -s "$COV_LIBS" -no-codfile -hex "$WORK/cov.hex" \
              "$prog" >/dev/null 2>"$err"
      fi ) || return 0
    grep 'jal access coverage' "$err" >> "$COVERAGE"
}

{
    corpus
    ( cd "$ROOT" && find unit-tests/corpus_overlays -name '*.jal' \
        2>/dev/null | sort )
} > "$WORK/cov_corpus.txt"
while IFS= read -r prog; do
    [ -n "$prog" ] || continue
    cov_compile "$prog"
done < "$WORK/cov_corpus.txt"

# the FAT family is not enabled in the matrix corpus; its witnesses are
# compiled here with the gate env pinned (A15)
cov_compile sims/picsim/16f18857_picsim_fat_pointer.jal fat
cov_compile sims/gpsim/18f4620_gpsim_pointer_array_200.jal fat
# G3 slot binds: the scalar-pointee runtime offset and the array-pointee
# control are both FAT_BIND under the engine (design D4); NOT_FAT_BIND is
# now shadowed by FAT_BIND (pic_fat_bind_forced only consults the recipe
# with the engine on), so its row stays unexercised.
cov_compile sims/picsim/16f18857_picsim_pointer_slot_runtime_ofs.jal fat
cov_compile sims/gpsim/16f887_gpsim_pointer_slot_runtime_ofs.jal fat
cov_compile sims/picsim/16f18857_picsim_pointer_slot_array_bind.jal fat

COVERAGE_ARGS=(--coverage "$COVERAGE"
    --coverage-baseline "$OUT_DIR/coverage.txt")
if [ "$MODE" = update ]; then
    COVERAGE_ARGS+=(--update-coverage)
fi
python3 -I "$ROOT/unit-tests/access_matrix.py" \
    --log "$MERGED" --out "$OUT/classic-pic.md" \
    "${COVERAGE_ARGS[@]}" || exit 1
echo "matrix: $OUT/classic-pic.md"

# ---- A12: the layout trial must select the window layout on its witness ----
if [ "$MODE" != run ]; then
    WIN_PROG=sims/gpsim/18f4620_gpsim_ptr_store_inout.jal
    plain="$WORK/win_plain.hex"
    notrial="$WORK/win_notrial.hex"
    if ! ( cd "$ROOT" && "$COMPILER" -s "$COV_LIBS" -no-codfile \
        -hex "$plain" "$WIN_PROG" >/dev/null 2>&1 ) \
      || ! ( cd "$ROOT" && JALV2_NO_LAYOUT_TRIALS=1 "$COMPILER" \
        -s "$COV_LIBS" -no-codfile -hex "$notrial" "$WIN_PROG" \
        >/dev/null 2>&1 ); then
        echo "JAL-MATRIX-WIN: cannot compile $WIN_PROG" >&2
        exit 1
    fi
    if cmp -s "$plain" "$notrial"; then
        echo "JAL-MATRIX-WIN: the trial never selected win=1 on $WIN_PROG" >&2
        exit 1
    fi
    echo "layout trial: win=1 selected on $WIN_PROG (image differs)"
fi

# ---- the committed artifacts ----------------------------------------------
# run/update write straight into $OUT_DIR; check compares in a temp dir
if [ "$MODE" = check ]; then
    if diff -u "$OUT_DIR/classic-pic.md" "$OUT/classic-pic.md" \
        > "$WORK/matrix.diff"; then
        echo "matrix: 0 drift"
    else
        echo "JAL-MATRIX-DIFF: the matrix changed; review then run --update" >&2
        head -40 "$WORK/matrix.diff" >&2
        exit 1
    fi
fi

rm -rf "$WORK"

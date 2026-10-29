#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# (jalv3compiler simulator test harness)

# Compile a .jal with baseline or current JALv2, run gpsim, validate GPR snapshot.
#
# Usage:
#   ./sims/jal_gpsim_validate.sh [options] path/to/file.jal
#
# Options:
#   --baseline          Use bin/old_versions/jalv25r9-x86-64 (original jalv2)
#   --current           Use ./bin/jalv2-original or ./bin/jalv3-x86-64 (default)
#   --compiler PATH     Use this compiler binary
#   -s DIR              First library search path (default: lib); sims/ is always
#                       appended: compiler gets -s DIR;<repo>/tests
#   -p NAME             gpsim processor (default: infer from first device-shaped include
#                       Nf… in the .jal, or inside a same-dir fragment from the first include)
#   -f HZ               Override clock frequency for gpsim (else @gpsim_validate / pragma)
#   -n STEPS            Override instruction steps before dump
#   --expect-dword N    Override value for expect dword_le v__d in the block
#   --no-validate       Only compile + gpsim smoke (no RAM checks)
#   --quiet             Less console output (full compile/gpsim dump only on failure).
#                       Also honored if env JAL_GPSIM_QUIET=1 is set.
#
# Optional in @gpsim_validate:
#   -- compiler_args: -task 2
#   -- skip_gpsim: 1   (after compile + check-config [+ asm symbols]: exit OK without gpsim RAM checks)
#   -- expect_hw_stkptr_le_compiler_depth: 1   (after RAM checks: gpsim max STKPTR vs compiler static depth)
#   Extra tokens are appended after -s … and before the .jal path (see sims/gpsim_validate_contract.txt).
#
# Expectations (default — RAM validation on):
#   Requires @gpsim_validate begin … end with at least one expect line (see
#   sims/gpsim_validate_contract.txt). Fails before compile if missing.
#   Use --no-validate for compile + gpsim only (no contract required).
#
# Compiler warnings: any non-zero warning count in the summary line (N errors, M warnings)
# fails the script after a successful exit code (warnings-as-errors for regression hygiene).
#
# Requires: gpsim, python3, compiler binary. Compile passes one -s argument with
#   semicolon-separated dirs (e.g. lib;<repo>/tests) so device files and regression.jal resolve.
#   .asm / .hex are written under a per-run temp directory (safe for parallel suite runs).
#
# Full guide (install, regression suite, writing tests): instructions_gpsim_test.txt in repo root.
#
# Environment:
#   JAL_GPSIM_GPSIM_OPTS — extra gpsim arguments (space-separated), inserted after -i.
#     If unset, defaults to -S disable (skip source attachment; see gpsim --help).
#     Export JAL_GPSIM_GPSIM_OPTS="" to pass no extra args (re-enable source attach).
#   JAL_GPSIM_ALLOW_MOVLB255 — if 1, skip the PIC18 .asm guard for movlb 255 / 0xff / H'FF'
#.
#   Exit 139 = 128+SIGSEGV: often gpsim or a plugin; -S disable can reduce crashes;
#     Ubuntu Launchpad #150321 (GUI NULL deref); SourceForge gpsim bugs (COD/stack,
#     CLI). Upgrade gpsim if crashes persist.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ENGINE="$ROOT/sims/gpsim_validate_engine.py"
LIBDIR="third_party/jallib/lib;demo_lib"   # pristine jallib keeps precedence; lib only
#                      # contributes files absent from lib (strings_dynamic
#                      # moved there 2026-09-23, see fix_lib_new_shadow.md)
COMPILER_MODE="current"
COMPILER_CUSTOM=""
PROCESSOR=""
FREQ_CLI=""
STEPS_CLI=""
EXPECT_DWORD=""
NO_VALIDATE=0
QUIET=0
JAL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --baseline) COMPILER_MODE="baseline"; shift ;;
    --current)  COMPILER_MODE="current"; shift ;;
    --compiler) COMPILER_CUSTOM="$2"; shift 2 ;;
    -s)         LIBDIR="$2"; shift 2 ;;
    -p)         PROCESSOR="$2"; shift 2 ;;
    -f)         FREQ_CLI="$2"; shift 2 ;;
    -n)         STEPS_CLI="$2"; shift 2 ;;
    --expect-dword) EXPECT_DWORD="$2"; shift 2 ;;
    --no-validate) NO_VALIDATE=1; shift ;;
    --quiet)    QUIET=1; shift ;;
    -h|--help)
      sed -n '1,32p' "$0"
      exit 0
      ;;
    *)
      JAL="$1"
      shift
      ;;
  esac
done

if [[ "${JAL_GPSIM_QUIET:-0}" == "1" ]] || [[ "${JAL_GPSIM_QUIET:-}" == "true" ]]; then
  QUIET=1
fi

if [[ -z "$JAL" ]]; then
  echo "Usage: $0 [options] path/to/file.jal" >&2
  exit 2
fi

if [[ ! -f "$JAL" ]]; then
  echo "error: not a file: $JAL" >&2
  exit 2
fi

if [[ ! -f "$ENGINE" ]]; then
  echo "error: missing $ENGINE" >&2
  exit 2
fi

JAL_ABS="$(cd "$(dirname "$JAL")" && pwd)/$(basename "$JAL")"

if [[ -n "$COMPILER_CUSTOM" ]]; then
  COMPILER="$COMPILER_CUSTOM"
elif [[ "$COMPILER_MODE" == "baseline" ]]; then
  COMPILER="$ROOT/bin/old_versions/jalv25r9-x86-64"
else
  if [[ -f "$ROOT/bin/jalv2-original" ]]; then
    COMPILER="$ROOT/bin/jalv2-original"
  else
    COMPILER="$ROOT/bin/jalv3-x86-64"
  fi
fi

if [[ ! -f "$COMPILER" ]]; then
  echo "error: compiler not found: $COMPILER" >&2
  exit 2
fi

# Include path: repo demo_lib/ + sims/ (COUNT() is canonical; no COUNT/_LENGTH shadow tree).
GP_S="${LIBDIR};${ROOT}/sims/shared"

if ! command -v gpsim >/dev/null 2>&1; then
  echo "error: gpsim not on PATH" >&2
  exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "error: python3 required" >&2
  exit 2
fi

# First include line matching PIC device file pattern (e.g. 18f4620, 12f1840), or the first
# such include inside a sibling "$(dirname jal)/NAME.jal" when NAME is a local fragment.
infer_include() {
  local core_dir inc line frag frag_line x
  core_dir="${JAL_ABS%/*}"
  while IFS= read -r line; do
    line="${line%%--*}"
    [[ "$line" =~ ^[[:space:]]*include[[:space:]]+([a-zA-Z0-9_]+) ]] || continue
    inc="${BASH_REMATCH[1],,}"
    if [[ "$inc" =~ ^[0-9]+f[0-9a-z]+$ ]]; then
      echo "$inc"
      return 0
    fi
    frag="${core_dir}/${inc}.jal"
    if [[ -f "$frag" ]]; then
      while IFS= read -r frag_line; do
        frag_line="${frag_line%%--*}"
        [[ "$frag_line" =~ ^[[:space:]]*include[[:space:]]+([a-zA-Z0-9_]+) ]] || continue
        x="${BASH_REMATCH[1],,}"
        if [[ "$x" =~ ^[0-9]+f[0-9a-z]+$ ]]; then
          echo "$x"
          return 0
        fi
      done < <(grep -iE '^[[:space:]]*include[[:space:]]+[a-zA-Z0-9_]+' "$frag" || true)
    fi
  done < <(grep -iE '^[[:space:]]*include[[:space:]]+[a-zA-Z0-9_]+' "$JAL_ABS" || true)
  echo ""
}

infer_frequency_bash() {
  grep -iE '^[[:space:]]*pragma[[:space:]]+target[[:space:]]+clock' "$JAL_ABS" | head -1 \
    | sed 's/--.*//' | awk '{print $NF}' | tr -d '_'
}

if [[ -z "$PROCESSOR" ]]; then
  inc="$(infer_include)"
  if [[ -z "$inc" ]]; then
    echo "error: could not infer device include; use -p p18f4620" >&2
    exit 2
  fi
  PROCESSOR="p${inc}"
fi

# Metadata from @gpsim_validate + pragma (JSON one line)
META_JSON="$(python3 "$ENGINE" meta "$JAL_ABS")"

mapfile -t COMPILER_EXTRA < <(python3 -c 'import json,sys; m=json.load(sys.stdin)
for a in (m.get("compiler_args") or []):
    print(a)' <<<"$META_JSON") || true

if [[ "$NO_VALIDATE" -eq 0 ]]; then
  if ! python3 "$ENGINE" check-contract "$JAL_ABS"; then
    exit 1
  fi
fi

pick_steps() {
  if [[ -n "$STEPS_CLI" ]]; then
    echo "$STEPS_CLI"
    return
  fi
  python3 -c 'import json,sys; m=json.load(sys.stdin); s=m.get("steps"); print(s if s else "")' <<<"$META_JSON" | head -1
}

pick_freq() {
  if [[ -n "$FREQ_CLI" ]]; then
    echo "$FREQ_CLI"
    return
  fi
  python3 -c 'import json,sys; m=json.load(sys.stdin); r=m.get("frequency_hz_resolved"); print(r if r is not None else "")' <<<"$META_JSON" | head -1
}

STEPS="$(pick_steps)"
if [[ -z "$STEPS" ]]; then
  STEPS=120000
fi

FREQ="$(pick_freq)"
if [[ -z "$FREQ" ]]; then
  FREQ="$(infer_frequency_bash)"
fi
if [[ -z "$FREQ" ]]; then
  FREQ=40000000
fi

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jal_gpsim_build.XXXXXX")"
JAL_BASENAME="$(basename "$JAL_ABS" .jal)"
ASM_ABS="$BUILD_DIR/${JAL_BASENAME}.asm"
HEX_ABS="$BUILD_DIR/${JAL_BASENAME}.hex"
GPSIM_OUT=""
VALIDATE_LOG=""
_cleanup_jgv() {
  rm -f "$GPSIM_OUT"
  [[ -n "${VALIDATE_LOG:-}" ]] && rm -f "$VALIDATE_LOG"
  rm -rf "$BUILD_DIR"
}
trap '_cleanup_jgv' EXIT

if [[ "$QUIET" -eq 0 ]]; then
  echo "=== jal_gpsim_validate ==="
  echo "jal:       $JAL_ABS"
  echo "compiler:  $COMPILER"
  echo "lib:       ${GP_S}"
  echo "processor: $PROCESSOR"
  echo "frequency: $FREQ Hz"
  echo "steps:     $STEPS"
  echo "meta:      $META_JSON"
  echo "build dir: $BUILD_DIR"
  if [[ ${#COMPILER_EXTRA[@]} -gt 0 ]]; then
    echo "compiler extra args: ${COMPILER_EXTRA[*]}"
  fi
  echo ""
else
  echo "jal_gpsim_validate (quiet): $JAL_ABS"
fi

if [[ "$QUIET" -eq 0 ]]; then
  echo "--- compile ---"
fi
set +e
OUT="$("$COMPILER" -s "${GP_S}" -no-codfile -asm "$ASM_ABS" -hex "$HEX_ABS" "${COMPILER_EXTRA[@]}" "$JAL_ABS" 2>&1)"
RC=$?
set -e
if [[ "$QUIET" -eq 0 ]]; then
  echo "$OUT"
else
  if [[ "$RC" -ne 0 ]]; then
    echo "--- compile output (failure) ---" >&2
    echo "$OUT" >&2
  fi
fi
if [[ "$RC" -ne 0 ]]; then
  echo "error: compiler exited $RC" >&2
  exit "$RC"
fi

# Treat any compiler warning as failure (JALv2 still exits 0 if only warnings),
# unless @gpsim_validate sets allow_compiler_warnings: 1 (documented in contract).
ALLOW_COMPILER_WARNINGS="$(python3 -c 'import json,sys; print(1 if json.load(sys.stdin).get("allow_compiler_warnings") else 0)' <<<"$META_JSON")"
if [[ "$ALLOW_COMPILER_WARNINGS" != "1" ]]; then
  if ! echo "$OUT" | python3 -c "import re, sys
m = re.search(r'(\d+)\s+errors,\s*(\d+)\s+warnings', sys.stdin.read())
sys.exit(0 if (m and int(m.group(2)) == 0) else 1)"; then
    echo "error: compiler reported one or more warnings (treated as errors)" >&2
    echo "$OUT" >&2
    exit 1
  fi
fi

if [[ ! -f "$HEX_ABS" ]]; then
  echo "error: expected HEX not found: $HEX_ABS" >&2
  exit 1
fi

if [[ -f "$ASM_ABS" ]]; then
  if ! python3 "$ENGINE" check-config "$JAL_ABS" "$ASM_ABS" "$HEX_ABS"; then
    exit 1
  fi
  # PIC18: movlb 255 / 0xff / H'FF' is 0xffff/256 from VARIABLE_BASE_UNKNOWN — bogus BSR;
  # gpsim 0.31.x can SIGSEGV (exit 139) on the following banked access. Fail before gpsim.
  _JGV_PROC_LC="$(printf '%s' "$PROCESSOR" | tr '[:upper:]' '[:lower:]')"
  if [[ "$_JGV_PROC_LC" == p18* && "${JAL_GPSIM_ALLOW_MOVLB255:-}" != "1" ]]; then
    if grep -Eqi '^[[:space:]]*movlb[[:space:]]+(255|0xff)(\>|[[:space:]])' "$ASM_ABS" \
        || grep -Eqi "^[[:space:]]*movlb[[:space:]]+H'[Ff][Ff]'" "$ASM_ABS"; then
      echo "error: $ASM_ABS: PIC18 movlb 255 / movlb 0xff / movlb H'FF' (typically VARIABLE_BASE_UNKNOWN/256) — gpsim may SIGSEGV (exit 139). See. To skip: JAL_GPSIM_ALLOW_MOVLB255=1" >&2
      grep -nEi '^[[:space:]]*movlb[[:space:]]+(255|0xff)(\>|[[:space:]])' "$ASM_ABS" | head -25 >&2 || true
      grep -nEi "^[[:space:]]*movlb[[:space:]]+H'[Ff][Ff]'" "$ASM_ABS" | head -25 >&2 || true
      exit 1
    fi
  fi
  unset _JGV_PROC_LC
fi

if [[ "$NO_VALIDATE" -eq 0 ]]; then
  if [[ ! -f "$ASM_ABS" ]]; then
    echo "error: expected .asm for RAM validation: $ASM_ABS" >&2
    exit 1
  fi
  if ! python3 "$ENGINE" check-asm-symbols "$JAL_ABS" "$ASM_ABS"; then
    exit 1
  fi
fi

SKIP_GPSIM="$(python3 -c 'import json,sys; print(1 if json.load(sys.stdin).get("skip_gpsim") else 0)' <<<"$META_JSON")"
if [[ "$SKIP_GPSIM" == "1" ]]; then
  if [[ "$NO_VALIDATE" -eq 1 ]]; then
    if [[ "$QUIET" -eq 0 ]]; then
      echo ""
      echo "OK: skip_gpsim + --no-validate (compile + check-config only)"
    else
      echo "OK: skip_gpsim (--no-validate)"
    fi
    exit 0
  fi
  if [[ "$QUIET" -eq 0 ]]; then
    echo ""
    echo "OK: skip_gpsim — asm symbols OK; skipping gpsim (no CPU model in this gpsim / contract opt-out)"
  else
    echo "OK: skip_gpsim (compile + check-config + asm symbols)"
  fi
  exit 0
fi

if [[ "$QUIET" -eq 0 ]]; then
  echo ""
  echo "--- gpsim (batch) ---"
fi
GPSIM_OUT="$(mktemp)"

_JGV_GPSIM_EXTRA=()
if [[ -n "${JAL_GPSIM_GPSIM_OPTS+x}" ]]; then
  read -r -a _JGV_GPSIM_EXTRA <<< "${JAL_GPSIM_GPSIM_OPTS}"
else
  _JGV_GPSIM_EXTRA=(-S disable)
fi

set +e
if [[ "$QUIET" -eq 0 ]]; then
  gpsim -i "${_JGV_GPSIM_EXTRA[@]}" -p "$PROCESSOR" "$HEX_ABS" 2>&1 <<EOF | tee "$GPSIM_OUT"
frequency $FREQ
step $STEPS
dump r
quit
EOF
  GRC=${PIPESTATUS[0]}
else
  gpsim -i "${_JGV_GPSIM_EXTRA[@]}" -p "$PROCESSOR" "$HEX_ABS" >"$GPSIM_OUT" 2>&1 <<EOF
frequency $FREQ
step $STEPS
dump r
quit
EOF
  GRC=$?
fi
set -e
if [[ "$GRC" -ne 0 ]]; then
  echo "error: gpsim exited $GRC" >&2
  if [[ "$QUIET" -ne 0 ]]; then
    echo "--- gpsim output (failure) ---" >&2
    tail -n 80 "$GPSIM_OUT" >&2 || true
  fi
  exit "$GRC"
fi

if [[ "$NO_VALIDATE" -eq 1 ]]; then
  if [[ "$QUIET" -eq 0 ]]; then
    echo ""
    echo "OK: compile + gpsim finished (--no-validate)"
  else
    echo "OK: compile + gpsim (--no-validate)"
  fi
  exit 0
fi

if [[ ! -f "$ASM_ABS" ]]; then
  echo "error: expected .asm for RAM validation: $ASM_ABS" >&2
  exit 1
fi

VALIDATE_LOG="$(mktemp)"

set +e
python3 "$ENGINE" validate "$JAL_ABS" "$ASM_ABS" "$GPSIM_OUT" "${EXPECT_DWORD:-}" >"$VALIDATE_LOG" 2>&1
VALIDATE_RC=$?
set -e

if [[ "$QUIET" -eq 0 ]]; then
  cat "$VALIDATE_LOG"
  echo ""
  if [[ "$VALIDATE_RC" -eq 0 ]]; then
    echo "=== all checks passed ==="
  else
    echo "=== validation failed ===" >&2
  fi
else
  if [[ "$VALIDATE_RC" -ne 0 ]]; then
    echo "--- RAM validation (failure) ---" >&2
    cat "$VALIDATE_LOG" >&2
    echo "=== validation failed ===" >&2
  else
    echo "PASS: RAM validation"
  fi
fi

CHK_HW_STK="$(python3 -c 'import json,sys; print(1 if json.load(sys.stdin).get("expect_hw_stkptr_le_compiler_depth") else 0)' <<<"$META_JSON")"
if [[ "$VALIDATE_RC" -eq 0 && "$CHK_HW_STK" == "1" ]]; then
  HW_SCRIPT="$ROOT/sims/gpsim_hw_return_stack_max.py"
  if [[ ! -f "$HW_SCRIPT" ]]; then
    echo "error: missing $HW_SCRIPT" >&2
    exit 1
  fi
  STK_STEPS="$(python3 -c 'import json,sys; m=json.load(sys.stdin); s=m.get("hw_stkptr_steps"); print(int(s) if isinstance(s,int) else "")' <<<"$META_JSON")"
  if [[ -z "$STK_STEPS" ]]; then
    STK_STEPS="$STEPS"
  fi
  if ! COMPILER_DEPTH="$(echo "$OUT" | python3 "$ENGINE" compiler-hw-depth)"; then
    echo "error: expect_hw_stkptr_le_compiler_depth set but could not parse compiler depth" >&2
    exit 1
  fi
  set +e
  HW_JSON="$(python3 "$HW_SCRIPT" -p "$PROCESSOR" -f "$FREQ" -n "$STK_STEPS" "$HEX_ABS" --json 2>&1)"
  HW_RC=$?
  set -e
  if [[ "$HW_RC" -ne 0 ]]; then
    echo "error: gpsim_hw_return_stack_max.py failed (exit $HW_RC)" >&2
    echo "$HW_JSON" >&2
    exit "$HW_RC"
  fi
  MAX_STK="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["max_stkptr"])' <<<"$HW_JSON")"
  if [[ "$MAX_STK" -gt "$COMPILER_DEPTH" ]]; then
    echo "error: gpsim max STKPTR ($MAX_STK) > compiler Hardware stack depth ($COMPILER_DEPTH)" >&2
    echo "  (static bound should be conservative: dynamic max along this run should be <= compiler depth)" >&2
    exit 1
  fi
  if [[ "$QUIET" -eq 0 ]]; then
    echo "OK: hardware return stack (gpsim max STKPTR=$MAX_STK <= compiler depth $COMPILER_DEPTH, steps=$STK_STEPS)"
  else
    echo "PASS: hardware return stack (gpsim max STKPTR=$MAX_STK <= compiler depth $COMPILER_DEPTH, steps=$STK_STEPS)"
  fi
fi

exit "$VALIDATE_RC"

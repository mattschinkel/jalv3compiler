#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Compile four unit samples that historically triggered pic_opt_verify_skip
# "skip error" false positives (btfsc → movlb same file byte). Fails if any
# compile errors or if compiler output contains "skip error".
#
# Environment:
#   JALV2  — path to jalv2 executable (default: first of bin/jalv3-x86-64,
#            bin/jalv2-original, bin/jalv2 under repo root).
#
# Exit codes: 0 success, 1 compile/log failure or skip error in output,
#             2 incomplete sample set (some paths missing), 3 no compiler found.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

resolve_jalv2() {
  if [[ -n "${JALV2:-}" ]]; then
    if [[ -x "$JALV2" ]]; then
      echo "$JALV2"
      return 0
    fi
    echo "JALV2 is set but not an executable file: $JALV2" >&2
    return 1
  fi
  for c in "$ROOT/bin/jalv3-x86-64" "$ROOT/bin/jalv2-original" "$ROOT/bin/jalv2"; do
    if [[ -x "$c" ]]; then
      echo "$c"
      return 0
    fi
  done
  echo "No jalv2 found. Build with: make -C src ARCH=x86-64" >&2
  echo "Or set JALV2 to your compiler path." >&2
  return 1
}

JAL="$(resolve_jalv2)" || exit 3

# Same four files documented in fixes/fix_pic_opt_verify_skip_movlb_same_file_false_positive.md
SAMPLES=(
  "third_party/jallib/samples/16f1825_dcf77_decoder_serial.jal"
  "third_party/jallib/samples/16f1825_philips_rc_decoder_serial.jal"
  "third_party/jallib/samples/16f1825_philips_rc_decoder_ssd1306.jal"
  "third_party/jallib/samples/16f18857_philips_rc_decoder_serial.jal"
)

present=0
missing=()
for rel in "${SAMPLES[@]}"; do
  if [[ -f "$ROOT/$rel" ]]; then
    present=$((present + 1))
  else
    missing+=("$rel")
  fi
done

if [[ "$present" -eq 0 ]]; then
  echo "No regression samples under third_party/jallib/samples/ (minimal checkout). Nothing to run." >&2
  echo "Expected files:" >&2
  printf '  %s\n' "${SAMPLES[@]}" >&2
  exit 0
fi

if [[ "${#missing[@]}" -gt 0 ]]; then
  echo "Incomplete sample set: ${#missing[@]} file(s) missing (have $present/4)." >&2
  printf '  missing: %s\n' "${missing[@]}" >&2
  exit 2
fi

fail=0
# The jallib tree moved under third_party/ (the old bare `-s lib` pointed
# at a directory that no longer exists, so every sample "failed" on its
# own device include and the gate could never pass).  -no-codfile keeps
# the pristine fetched tree read-only.
INCLUDE="$ROOT/demo_lib;$ROOT/third_party/jallib/lib;$ROOT/tests"
for rel in "${SAMPLES[@]}"; do
  echo "==> $rel"
  out="$(mktemp)"
  if ! "$JAL" -s "$INCLUDE" -no-codfile "$rel" >"$out" 2>&1; then
    echo "COMPILE FAILED: $rel" >&2
    cat "$out" >&2
    rm -f "$out"
    fail=1
    continue
  fi
  if grep -q 'skip error' "$out"; then
    echo "SKIP ERROR in output: $rel" >&2
    grep 'skip error' "$out" >&2 || true
    rm -f "$out"
    fail=1
    continue
  fi
  rm -f "$out"
  echo "    ok"
done

exit "$fail"

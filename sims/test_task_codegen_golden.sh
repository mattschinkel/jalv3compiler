#!/usr/bin/env bash
# Task-codegen golden canary (dev tooling 2026-10-05).
#
# The task-control bodies cover task codegen at RUNTIME (QEMU / gpsim),
# but nothing watched the EMITTED BYTES: a codegen change that keeps the
# bodies passing (or, worse, one that only shows up in programs shaped
# differently than the bodies) would land unnoticed. This compiles the
# three taskctl bodies (classic / PIC32 / ESP32) and byte-compares the
# HEX against checked-in golden snapshots in sims/golden/.
#
# When a change is INTENTIONAL: recompile the bodies with the new
# compiler into sims/golden/ (same command as below), verify the
# runtime gates still pass, and commit the updated fixtures with a note
# of WHY the bytes changed.
#
# Usage: sims/test_task_codegen_golden.sh [compiler]
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Normalize the loader boot metadata's YYYYMMDD build-date word (changes
# at midnight; would fail the canary on pure date rollover). All other
# bytes remain strictly byte-identical.
date_norm() {
  python3 "$ROOT/sims/_hex_datemask.py" "$1"
}
COMPILER="${1:-$ROOT/bin/jalv3-x86-64}"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

fails=0
for spec in \
  "classic:18f4620_compiler_regression_taskctl:sims/gpsim/18f4620_compiler_regression_taskctl.jal" \
  "pic32:32mz1024efh064_taskctl:sims/pic32/32mz1024efh064_taskctl.jal" \
  "esp32:esp32_body_taskctl:sims/esp32/esp32_body_taskctl.jal" \
; do
  label="${spec%%:*}"; rest="${spec#*:}"
  name="${rest%%:*}"; src="${rest#*:}"
  golden="$ROOT/sims/golden/$name.hex"
  if "$COMPILER" -s "$ROOT/demo_lib;$ROOT/third_party/jallib/lib;$ROOT/sims/shared" \
      -no-codfile -qemu-hex -hex "$TMPDIR/$name.hex" "$ROOT/$src" >/dev/null 2>&1;
  then
    # The MIPS loader boot metadata embeds a YYYYMMDD build-date word
    # (e.g. 0x01352890 = 20261008) -- it changes at every midnight and
    # would fail the canary on pure date rollover. Normalize plausible
    # date words (20200101..20991231) in BOTH files before comparing;
    # every other byte stays strictly byte-identical.
    if cmp -s <(date_norm "$golden") <(date_norm "$TMPDIR/$name.hex"); then
      echo "OK   $label task codegen byte-identical"
    else
      echo "FAIL $label task codegen differs from sims/golden/$name.hex"
      echo "     (intentional? regenerate the fixture and rerun runtime gates)"
      fails=$((fails+1))
    fi
  else
    echo "FAIL $label body no longer compiles"
    fails=$((fails+1))
  fi
done

echo "---"
if [ "$fails" -eq 0 ]; then
  echo "RESULT: GOLDEN CODEGEN UNCHANGED"
  exit 0
fi
echo "RESULT: $fails GOLDEN FILE(S) CHANGED"
exit 1

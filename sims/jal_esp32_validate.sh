#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# (jalv3compiler simulator test harness)

# Compile an ESP32 .jal, free-run in espressif/qemu, validate DRAM via @sim_validate.
#
# ESP32 twin of jal_sim_validate.sh (PIC32): same sim_validate_engine
# contract, but sims/esp32_qemu.py converts the HEX to a flash image,
# boots it through the ESP32 ROM in qemu-system-xtensa, and dumps DRAM at
# ESP32_RAM_BASE (absolute addresses, matching the `v_` EQU dump).
#
# Usage:
#   ./sims/jal_esp32_validate.sh [options] path/to/file.jal
#
# Options:
#   --compiler PATH     Compiler binary (default: ./bin/jalv3-x86-64)
#   -s DIR              First library search path (default: lib); sims/
#                       is always appended
#   --quiet             Less console output
#   --no-validate       Compile + QEMU dump only (no contract required)
#
# Requires: python3, sims/esp32_qemu.py,
#           /opt/qemu-esp32/qemu/bin/qemu-system-xtensa (override with
#           ESP32_QEMU). See sims/sim_validate_contract.txt.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ENGINE="$ROOT/sims/sim_validate_engine.py"
QEMU_PY="$ROOT/sims/esp32_qemu.py"
LIBDIR="demo_lib;third_party/jallib/lib"   # 32-bit lib variants shadow pristine jallib
COMPILER="$ROOT/bin/jalv3-x86-64"
NO_VALIDATE=0
QUIET=0
JAL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --compiler) COMPILER="$2"; shift 2 ;;
    -s)         LIBDIR="$2"; shift 2 ;;
    --quiet)    QUIET=1; shift ;;
    --no-validate) NO_VALIDATE=1; shift ;;
    -h|--help)
      sed -n '1,24p' "$0"
      exit 0
      ;;
    *)
      JAL="$1"
      shift
      ;;
  esac
done

if [[ -z "$JAL" ]]; then
  echo "Usage: $0 [options] path/to/file.jal" >&2
  exit 2
fi
if [[ ! -f "$JAL" ]]; then
  echo "error: not a file: $JAL" >&2
  exit 2
fi
if [[ ! -f "$COMPILER" ]]; then
  echo "error: compiler not found: $COMPILER" >&2
  exit 2
fi
if [[ ! -f "$ENGINE" ]] || [[ ! -f "$QEMU_PY" ]]; then
  echo "error: missing engine or sims/esp32_qemu.py" >&2
  exit 2
fi

JAL_ABS="$(cd "$(dirname "$JAL")" && pwd)/$(basename "$JAL")"
GP_S="${LIBDIR};${ROOT}/sims/shared"

META_JSON="$(python3 "$ENGINE" meta "$JAL_ABS")"
DURATION="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["duration_s"])' <<<"$META_JSON")"
MACHINE="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["machine"])' <<<"$META_JSON")"
mapfile -t COMPILER_EXTRA < <(python3 -c 'import json,sys; m=json.load(sys.stdin)
for a in (m.get("compiler_args") or []):
    print(a)' <<<"$META_JSON") || true

if [[ "$NO_VALIDATE" -eq 0 ]]; then
  if ! python3 "$ENGINE" check-contract "$JAL_ABS"; then
    exit 1
  fi
fi

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jal_esp32_build.XXXXXX")"
JAL_BASENAME="$(basename "$JAL_ABS" .jal)"
ASM_ABS="$BUILD_DIR/${JAL_BASENAME}.asm"
HEX_ABS="$BUILD_DIR/${JAL_BASENAME}.hex"
DUMP_ABS="$BUILD_DIR/${JAL_BASENAME}.dump"
trap 'rm -rf "$BUILD_DIR"' EXIT

if [[ "$QUIET" -eq 0 ]]; then
  echo "=== jal_esp32_validate ==="
  echo "jal:       $JAL_ABS"
  echo "compiler:  $COMPILER"
  echo "machine:   $MACHINE"
  echo "duration:  ${DURATION}s"
  echo "build dir: $BUILD_DIR"
fi

set +e
OUT="$("$COMPILER" -s "${GP_S}" -no-codfile -asm "$ASM_ABS" -hex "$HEX_ABS" "${COMPILER_EXTRA[@]}" "$JAL_ABS" 2>&1)"
RC=$?
set -e
if [[ "$QUIET" -eq 0 ]]; then
  echo "$OUT"
elif [[ "$RC" -ne 0 ]]; then
  echo "$OUT" >&2
fi
if [[ "$RC" -ne 0 ]]; then
  echo "error: compile failed ($RC)" >&2
  exit "$RC"
fi
if ! echo "$OUT" | grep -qE '[0-9]+ errors, [0-9]+ warnings'; then
  echo "error: missing compiler summary line" >&2
  exit 1
fi
ERRS="$(echo "$OUT" | grep -E '[0-9]+ errors, [0-9]+ warnings' | tail -1 | awk '{print $1}')"
WARNS="$(echo "$OUT" | grep -E '[0-9]+ errors, [0-9]+ warnings' | tail -1 | awk '{print $3}')"
if [[ "$ERRS" != "0" ]]; then
  echo "error: compile reported $ERRS errors" >&2
  exit 1
fi
if [[ "$WARNS" != "0" ]]; then
  echo "error: compile reported $WARNS warnings (warnings-as-errors)" >&2
  exit 1
fi

if [[ "$NO_VALIDATE" -eq 0 ]]; then
  if ! python3 "$ENGINE" check-asm-symbols "$JAL_ABS" "$ASM_ABS"; then
    exit 1
  fi
fi

# Free-run in QEMU; write gpsim-like dump text for the engine.
# When the @sim_validate block has a `uart_out:`/`uart_contains:` line,
# capture UART0 output into UART_ABS for the engine to compare.
NEEDS_UART="$(python3 - "$JAL_ABS" <<'PY'
import sys
sys.path.insert(0, "sims")
from sim_validate_engine import parse_validate_block
from pathlib import Path
m = parse_validate_block(Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace"))
print(1 if (m.get("uart_out") or m.get("uart_contains")) else 0)
PY
)"
UART_ABS="$BUILD_DIR/${JAL_BASENAME}.uart"
ICOUNT="$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("icount") or 3)' <<<"$META_JSON")"
python3 - "$QEMU_PY" "$HEX_ABS" "$MACHINE" "$DURATION" "$DUMP_ABS" "$UART_ABS" "$NEEDS_UART" "$ICOUNT" <<'PY'
import importlib.util
import sys
from pathlib import Path

qemu_py, hex_path, machine, duration, dump_path, uart_path, needs_uart, icount = sys.argv[1:9]
spec = importlib.util.spec_from_file_location("esp32_qemu", qemu_py)
mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(mod)
capture = needs_uart == "1"
if capture:
    ram, ser = mod.run_hex(hex_path, machine=machine, duration_s=float(duration), dump_bytes=1024, capture_serial=True, icount_shift=int(icount))
    Path(uart_path).write_bytes(ser)
else:
    ram = mod.run_hex(hex_path, machine=machine, duration_s=float(duration), dump_bytes=1024, icount_shift=int(icount))
Path(dump_path).write_text(mod.xp_to_dump_text(ram), encoding="utf-8")
PY

if [[ "$NO_VALIDATE" -ne 0 ]]; then
  echo "=== QEMU dump (no validate) ==="
  cat "$DUMP_ABS"
  exit 0
fi

if [[ "$QUIET" -eq 0 ]]; then
  echo "--- validate ---"
fi
python3 "$ENGINE" validate "$JAL_ABS" "$ASM_ABS" "$DUMP_ABS" "$UART_ABS"

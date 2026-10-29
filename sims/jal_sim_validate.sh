#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# (jalv3compiler simulator test harness)

# Compile a PIC32 .jal, free-run in sergev/qemu, validate SRAM via @sim_validate.
#
# Usage:
#   ./sims/jal_sim_validate.sh [options] path/to/file.jal
#
# Options:
#   --compiler PATH     Compiler binary (default: ./bin/jalv3-x86-64)
#   -s DIR              First library search path (default: lib); sims/
#                       is always appended
#   --quiet             Less console output
#   --no-validate       Compile + QEMU dump only (no contract required)
#
# Requires: python3, sims/pic32_qemu.py, /opt/qemu-pic32/.../qemu-system-mipsel
#           (override with PIC32_QEMU). See sims/sim_validate_contract.txt.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ENGINE="$ROOT/sims/sim_validate_engine.py"
QEMU_PY="$ROOT/sims/pic32_qemu.py"
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
      sed -n '1,20p' "$0"
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
  echo "error: missing engine or sims/pic32_qemu.py" >&2
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

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jal_sim_build.XXXXXX")"
JAL_BASENAME="$(basename "$JAL_ABS" .jal)"
ASM_ABS="$BUILD_DIR/${JAL_BASENAME}.asm"
HEX_ABS="$BUILD_DIR/${JAL_BASENAME}.hex"
DUMP_ABS="$BUILD_DIR/${JAL_BASENAME}.dump"
trap 'rm -rf "$BUILD_DIR"' EXIT

if [[ "$QUIET" -eq 0 ]]; then
  echo "=== jal_sim_validate ==="
  echo "jal:       $JAL_ABS"
  echo "compiler:  $COMPILER"
  echo "machine:   $MACHINE"
  echo "duration:  ${DURATION}s"
  echo "build dir: $BUILD_DIR"
fi

set +e
OUT="$("$COMPILER" -s "${GP_S}" -no-codfile -qemu-hex -asm "$ASM_ABS" -hex "$HEX_ABS" "${COMPILER_EXTRA[@]}" "$JAL_ABS" 2>&1)"
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
# When the @sim_validate block has a `uart_out:` / `uart_contains:` line,
# capture the emulated UART output into UART_ABS for the engine to compare.
# `uart_input: <text>` feeds bytes into the guest's UART (the QEMU PIC32
# UART model has a single-byte RX holding register, so the feed is paced).
UART_INPUT="$(python3 - "$JAL_ABS" <<'PY'
import sys
sys.path.insert(0, "tests")
from sim_validate_engine import parse_validate_block
from pathlib import Path
m = parse_validate_block(Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace"))
print(m.get("uart_input") or "")
PY
)"
NEEDS_UART="$(python3 - "$JAL_ABS" <<'PY'
import sys
sys.path.insert(0, "tests")
from sim_validate_engine import parse_validate_block
from pathlib import Path
m = parse_validate_block(Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace"))
print(1 if (m.get("uart_out") or m.get("uart_contains") or m.get("uart_input")) else 0)
PY
)"
UART_ABS="$BUILD_DIR/${JAL_BASENAME}.uart"
python3 - "$QEMU_PY" "$HEX_ABS" "$MACHINE" "$DURATION" "$DUMP_ABS" "$UART_ABS" "$NEEDS_UART" "$UART_INPUT" <<'PY'
import codecs
import importlib.util
import sys
from pathlib import Path

(qemu_py, hex_path, machine, duration, dump_path, uart_path,
 needs_uart, uart_input) = sys.argv[1:9]
spec = importlib.util.spec_from_file_location("pic32_qemu", qemu_py)
mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(mod)
capture = needs_uart == "1"
serial_input = None
if uart_input:
    serial_input = codecs.decode(uart_input, "unicode_escape").encode("latin-1")
if capture:
    ram, ser = mod.run_hex(
        hex_path, machine=machine, duration_s=float(duration),
        dump_bytes=1024, capture_serial=True, serial_input=serial_input)
    Path(uart_path).write_bytes(ser)
else:
    ram = mod.run_hex(hex_path, machine=machine, duration_s=float(duration), dump_bytes=1024)
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

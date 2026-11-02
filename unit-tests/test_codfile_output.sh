#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# COD (debug information) output is a driver feature that no other gate
# touched: every compile pin and the whole coverage run pass `-no-codfile`,
# so src/libutils/cod_file.c sat at 3.46% and a regression in the COD writer
# (or in the flag that disables it) would have shipped unseen.
#
# This pin is the functional half; `unit-tests/coverage_report.py` compiles a
# few samples WITH COD enabled as its measurement half, so the path is both
# tested and counted.
#
# Checks:
#   1. -codfile NAME writes NAMEs, and the file carries the source path and
#      the target device (i.e. it is the real COD, not an empty stub).
#   2. default naming: without -codfile the file lands beside the source.
#   3. the writer is stable: the same source gives the same COD size
#      (the COD header embeds a build date, so byte identity is not expected).
#   4. -no-codfile writes nothing (the flag every other gate relies on).
#
# Usage: unit-tests/test_codfile_output.sh [compiler]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$ROOT/bin/beta/jalv3-x86-64}
INC="$ROOT/demo_lib;$ROOT/third_party/jallib/lib"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

SRC="$WORK/probe.jal"
cp "$ROOT/demo_sample/16f887_blink.jal" "$SRC" || {
  echo "test_codfile_output: missing demo_sample/16f887_blink.jal" >&2
  exit 2
}

compile() { # outdir [extra args...]
  local out="$1"
  shift
  ( cd "$ROOT" && "$CC" -s "$INC" -hex "$out/out.hex" "$@" "$SRC" \
      >"$out/compile.log" 2>&1 )
}

# 1. explicit -codfile
d="$WORK/explicit"
mkdir -p "$d"
if compile "$d" -codfile "$d/probe.cod"; then
  if [[ -s "$d/probe.cod" ]]; then
    ok "-codfile writes the requested file ($(wc -c <"$d/probe.cod") bytes)"
  else
    bad "-codfile produced an empty file"
  fi
  if grep -aq "$(basename "$SRC")" "$d/probe.cod" \
     && grep -aq "16F887" "$d/probe.cod"; then
    ok "COD names the source and the target device"
  else
    bad "COD is missing the source path or the device name" \
      "$(grep -ao '16F[0-9A-Z]*' "$d/probe.cod" | head -1)"
  fi
else
  bad "-codfile compile failed" "$(tail -5 "$d/compile.log")"
fi

# 2. default naming (beside the source)
d="$WORK/default"
mkdir -p "$d"
cp "$SRC" "$d/probe.jal"
if ( cd "$ROOT" && "$CC" -s "$INC" -hex "$d/out.hex" "$d/probe.jal" \
       >"$d/compile.log" 2>&1 ); then
  if [[ -s "$d/probe.cod" ]]; then
    ok "default COD lands beside the source"
  else
    bad "default COD not created beside the source"
  fi
else
  bad "default-COD compile failed" "$(tail -5 "$d/compile.log")"
fi

# 3. the writer is stable: the same source produces the same COD size.
#    (Byte identity is not expected -- the COD header embeds the build date.)
d="$WORK/stable"
mkdir -p "$d"
compile "$d" -codfile "$d/a.cod" >/dev/null 2>&1
compile "$d" -codfile "$d/b.cod" >/dev/null 2>&1
if [[ -s "$d/a.cod" && -s "$d/b.cod" ]] \
   && [[ "$(wc -c <"$d/a.cod")" -eq "$(wc -c <"$d/b.cod")" ]]; then
  ok "COD size is stable across compiles (build date is the only variance)"
else
  bad "two identical compiles produced different-sized COD files" \
    "$(wc -c <"$d/a.cod" 2>/dev/null) vs $(wc -c <"$d/b.cod" 2>/dev/null)"
fi

# 4. -no-codfile writes nothing
d="$WORK/none"
mkdir -p "$d"
cp "$SRC" "$d/probe.jal"
if ( cd "$ROOT" && "$CC" -s "$INC" -no-codfile -hex "$d/out.hex" "$d/probe.jal" \
       >"$d/compile.log" 2>&1 ); then
  if compgen -G "$d/*.cod" >/dev/null; then
    bad "-no-codfile still wrote a COD" "$(ls "$d"/*.cod)"
  else
    ok "-no-codfile writes no COD"
  fi
else
  bad "-no-codfile compile failed" "$(tail -5 "$d/compile.log")"
fi

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "codfile output: PASS"
  exit 0
fi
echo "codfile output: $fails FAILURE(S)"
exit 1

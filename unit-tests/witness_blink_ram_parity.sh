#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# Pin witness for fix_pic12_dead_bitbucket_gpr_reclaim_20261119:
# 10f200_blink_intosc.jal must compile to Data area 6 (parity with jalv25r9).
# The pre-fix build emits Data area 7; this witness exits nonzero on it.
set -u
compiler="$1"
repo="$(cd "$(dirname "$0")/.." && pwd)"
out="$( cd "$repo" && "$compiler" -s third_party/jallib/lib \
  -hex /tmp/witness_blink_ram_parity.hex \
  third_party/jallib/samples/10f200_blink_intosc.jal 2>&1 )"
printf '%s\n' "$out" | grep -q 'Data area: 6 of 16 used'

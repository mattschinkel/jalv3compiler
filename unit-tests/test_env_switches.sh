#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# DX F1: strict JALV2_* switch parsing and the --features report.
#
#   1. the accepted on/off sets are case-insensitive and whitespace-tolerant
#      (`FALSE` and ` 0 ` mean OFF, not the old "anything not 0 is ON");
#   2. an invalid value is ONE stderr line with the stable code JALV2-SWITCH,
#      the effective mode and the doc anchor, and the default is used;
#   3. --features / -v report the effective modes and exit;
#   4. codegen follows: a no-pragma fat witness has fat tables by DEFAULT
#      (unset and an invalid value both mean on) and none when the switch is
#      explicitly off (`FALSE`/` 0 `);
#   5. an explicit off prints ONE JALV2-NOTICE (classic is deprecated), and
#      the default prints none.
#
# Usage: unit-tests/test_env_switches.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib;sims/shared'
WITNESS="$REPO/sims/gpsim/16f887_gpsim_pointer_slot_chunked_elem.jal"

features() {  # features <log> [ENV=VAL ...]
  local log="$1"; shift
  ( cd "$REPO" && env "$@" "$CC" --features ) > "$log" 2>&1
}

fail() {
  echo "FAIL: $1"
  shift
  sed 's/^/     /' "$@" 2>/dev/null | head -6
  fails=$((fails + 1))
}

# --- 1. accepted sets -----------------------------------------------------
for spec in "JALV2_FAT_POINTERS=FALSE:off" "JALV2_FAT_POINTERS= 0 :off" \
            "JALV2_FAT_POINTERS=false:off" "JALV2_FAT_POINTERS=TRUE:on" \
            "JALV2_FAT_POINTERS=1:on" "JALV2_FAT_POINTERS=on:on"; do
  envspec=${spec%:*}; want=${spec##*:}
  log="$WORK/f_${envspec//[^A-Za-z0-9]/_}.log"
  features "$log" "$envspec"
  if grep -qE "fat pointers +: $want " "$log" \
      && ! grep -q 'JALV2-SWITCH' "$log"; then
    echo "ok   accepted value: $envspec -> $want"
  else
    fail "accepted value: $envspec -> $want" "$log"
  fi
done

# --- the default (unset/empty) is ON since the 2026-11-11 flip -----------
log="$WORK/default.log"
( cd "$REPO" && env -u JALV2_FAT_POINTERS "$CC" --features ) > "$log" 2>&1
if grep -qE 'fat pointers +: on ' "$log" && ! grep -q 'JALV2-SWITCH' "$log" \
    && ! grep -q 'JALV2-NOTICE' "$log"; then
  echo "ok   unset value: default on, no notice"
else
  fail "unset value: default on" "$log"
fi

# --- 2. the invalid-value contract ---------------------------------------
log="$WORK/typo.log"
features "$log" "JALV2_FAT_POINTERS=flase"
sw_ct=$(grep -c 'JALV2-SWITCH' "$log" || true)
if [ "$sw_ct" -eq 1 ] \
    && grep -q 'JALV2-SWITCH: JALV2_FAT_POINTERS' "$log" \
    && grep -q 'is not on/off' "$log" \
    && grep -q 'using on' "$log" \
    && grep -q 'one-access-model.md#feature-switches' "$log" \
    && grep -qE 'fat pointers +: on ' "$log"; then
  echo "ok   invalid value: one JALV2-SWITCH line and the default mode"
else
  fail "invalid value contract (warnings=$sw_ct)" "$log"
fi
# the same typo through a compile (the engine is read many times): still one
log="$WORK/typo_compile.log"
( cd "$REPO" && JALV2_FAT_POINTERS=flase "$CC" -s "$LIBS" -no-codfile \
    -asm "$WORK/typo.asm" -hex "$WORK/typo.hex" "$WITNESS" ) > "$log" 2>&1
sw_ct=$(grep -c 'JALV2-SWITCH' "$log" || true)
if [ "$sw_ct" -eq 1 ] && grep -q '^0 errors' "$log"; then
  echo "ok   invalid value: warned exactly once across a compile"
else
  fail "invalid value warning count across a compile (warnings=$sw_ct)" "$log"
fi

# --- 3. --features / -v exist --------------------------------------------
log="$WORK/alias.log"
( cd "$REPO" && "$CC" -v ) > "$log" 2>&1
if grep -q 'JALV2 feature switches' "$log"; then
  echo "ok   -v reports the feature switches"
else
  fail "-v report" "$log"
fi

# --- 4. codegen follows the switch ---------------------------------------
compile() {  # compile <env-spec or ''> <asm>
  if [ -n "$1" ]; then
    ( cd "$REPO" && env "$1" "$CC" -s "$LIBS" -no-codfile \
        -asm "$2" -hex "$WORK/o.hex" "$WITNESS" ) >/dev/null 2>&1
  else
    ( cd "$REPO" && env -u JALV2_FAT_POINTERS "$CC" -s "$LIBS" \
        -no-codfile -asm "$2" -hex "$WORK/o.hex" "$WITNESS" ) >/dev/null 2>&1
  fi
}
fat_symbols() {  # fat_symbols <asm> -> count
  grep -cE 'pic_fat_seg_addr|_pic_fat_desc_tbl|_pic_fat_id' "$1" 2>/dev/null || true
}
for spec in "JALV2_FAT_POINTERS=FALSE" "JALV2_FAT_POINTERS= 0 "; do
  asm="$WORK/off_${spec//[^A-Za-z0-9]/_}.asm"
  compile "$spec" "$asm"
  n=$(fat_symbols "$asm")
  if [ "${n:-1}" -eq 0 ]; then
    echo "ok   codegen: $spec has no fat tables"
  else
    fail "codegen: $spec still emitted fat tables ($n)" "$asm"
  fi
done
for spec in "" "JALV2_FAT_POINTERS=TRUE" "JALV2_FAT_POINTERS=flase"; do
  asm="$WORK/on_${spec:-default}.asm"
  compile "$spec" "$asm"
  n=$(fat_symbols "$asm")
  if [ "${n:-0}" -ge 1 ]; then
    echo "ok   codegen: ${spec:-<unset>} emits the fat tables"
  else
    fail "codegen: ${spec:-<unset>} has no fat tables" "$asm"
  fi
done

# --- 5. the classic-mode deprecation notice -------------------------------
log="$WORK/notice.log"
( cd "$REPO" && JALV2_FAT_POINTERS=0 "$CC" -s "$LIBS" -no-codfile \
    -asm "$WORK/notice.asm" -hex "$WORK/notice.hex" "$WITNESS" ) > "$log" 2>&1
notice_ct=$(grep -c 'JALV2-NOTICE' "$log" || true)
if [ "$notice_ct" -eq 1 ] \
    && grep -q 'JALV2_FAT_POINTERS=0 selects the classic' "$log" \
    && grep -q 'deprecated' "$log" \
    && grep -q '^0 errors' "$log"; then
  echo "ok   explicit off: one deprecation notice, compile succeeds"
else
  fail "classic deprecation notice (notices=$notice_ct)" "$log"
fi

if (( fails )); then
  echo "FAIL: $fails switch check(s)"
  exit 1
fi
echo "ok   JALV2 switch parsing"

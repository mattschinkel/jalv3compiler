#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Pins for USER arrays fed to one open-array formal from several call sites
# (fixes/fix_openarray_user_array_percall.md; the string half is
# unit-tests/test_string_arg_chunk_map.sh).
#
# The defect: a big buffer passed to `byte in data[]` is addressed through a
# compile-time chunk map, and the callee body is emitted ONCE.  Copying the
# actual's map onto the formal (guarded by "the formal has no map yet") makes
# the FIRST call site's layout win, so a second call with a buffer in different
# banks is addressed with the first buffer's bank math -- silent memory
# corruption.  It needs the actuals to HAVE chunk maps (a direct cross-bank
# access creates them), which is why the compile-only sd_card/fat32 samples
# never showed it.
#
# The fix gives such a formal a map of its own (`_pic_chunkmap_N`,
# [count][base lo,hi,span]...); every call site fills it with the ACTIVE layout
# before the call and the callee's subscript walks it.  The map is created only
# when a formal is fed two or more DIFFERENT layouts AND at least one of them is
# split across RAM banks -- one layout is addressed exactly by the baked copy,
# and a formal with no chunked actual addresses through the per-call pointer.
# That inertness is what keeps the rest of the corpus byte-identical, and it is
# pinned here.
#
# Runtime proof: sims/gpsim/16f1825_gpsim_user_array_over_bank.jal (the pin --
# it fails 7/10 checks on the pre-fix build) and its 18F4620 twin.
#
# Usage: unit-tests/test_openarray_user_array_percall.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jal_userchunk-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

INC="$REPO/demo_lib;$REPO/third_party/jallib/lib;$REPO/sims/shared"

# compile SOURCE into $WORK/out.asm; echoes the exit code
cc_one() {
  timeout 120 "$CC" -s "$INC" -no-codfile -no-fuse \
    -hex "$WORK/out.hex" -asm "$WORK/out.asm" "$1" >"$WORK/out.txt" 2>&1
  echo $?
}

# dev_head DEVICE -> the pragma block the sims' device files use
dev_head() { # device
  cat <<JAL
include $1
pragma target clock 16_000_000
pragma target OSC INTOSC_NOCLKOUT
pragma target PLLEN DISABLED
pragma target WDT DISABLED
pragma target PWRTE ENABLED
pragma target BROWNOUT DISABLED
pragma target FCMEN DISABLED
pragma target IESO DISABLED
pragma target LVP DISABLED
pragma target MCLR EXTERNAL
enable_digital_io()
JAL
}

# ---------------------------------------------------------------- 1. the defect
# two 150-byte arrays (128-byte banks -> 2 and 3 chunks) through ONE formal:
# the callee must walk a map that the call site fills, not a baked layout.
# The direct `big_x[149]` reads are load-bearing: they are what makes the
# allocator SPLIT the arrays into chunks (a buffer that is only written through
# an open-array formal is placed linearly, and then the same program is correct
# on the pre-fix build -- see the investigation doc).
{
  dev_head 16f1825
  cat <<'JAL'
var byte big_a[150]
var byte big_b[150]
var volatile byte sink = 0
procedure fill_array(byte in data[], byte in base) is
   var byte ii = 0
   for 150 loop
      data[ii] = base + ii
      ii = ii + 1
   end loop
end procedure
forever loop
   fill_array(big_a, 1)
   fill_array(big_b, 100)
   sink = big_a[149]
   sink = big_b[149]
end loop
JAL
} >"$WORK/defect.jal"
rc=$(cc_one "$WORK/defect.jal")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/out.txt" \
   && grep -q "_pic_chunkmap_" "$WORK/out.asm" \
   && grep -q "_pic_chunk_cnt" "$WORK/out.asm"; then
  ok "two multi-bank arrays through one formal: per-formal map + walk emitted"
else
  bad "two multi-bank arrays through one formal: per-formal map + walk emitted" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/out.txt" | head -2)"
fi

# ------------------------------------------------- 2. one call site: no map
# one layout is addressed exactly by the baked copy, so the runtime machinery
# must stay out (RAM + instructions for nothing).
{
  dev_head 16f1825
  cat <<'JAL'
var byte big_a[150]
procedure fill_array(byte in data[], byte in base) is
   var byte ii = 0
   for 150 loop
      data[ii] = base + ii
      ii = ii + 1
   end loop
end procedure
forever loop
   fill_array(big_a, 1)
end loop
JAL
} >"$WORK/one_site.jal"
rc=$(cc_one "$WORK/one_site.jal")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/out.txt" \
   && ! grep -q "_pic_chunkmap_" "$WORK/out.asm"; then
  ok "one call site with a multi-bank buffer: clean, no runtime map"
else
  bad "one call site with a multi-bank buffer: clean, no runtime map" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/out.txt" | head -2)"
fi

# ------------------------------------------- 3. the same buffer twice: no map
{
  dev_head 16f1825
  cat <<'JAL'
var byte big_a[150]
procedure fill_array(byte in data[], byte in base) is
   var byte ii = 0
   for 150 loop
      data[ii] = base + ii
      ii = ii + 1
   end loop
end procedure
forever loop
   fill_array(big_a, 1)
   fill_array(big_a, 100)
end loop
JAL
} >"$WORK/same.jal"
rc=$(cc_one "$WORK/same.jal")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/out.txt" \
   && ! grep -q "_pic_chunkmap_" "$WORK/out.asm"; then
  ok "the same buffer passed twice: clean, no runtime map"
else
  bad "the same buffer passed twice: clean, no runtime map" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/out.txt" | head -2)"
fi

# ----------------------------- 4. two buffers that fit one bank: no map
# neither actual is chunked, so the formal has no baked map and every call site
# passes the pointer correctly.
{
  dev_head 16f1825
  cat <<'JAL'
var byte buf1[100]
var byte buf2[100]
procedure fill_array(byte in data[], byte in base) is
   var byte ii = 0
   for 100 loop
      data[ii] = base + ii
      ii = ii + 1
   end loop
end procedure
forever loop
   fill_array(buf1, 1)
   fill_array(buf2, 100)
end loop
JAL
} >"$WORK/one_bank.jal"
rc=$(cc_one "$WORK/one_bank.jal")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/out.txt" \
   && ! grep -q "_pic_chunkmap_" "$WORK/out.asm"; then
  ok "two one-bank buffers through one formal: clean, no runtime map"
else
  bad "two one-bank buffers through one formal: clean, no runtime map" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/out.txt" | head -2)"
fi

# ------------------------- 5. a const ROM actual beside a RAM one: map needed
# the two need different address paths (ROM is read through the lookup path,
# RAM walks the map); the portability proof is the string witness, this pins
# that the shape still compiles and gets the map.
{
  dev_head 16f1825
  cat <<'JAL'
const byte rom_tab[] = "0123456789abcdefghijklmnopqrstuvwxyz"
var byte big_a[150]
var byte sink
var volatile byte tail = 0
var volatile word n = 0
procedure take(byte in data[]) is
   var word len = count(data)
   var byte ii = 0
   for len using ii loop
      sink = data[ii]
      n = n + 1
   end loop
end procedure
forever loop
   take(rom_tab)
   take(big_a)
   tail = big_a[149]
end loop
JAL
} >"$WORK/rom_ram.jal"
rc=$(cc_one "$WORK/rom_ram.jal")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/out.txt" \
   && grep -q "_pic_chunkmap_" "$WORK/out.asm"; then
  ok "a const ROM actual beside a RAM buffer: clean, runtime map wired"
else
  bad "a const ROM actual beside a RAM buffer: clean, runtime map wired" \
      "rc=$rc" "$(grep -aE 'error|errors,' "$WORK/out.txt" | head -2)"
fi

# ------------------- 6. a CONSTANT subscript past the first chunk
# `data[149]` used to fall through to the linear `deref + 149` sequence, which
# cannot see a bank hole (element 149 of a 150-byte array split 73/77 landed 45
# bytes short).  The ladder is what the fix emits; the linear form is the bug.
{
  dev_head 16f1825
  cat <<'JAL'
var byte big_a[150]
var volatile byte got = 0
var volatile byte tail = 0
procedure take(byte in data[]) is
   got = data[149]
end procedure
forever loop
   take(big_a)
   tail = big_a[149]
end loop
JAL
} >"$WORK/const_sub.jal"
rc=$(cc_one "$WORK/const_sub.jal")
if [ "$rc" -eq 0 ] && grep -q "0 errors, 0 warnings" "$WORK/out.txt" \
   && ! grep -qE "addlw[[:space:]]+149" "$WORK/out.asm"; then
  ok "a constant subscript past the first chunk walks the map (no linear add)"
else
  bad "a constant subscript past the first chunk walks the map (no linear add)" \
      "rc=$rc linear_add=$(grep -cE 'addlw[[:space:]]+149' "$WORK/out.asm")" \
      "$(grep -aE 'error|errors,' "$WORK/out.txt" | head -2)"
fi

echo "----"
if [ "$fails" -ne 0 ]; then
  echo "openarray user-array per-call: $fails FAILURE(S)"
  exit 1
fi
echo "openarray user-array per-call: PASS"
exit 0

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# P3b: the capacity matrix.  A first-class allocator states its capacity as a
# number, not as folklore: for each target and each object class, the largest
# array the allocator can place is measured and PINNED here.  A drop fails
# the gate; an intentional change refreshes the table with its fix record
# (the same rule as the HEX manifest).
#
# The probe program is deliberately minimal -- one array, one accessed
# element (an unaccessed array is optimized away, which would measure
# nothing) -- so the number is a property of the allocator, not of a sample.
# Each row also asserts that capacity+1 is REFUSED, so the boundary is real
# rather than "this size happened to compile".
#
# Usage: unit-tests/test_alloc_capacity.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
INC=(-s "$REPO/demo_lib" -s "$REPO/third_party/jallib/lib")

gen() { # $1 dev, $2 class, $3 n
  case "$2" in
    byte)    printf 'include %s\nvar byte a[%s]\na[0] = 1\n' "$1" "$3" ;;
    word)    printf 'include %s\nvar word a[%s]\na[0] = 1\n' "$1" "$3" ;;
    dword)   printf 'include %s\nvar dword a[%s]\na[0] = 1\n' "$1" "$3" ;;
    bit)     printf 'include %s\nvar bit a[%s]\na[0] = 1\n' "$1" "$3" ;;
    pointer) printf 'include %s\nvar byte v0 = 0\nvar pointer a[%s] as var byte\na[0] is v0\n' "$1" "$3" ;;
    *)       return 1 ;;
  esac
}

compiles() { # $1 dev, $2 class, $3 n
  gen "$1" "$2" "$3" > "$WORK/t.jal" || return 1
  rm -f "$WORK/t.asm" "$WORK/t.hex" "$WORK/t.cod" "$WORK/t.log"
  ( cd "$WORK" && "$CC" "${INC[@]}" t.jal > t.log 2>&1 )
  [ $? -eq 0 ] && grep -q "0 errors" "$WORK/t.log"
}

refused_for_space() { # $1 dev, $2 class, $3 n: fails AND names the space limit
  gen "$1" "$2" "$3" > "$WORK/t.jal" || return 1
  rm -f "$WORK/t.asm" "$WORK/t.hex" "$WORK/t.cod" "$WORK/t.log"
  ( cd "$WORK" && "$CC" "${INC[@]}" t.jal > t.log 2>&1 )
  local rc=$?
  [ $rc -ne 0 ] \
    && grep -qE "exceeds total available memory space|no space for" \
         "$WORK/t.log"
}

max_for() { # $1 dev, $2 class, $3 hi -> max n (or -1)
  local dev=$1 cls=$2 hi=$3 lo=1 mid
  if ! compiles "$dev" "$cls" 1; then
    echo -1
    return
  fi
  while [ "$lo" -lt "$hi" ]; do
    mid=$(( (lo + hi + 1) / 2 ))
    if compiles "$dev" "$cls" "$mid"; then
      lo=$mid
    else
      hi=$((mid - 1))
    fi
  done
  echo "$lo"
}

# device      class     pinned   search ceiling
# The pinned numbers are the 2026-11-08 measurements on the P3b binary.
while read -r dev cls want hi; do
  [ -n "${dev:-}" ] || continue
  case "$dev" in \#*) continue ;; esac
  got=$(max_for "$dev" "$cls" "$hi")
  if [ "$got" != "$want" ]; then
    echo "FAIL $dev $cls capacity $got, pinned $want"
    if [ "$got" -lt "$want" ]; then
      echo "     a capacity DROP; if the change is intentional refresh this table"
    else
      echo "     capacity grew; refresh this table with the fix record"
    fi
    fails=$((fails + 1))
  elif refused_for_space "$dev" "$cls" $((want + 1)); then
    echo "ok   $dev $cls capacity $want (and $((want + 1)) is refused for space)"
  else
    echo "FAIL $dev $cls capacity+1 still compiles"
    fails=$((fails + 1))
  fi
done <<'TABLE'
18f4620   byte    3872  40000
18f4620   word    1936  40000
18f4620   dword    968  20000
18f4620   bit    30976  60000
18f4620   pointer 1290   4000
16f18857  byte    4079  40000
16f18857  word    2039  40000
16f18857  dword   1019  20000
16f18857  bit    32632  60000
16f18857  pointer 2039   4000
16f887    byte     351  40000
16f887    word     175  40000
16f887    dword     87  20000
16f887    bit     2807  60000
16f887    pointer  175   4000
12f1840   byte     239  40000
12f1840   word     119  40000
12f1840   dword     59  20000
12f1840   bit     1912  60000
12f1840   pointer  119   4000
TABLE

echo ""
if [ "$fails" -ne 0 ]; then
  echo "RESULT: FAIL ($fails capacities)"
  exit 1
fi
echo "RESULT: PASS (20 capacities)"
exit 0

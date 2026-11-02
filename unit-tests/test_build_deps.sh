#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Header dependency tracking is a gate-level safety property, not a build
# detail: when it is wrong the compiler silently links objects built from
# older headers (the "mixed-object lesson", the root of several fix docs),
# and every correctness gate can pass on a binary that does not match the
# source.
#
# Recurring defect family (no automated coverage before this pin):
#   fixes/fix_makefile_dep_include_tab.md   -- -include was eaten as recipe
#                                              text for years (deps dead)
#   fixes/fix_gop_walk_depth.md             -- project rule ignored sub-make
#                                              failures, linking stale archives
#   fixes/fix_variable_arena.md             -- struct changes needed a clean
#   fixes/fix_build_dep_flavor_collision.md -- NEW: the shared .dep was
#       overwritten by the last flavor, so after an SAN/COV build a default
#       header edit never rebuilt the default object
#
# The pin makes the dependency graph observable without touching a file:
#   make -n --what-if=<header>  must print the compile of the dependent
#   object for the CURRENT flavor, and building another flavor in between
#   must not change that answer.
#
# Usage: unit-tests/test_build_deps.sh
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; shift; [ "$#" -gt 0 ] && printf '     %s\n' "$@"; fails=$((fails + 1)); }

DEP_DEFAULT="$REPO/src/libcore/.dep-x86-64/value.d"
OBJ_DEFAULT=".obj-x86-64/value.o"
OBJ_SAN=".obj-x86-64-san/value.o"

# 1. The tree must build (incremental: L0 has normally done this already).
if make -C "$REPO/src" ARCH=x86-64 >"$WORK/build.log" 2>&1; then
  ok "make -C src ARCH=x86-64"
else
  bad "make -C src ARCH=x86-64" "$(tail -15 "$WORK/build.log")"
  echo "build-deps: cannot continue without a build"
  exit 1
fi

# 2. The active dep file names the CURRENT flavor's object.
if [ -f "$DEP_DEFAULT" ] \
   && head -1 "$DEP_DEFAULT" | grep -q "^$OBJ_DEFAULT:"; then
  ok "default dep file targets $OBJ_DEFAULT"
else
  bad "default dep file is missing or targets another flavor" \
    "$(head -1 "$DEP_DEFAULT" 2>/dev/null)"
fi

# 3. A header edit must schedule a recompile of the dependent object.
whatif() {
  make -C "$REPO/src/libcore" ARCH=x86-64 -n --what-if=value.h 2>&1
}
if whatif | grep -q -- "-c -o $OBJ_DEFAULT"; then
  ok "value.h change rebuilds $OBJ_DEFAULT"
else
  bad "value.h change does NOT rebuild $OBJ_DEFAULT" \
    "$(whatif | head -5)"
fi

# 4. Build ONE object of a different flavor.  This is what used to rewrite
#    the shared .dep; force it with -B so the object's own dep file is
#    written even when the object already exists.
if make -C "$REPO/src/libcore" ARCH=x86-64 SAN=1 -B "$OBJ_SAN" \
     >"$WORK/san.log" 2>&1; then
  ok "built one SAN object ($OBJ_SAN)"
else
  bad "could not build $OBJ_SAN" "$(tail -10 "$WORK/san.log")"
fi

if [ -f "$REPO/src/libcore/.dep-x86-64-san/value.d" ] \
   && head -1 "$REPO/src/libcore/.dep-x86-64-san/value.d" \
        | grep -q "^$OBJ_SAN:"; then
  ok "SAN dep file is separate and targets $OBJ_SAN"
else
  bad "SAN dep file missing or mixed with the default flavor"
fi

# 5. The other flavor must not have disturbed the default graph.
if head -1 "$DEP_DEFAULT" | grep -q "^$OBJ_DEFAULT:"; then
  ok "SAN build left the default dep file targeting $OBJ_DEFAULT"
else
  bad "SAN build rewrote the default dep file" "$(head -1 "$DEP_DEFAULT")"
fi
if whatif | grep -q -- "-c -o $OBJ_DEFAULT"; then
  ok "value.h still rebuilds $OBJ_DEFAULT after the SAN build"
else
  bad "default dependency rule lost after the SAN build" \
    "$(whatif | head -5)"
fi

echo "----"
if [ "$fails" -eq 0 ]; then
  echo "build deps: PASS"
  exit 0
fi
echo "build deps: $fails FAILURE(S)"
exit 1

#!/bin/bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
#
# Phase 7: the string-clone ELIGIBILITY decision after the
# body_pos/body_pos_valid marker was deleted.  The marker only recorded
# "the parser reached `is`"; the real precondition is the DEFINED body
# block, which the clone already tests (LABEL_FLAG_DEFINED), so the
# decisions must be unchanged:
#
#   1. a procedure forward-declared and then defined is ELIGIBLE (the
#      marker's case -- the definition's body is parsed);
#   2. a body with an include/pragma inside is NOT eligible (the guard),
#      and it gets no `_strp_` clone;
#   3. an empty `begin ... end procedure` body compiles but never requests
#      a clone (nothing is stored through), and the decision is not
#      consulted -- asserted so a future change cannot start cloning it;
#   4. a call before the body exists stays refused with the string-passing
#      error (the eligibility decision is never reached on that path).
#
# The decision is observed through JALV2_PROC_ELIG=1 (one stderr line per
# query: `proc-elig: <tag> eligible=0|1 [reason=...]`).
#
# Usage: unit-tests/test_proc_clone_eligibility.sh [compiler]
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${1:-$REPO/bin/beta/jalv3-x86-64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
LIBS='demo_lib;third_party/jallib/lib'

fail() {
  echo "FAIL: $1"
  shift
  sed 's/^/     /' "$@" 2>/dev/null | tail -8
  fails=$((fails + 1))
}

compile() {  # compile <file> <asm> <log>
  ( cd "$REPO" && JALV2_PROC_ELIG=1 "$CC" -s "$LIBS" -no-codfile \
      -asm "$2" -hex "$WORK/o.hex" "$1" ) > "$3" 2>&1
}

# --- 1. forward declaration then definition: eligible ---------------------
cat > "$WORK/fwd.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
procedure fwd(byte in out data[])
procedure fwd(byte in out data[]) is
   data[0] = "A"
end procedure
s = "ab"
fwd(s)
forever loop end loop
EOF
if compile "$WORK/fwd.jal" "$WORK/fwd.asm" "$WORK/fwd.capture" \
    && grep -q '^0 errors' "$WORK/fwd.capture" \
    && grep -q 'proc-elig: fwd eligible=1' "$WORK/fwd.capture" \
    && grep -q '_strp_fwd_' "$WORK/fwd.asm"; then
  echo "ok   forward declaration then definition: eligible and cloned"
else
  fail "the forward/defined case" "$WORK/fwd.capture"
fi

# --- 2. a guarded body: ineligible, no clone ------------------------------
cat > "$WORK/guarded.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
procedure guarded(byte in out data[]) is
   pragma target clock 4_000_000
   data[0] = "B"
end procedure
s = "ab"
guarded(s)
forever loop end loop
EOF
if compile "$WORK/guarded.jal" "$WORK/guarded.asm" "$WORK/guarded.capture" \
    && grep -q '^0 errors' "$WORK/guarded.capture" \
    && grep -q 'proc-elig: guarded eligible=0 reason=guarded' "$WORK/guarded.capture" \
    && ! grep -q '_strp_guarded_' "$WORK/guarded.asm"; then
  echo "ok   a guarded body is ineligible and gets no clone"
else
  fail "the guarded-body case" "$WORK/guarded.capture"
fi

# --- 3. an empty body: compiles, never requests a clone -------------------
cat > "$WORK/empty.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
procedure empty(byte in out data[]) is
begin
end procedure
s = "ab"
empty(s)
forever loop end loop
EOF
if compile "$WORK/empty.jal" "$WORK/empty.asm" "$WORK/empty.capture" \
    && grep -q '^0 errors' "$WORK/empty.capture" \
    && ! grep -q 'proc-elig: empty' "$WORK/empty.capture" \
    && ! grep -q '_strp_empty_' "$WORK/empty.asm"; then
  echo "ok   an empty body compiles and requests no clone"
else
  fail "the empty-body case" "$WORK/empty.capture"
fi

# --- 4. a call before the body exists: refused, no decision ---------------
cat > "$WORK/late.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
procedure late(byte in out data[])
s = "ab"
late(s)
procedure late(byte in out data[]) is
   data[0] = "A"
end procedure
forever loop end loop
EOF
if compile "$WORK/late.jal" "$WORK/late.asm" "$WORK/late.capture"; then
  fail "a call before the body exists was accepted" "$WORK/late.capture"
elif grep -q 'cannot pass the string' "$WORK/late.capture" \
    && ! grep -q 'proc-elig: late' "$WORK/late.capture"; then
  echo "ok   a call before the body exists is refused (no eligibility query)"
else
  fail "the call-before-definition case" "$WORK/late.capture"
fi

# --- 5. byref default: the BYREF exclusion, no clone ----------------------
cat > "$WORK/byref_default.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
procedure fill(byref in out arr[]) is
   arr[0] = "A"
end procedure
s = "ab"
fill(s)
forever loop end loop
EOF
if compile "$WORK/byref_default.jal" "$WORK/byref_default.asm" "$WORK/byref_default.capture" \
    && grep -q '^0 errors' "$WORK/byref_default.capture" \
    && ! grep -q '_strp_fill_' "$WORK/byref_default.asm"; then
  echo "ok   a byref formal is not cloned by default"
else
  fail "the byref-default case" "$WORK/byref_default.capture"
fi

# --- 6. pragma byref_clone on: the byref formal rejoins the clone ---------
cat > "$WORK/byref_clone.jal" <<'EOF'
include 16f887
pragma target string_heap_size 64
var string s
procedure fill(byref in out arr[]) is
   pragma byref_clone on
   arr[0] = "A"
end procedure
s = "ab"
fill(s)
forever loop end loop
EOF
if compile "$WORK/byref_clone.jal" "$WORK/byref_clone.asm" "$WORK/byref_clone.capture" \
    && grep -q '^0 errors' "$WORK/byref_clone.capture" \
    && grep -q '_strp_fill_' "$WORK/byref_clone.asm"; then
  echo "ok   pragma byref_clone on clones the byref formal"
else
  fail "the byref-clone case" "$WORK/byref_clone.capture"
fi

if (( fails )); then
  echo "FAIL: $fails clone-eligibility check(s)"
  exit 1
fi
echo "PASS: string-clone eligibility"

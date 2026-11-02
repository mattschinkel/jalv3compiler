#!/bin/bash
# Witness: 2 000 SEQUENTIAL `if` statements must compile.
# Pre-fix: the GOP walk recursed once per branch and its analysis stacks were
# fixed at PIC_CODE_GOP_DEPTH (256), so this tripped
# `assert(data->ix < PIC_CODE_GOP_DEPTH)` (SIGABRT) in pic_daop.
set -u
CC=$1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jalgop-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
python3 - "$WORK/branches.jal" <<'PY'
import sys
n = 2000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\na = 1\n'
    + ''.join('if a == %d then\na = 2\nend if\n' % i for i in range(n))
    + 'forever loop\nend loop\n')
PY
"$CC" -s demo_lib -s third_party/jallib/lib -no-codfile "$WORK/branches.jal" \
  > "$WORK/out.txt" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "compile failed rc=$rc"
  tail -3 "$WORK/out.txt"
fi
exit "$rc"

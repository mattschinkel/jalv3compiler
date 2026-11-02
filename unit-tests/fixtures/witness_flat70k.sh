#!/bin/bash
# Witness: a large FLAT program (70 000 statements) must compile.
# Pre-fix: the 16-bit source reference counter wrapped at 65 536 and the
# compiler died with SIGSEGV in pfile_source_lock (use-after-free).
set -u
CC=$1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jalflat-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
python3 - "$WORK/flat.jal" <<'PY'
import sys
n = 70000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\na = 1\n'
    + 'a = 2\n' * n + 'forever loop\nend loop\n')
PY
"$CC" -s demo_lib -s third_party/jallib/lib -no-codfile "$WORK/flat.jal" \
  > "$WORK/out.txt" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "compile failed rc=$rc"
  tail -3 "$WORK/out.txt"
fi
exit "$rc"

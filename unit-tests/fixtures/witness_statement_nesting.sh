#!/bin/bash
# Witness: 12 000 nested IFs must fail CLEANLY (one "nested too deeply"
# diagnostic, exit 1).  Pre-fix: C-stack overflow -> SIGSEGV/SIGABRT.
set -u
CC=$1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jalnest-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
python3 - "$WORK/nest.jal" <<'PY'
import sys
n = 12000
open(sys.argv[1], 'w').write(
    'pragma target clock 4_000_000\ninclude 18f4620\nvar word a\na = 1\n'
    + 'if a == 1 then\n' * n + 'a = 2\n' + 'end if\n' * n
    + 'forever loop\nend loop\n')
PY
"$CC" -s demo_lib -s third_party/jallib/lib -no-codfile "$WORK/nest.jal" \
  > "$WORK/out.txt" 2>&1
rc=$?
if [ "$rc" -ne 1 ]; then
  echo "expected exit 1, got $rc"
  tail -3 "$WORK/out.txt"
  exit 1
fi
if ! grep -q 'nested too deeply' "$WORK/out.txt"; then
  echo "missing 'nested too deeply' diagnostic"
  tail -3 "$WORK/out.txt"
  exit 1
fi
exit 0

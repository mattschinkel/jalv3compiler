# unit-tests

Corpus-compile unit gate: every jallib sample and library must compile with
0 errors / 0 warnings against the current compiler, with baseline and
compare modes against the original jalv2 (bin/old_versions/).

1. `./fetch_jallib.sh` — fetch the pristine jallib 2.1 corpus into
   `third_party/jallib/` (not vendored; pinned by `jallib_pinned.txt`)
2. `python3 unittests.py --current` — compile the whole corpus
3. `python3 unittests.py --baseline|--compare` — against/versus the
   original compiler; `--pic32` compile-checks the PIC32 sim suite

`run_negative_tests.py` — compile-must-fail table over `negative/`.
`validate_hex_static.py` — static HEX sanity checks used by the gate.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.

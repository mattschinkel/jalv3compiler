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

`run_gates.sh` — the tiered gate ladder (L0 build+syntax smoke,
L1 runtime suites + compile-pin tests, L2 unit/compare + HEX manifest,
L3 second QEMU suite + ASan corpus). `run_compile_pin_tests.sh` runs the
`test_*` pin scripts in this folder.

Access-model flags (compiler): `-dump-plan <file>` writes the access-plan
rows, and `-explain-access <file>:<line>` prints the access decisions made
for one source line (kind, fact vector, operation, recipe and target, in
operand order). Exit codes: 0 when rows were printed or when the line has no
access (`no access at <file>:<line>`), 1 on a REFUSE row; a compile that
already failed keeps its failure. Example:

```sh
bin/beta/jalv3-x86-64 -s 'demo_lib;third_party/jallib/lib;tests' \
    -no-codfile -no-hex -quiet -explain-access \
    sims/picsim/16f18857_picsim_fat_pointer.jal:40 \
    sims/picsim/16f18857_picsim_fat_pointer.jal
```

The `run_plan_gate.sh` baseline (`plan-baselines/`) and the
`run_access_matrix.sh --check` matrix + recipe-row coverage
(`plan-matrix/`) pin both the decisions and the surface that made them.
The per-backend plan-diff subsets go through the SAME gate (DX F12, no
per-backend script fork):

```sh
unit-tests/run_plan_gate.sh bin/beta/jalv3-x86-64 --backend pic32
unit-tests/run_plan_gate.sh bin/beta/jalv3-x86-64 --backend esp32   # or --update
```

Each back end records its family decisions through the neutral recorder
(`src/libcore/access_record.h`, ENG A4): the row is
`program,generation,site,var,ordinal,operation,facts,matched`, so the
baseline pins the classification a back end makes. A compile failure in
either mode prints the program, the log path and the exit status (DX F5).

`test_isr_source_wiring.sh` runs the Phase 4 ISR source bodies on the QEMU
targets (ESP32 UART, ESP32 on CPU interrupt 5, PIC32 UART1) and their
negative controls (matrix route mismatch, muted source, priority 0),
which must NOT dispatch.
`test_access_cli.sh` pins the two options' argument validation (ENG S1):
`-explain-access` takes only `<file>:<line>` with a decimal line in
1..65535 (0, a typo, a negative, an overflow, trailing junk and an
over-long file name are usage errors), `-dump-plan` needs a non-empty
path, an unreadable source fails both without installing an image, and an
unwritable dump path fails the compile without touching a pre-existing
output.

Feature switches (compiler): every boolean `JALV2_*` switch is unset =
default, `1/true/yes/on` = on, `0/false/no/off` = off (case-insensitive,
whitespace-trimmed); anything else prints one `JALV2-SWITCH` warning and
uses the default. ` --features` (alias `-v`) prints the effective modes:

```sh
bin/beta/jalv3-x86-64 --features                             # fat pointers: on (default)
JALV2_FAT_POINTERS=FALSE bin/beta/jalv3-x86-64 --features     # fat pointers: off (classic, deprecated)
```

`JALV2_FAT_POINTERS` defaults **on** since 2026-11-11; `=0` still restores
the classic images for one release and prints one `JALV2-NOTICE` per
compile. The per-program opt-out is `pragma target fat_pointers off`.

The `env_switches` pin covers the accepted sets, the one-warning contract
and the codegen consequence. `docs/designs/one-access-model.md#feature-switches`
is the anchor.

U3 open-array pair attachment (compiler): the pair set now lives on the CALL
command. `JALV2_U3_PAIR_COMPARE=1` prints the acceptance comparator footer
(`pair-compare: N calls, M pairs, X mismatches`) and makes X != 0 and zero
calls errors; `JALV2_U3_PAIR_FAULT=drop|alter` corrupts one attachment so
the pin proves the comparator is non-vacuous; `JALV2_U3_CALL_SHAPES=1`
regenerates the call-shape matrix rows on stderr. Example:

```sh
JALV2_U3_PAIR_COMPARE=1 bin/beta/jalv3-x86-64 \
    -s 'demo_lib;third_party/jallib/lib;sims/shared' -no-codfile \
    sims/picsim/16f18857_picsim_string_clone.jal
# expect: pair-compare: 6 calls, 9 pairs, 0 mismatches
```

The `u3_pair_attach` pin runs the four pair shapes, both fault hooks and the
zero-call guard; the matrix is
`docs/investigations/u3_call_shape_matrix_20261110.md`.

New pins (2026-11-02, from the recurring-fix coverage audit):
`test_cli_hex_contract.sh` (CLI termination + atomic HEX install + a
non-regular target is never replaced; uses a FIFO, not `/dev/null`),
`test_oom_policy.sh` (`ulimit -v`: one OOM line, exit 1, no signal or
cascade, no leftover HEX), `test_reserved_internal_names.sh` (reserved
compiler names and the one-error `_string_heap_size` diagnostic),
`test_build_deps.sh` (per-flavor header dependency tracking), and
`test_harness_contract.py` (the test harness's own pass/fail decisions,
including `--compare` with no baseline).  All are registered in
`run_compile_pin_tests.sh`.

`test_adversarial_inputs.sh` — shape-pathological inputs (deep nesting,
huge arrays, thousands of blocks, binary junk) that the mutating fuzzer
cannot generate. Registered in the L1 pin gate; every entry pins a defect
that mutation fuzzing structurally missed. See
`docs/fixes/2026/fix_adversarial_input_gate.md`.

`coverage_report.py` — line coverage of `src/` over a spread of the corpus,
using the `COV=1` flavor and `gcov`, compared against
`coverage_baseline.txt`. Answers "which code has no test at all?"; runs in
the nightly CI job (~50 s), not in the L1 ladder. See
`docs/fixes/2026/fix_coverage_gate.md`.

`jaldbg.py` — the bug-investigation harness (2026-10-29, born from the
proc-entry-BSR chase). Diagnosis layer only: the suites above stay the
detection layer and always run first. Subcommands: `compile`, `symbols`,
`watch` (PC-attributed write timeline on named variables under gpsim;
final-RAM snapshot on the 32-bit QEMU targets), `trace` (disassembly
annotated with symbols and the a/d bits — opcode table parsed live from
src/libpic12/pic16.c and pic14.c so they cannot disagree with the compiler; PIC18 + classic 14-bit cores),
`uart` (TXREG capture), `build REF` (clean out-of-tree git-worktree
build, cached), `pin-check WITNESS --old REF` (prove a witness fails
the pre-fix build — the j50 lesson), `cost --old REF` (compile-time
canary on the corpus giants — the 13x lesson).

# sims

Simulator regression suites with runtime checkpoint validation:
gpsim (PIC10-PIC18), QEMU pic32 (MIPS32) and QEMU esp32 (Xtensa).

- `gpsim/`, `pic32/`, `esp32/` — suite test programs (one .jal per test)
- `shared/` — common regression bodies, device definitions and the
  checkpoint framework, resolved through the `-s` include path
- `uart_txreg/` — cross-arch UART TXREG runtime checks
- `golden/` — task-codegen golden canaries (checked-in HEX fixtures for
  `test_task_codegen_golden.sh`)
- `run_regression_suite.py` / `run_pic32_regression.py` /
  `run_esp32_regression.py` — suite drivers (start here, `--help`)
- `jal_*_validate.sh` + `*_engine.py` — per-simulator validate harnesses
- `pic32_qemu.py` / `esp32_qemu.py` / `gpsim_*.py` — simulator helpers

## The grid

| tree | engine | machines |
|---|---|---|
| `gpsim/` | gpsim | `p12f508`, `p12f509`, `p12f1840`, `p16f877`, `p16f887`, `p16f1825`, `p18f4620`, `p18f26k22` |
| `pic32/` | QEMU | `pic32mz-explorer16`, `pic32mx7-explorer16` |
| `esp32/` | QEMU | `esp32` |

Suites and their file counts are in `run_regression_suite.py` (`SUITES` +
`SUITE_RUN_ORDER`); `sims/reports/gpsim_regression_summary.txt` records the
last run.  A test is one `.jal`; it passes only when the RAM values in its own
`@gpsim_validate` / `@sim_validate` block match.

```jal
-- @gpsim_validate begin
-- steps: 200000          -- gpsim instruction steps
-- frequency_hz: auto     -- from the device clock
-- expect byte v__check_step 11
-- expect dword_le v__d 50000001
-- @gpsim_validate end
--
include regression_16f877_device     -- device + pragmas + enable_digital_io
include regression                   -- helpers: regression_begin_run/
                                     -- checkpoint/pass_led_if_clean
include _regression_pic14_core_body  -- the logic
```

QEMU tests use `@sim_validate` with `machine:` and `duration_s:` instead of
`steps:`/`frequency_hz:`.  `duration_s` is a **wall-clock free-run window**
(not a timeout), so it must cover the slowest machine: the ESP32 tests need
about 3 s each and declare 5 s.

## Body families and cross-core reuse

* `_regression_pic14_*_body.jal` — the classic-midrange grid, run by the
  `12f1840_*`, `16f1825_*` and `16f877_*` wrappers (same logic, three
  devices; 12F1840 is a PIC12 part with the 14-bit core).
* `_regression_pic18_*_body.jal` — the PIC18 grid, run by the `18f4620_*`
  and `18f26k22_*` wrappers.
* `_regression_<feature>_body.jal` — family-neutral bodies (`bit2d`,
  `boundary`, `crc`, `dead_code`, `concat_expr`, `const_table_subscript`,
  `delay_timing`, `eeprom`, `fixed_string`, `float_ieee`, `indirect_args`,
  `large_array`, `multidim_array`, `multidim_wide`, `native_bit_array`,
  `optional`, `optsem`, `ptr_*`, `sdiv`, `string_subscript`, `tail_return`,
  `task_locals`, `taskctl`, `wide_*`).

The 32-bit engines reuse the family-neutral bodies and *either* the `pic14_*`
bodies (where the logic is core-agnostic) *or* their own `_regression_pic32_*`
twins.  That is why the `pic18_*` bodies run only on PIC18 — each has a
32-bit counterpart under a different name:

| PIC18 body (PIC_16 only) | 32-bit counterpart |
|---|---|
| `pic18_jallib` | `pic14_jallib` |
| `pic18_pragma` | `pic14_pragma` |
| `pic18_queues_more` | `pic14_queues_more` |
| `pic18_lib_misc` | `pic14_lib_misc` |
| `pic18_misc` | `pic32_misc` |
| `pic18_task` | `pic32_task` |
| `pic18_sample_onchip` | `pic32_sample_onchip` |
| `pic18_sample_pwm` | `pic32_sample_pwm` |

`float_ieee`, `multidim_wide`, `ptr_slots`, `sdiv`, `wide_muldiv` and
`task_locals` are **32-bit only** by construction (documented in their
headers).

## The 12-bit core

`12F508`/`12F509` have no FSR/INDF, no PCLATH, no strings and 25-41 bytes of
RAM, so `include regression` does not fit (its pinned GPRs sit at 0x77/0x78).
Those tests declare `checkpoint_verify: off` and use their own result cells:

```jal
include regression_12f508_device
var volatile byte r_flow = 0
-- expect byte v_r_flow 10
```

## Validate-block rules

* Every test needs a `@gpsim_validate` / `@sim_validate` block.
* The block must pin something a stalled program cannot satisfy — a
  checkpoint-counter expectation or a nonzero value
  (`unit-tests/check_validate_contract.py`).
* `expect byte v__check_step N` must match the number of
  `regression_checkpoint()` calls unless `checkpoint_verify: off` is
  declared (enforced by the suite runner).
* Exemptions — `skip_gpsim: 1` (compile/config only), `checkpoint_verify: off`
  and `allow_compiler_warnings: 1` — are pinned in
  `unit-tests/validate_exemptions_baseline.txt`; a NEW one fails the pin gate
  (`unit-tests/check_validate_exemptions.py`) until it is re-pinned
  deliberately.

## Known simulator limitations

* **No `p18f67j50` model** — that part is compile-swept and run on the bench
  tour only; PIC_16 *runtime* rests on 18F4620/18F26K22.
* **`p16f887` cannot log TXREG** — its USART exists but `log w txreg` captures
  nothing, so the 16F887 tests are RAM-checked only and print text is
  UART-verified on 16F877/18F4620.
* **`p18f4620` EEPROM reads return 0** for a seeded part (the baseline
  compiler behaves the same) — its EEPROM test is `skip_gpsim`; 18F26K22
  carries the PIC18 runtime witness.
* **`p16f887` WDT** resets on expiry but its timeout does not track
  `WDTCON_WDTPS`, so WDT timing stays a hardware/sample check.
* **Classic mid-range has no STKPTR** — `expect_hw_stkptr_le_compiler_depth`
  reports `SKIP` there instead of measuring a stack depth.

## Adding a device

Give it a `regression_<device>_device.jal` bundle in `shared/`, one wrapper per
body, and a suite entry in `run_regression_suite.py` (`SUITES` and
`SUITE_RUN_ORDER`).  QEMU boards live in `run_pic32_regression.py` /
`run_esp32_regression.py` and their `*_qemu.py` machine tables.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.

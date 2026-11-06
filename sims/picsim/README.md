# sims/picsim — 16F18857 runtime suite (THE midrange gate)

Runs JAL tests on **picsim** (the fork with the PIC16F18857 model) and checks
RAM, pins and UART text.

## Contract

```jal
-- @picsim_validate begin
-- steps: 20000
-- device: PIC16F18857        (optional; default PIC16F18857)
-- expect byte v_a 0x13
-- expect word v_ww 0x00A8
-- expect dword_le v_d 0x0001B2C3
-- expect bit_clear v__bitbucket 3
-- expect bit_set v__bitbucket 5
-- poke: v_poison 0x5C
-- pin_flips: RA0 16
-- apin: RB3 2.5
-- uart_contains: ##PICSIM##
-- @picsim_validate end
```

* `expect` symbols are resolved against the compiled `.asm` `EQU` lines
  (`v_<name>` for a JAL variable), byte/word/dword little-endian; plain
  `dword` is accepted as `dword_le` (gpsim-block parity); `bit_clear` /
  `bit_set` take the BIT NUMBER as the value field.
* `poke:` writes a byte into RAM after load and before the first step
  (poison-a-cell-then-run proofs; `sym+N` offsets resolve like expects).
* `pin_flips` counts level changes on a port pin over the run.
* `apin` drives an analog input (volts) every step.
* `uart_contains` captures the EUSART (pty) and asserts a substring.

## Coverage

127 tests: the 12F/16F1xxx compiler-regression bodies (30 `*_reg_*`
wrappers), the full retired 16F1825/16F877 feature set ported 2026-11-06
(all 155 deleted witnesses mapped -- the first sweep's list was built
from `git show --stat`, whose path truncation hid 16 of them; the
re-audit ported those too)
(strings/pointers/text-casts/native arrays/open arrays/ISRs/whereis...),
the bit-array and dynamic-store witnesses, and the 18857-specific
core/bank64/adc/uart/blink bodies.  Port notes live in each file's
header; the two Timer0-triggered 16f877/16f1825 witnesses
(`timer0_pointer_irp`, `isr_tmr0`) latch the INT flag in software
because picsim has no 18857 Timer0 model — the vector delivery, ISR
context and pointer traffic they pin are unchanged.

## Dependencies

* `picsim` built from the fork carrying the 16F18857 model + the
  15-bit-PC / MOVLB / 64-bank fixes + the **banked PCL write mirror fix**
  (the core registers 0x00-0x0F mirror into EVERY bank on 16F1xxx; the
  write hook compared pointers, so a banked `ADDWF PCL,f` — exactly what
  the jalv3 RETLW lookup tables emit with BSR!=0 — wrote the mirror and
  fell through to the next opcode; every literal staged as its first
  byte.  Found by the string-clone witness 2026-11-05; the fix lives in
  `picsim-18857.patch` with the model).  `PICSIM_DIR` must point at its
  source tree, with `src/libpicsim.*` built.
* `sims/picsim_runner.c` — a **GPL-2.0** helper (it links libpicsim). It is a
  standalone test binary, not linked into the compiler.
* `jalv3` in `bin/beta/`.

## Run

```sh
PICSIM_DIR=/path/to/picsim python3 sims/run_picsim_regression.py
# or one test:
PICSIM_DIR=/path/to/picsim python3 sims/picsim_validate_engine.py sims/picsim/16f18857_picsim_core.jal
```

Status: wired into `unit-tests/run_gates.sh` (runs when `PICSIM_DIR`
is set; skipped with a note otherwise).  Since 2026-11-05 this suite is
the midrange runtime gate: the 16F1825/16F877 gpsim suites were RETIRED
and their unique witnesses (user arrays over banks / passed on, string
heap over a bank, the in-direction payload view, pointers across banks,
native multibank arrays, the string-compare family) are PORTED here.
Known limits: libpicsim has no I2C/SPI slave models, so peripheral-init
samples that wait for a device stop there; timing/peripheral fidelity beyond
RAM/UART/ADC/pins needs per-test validation.

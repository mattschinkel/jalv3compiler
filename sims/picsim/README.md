# sims/picsim — 16F18857 runtime suite (experimental)

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
-- pin_flips: RA0 16
-- apin: RB3 2.5
-- uart_contains: ##PICSIM##
-- @picsim_validate end
```

* `expect` symbols are resolved against the compiled `.asm` `EQU` lines
  (`v_<name>` for a JAL variable), byte/word/dword little-endian.
* `pin_flips` counts level changes on a port pin over the run.
* `apin` drives an analog input (volts) every step.
* `uart_contains` captures the EUSART (pty) and asserts a substring.

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
is set; skipped with a note otherwise).
Known limits: libpicsim has no I2C/SPI slave models, so peripheral-init
samples that wait for a device stop there; timing/peripheral fidelity beyond
RAM/UART/ADC/pins needs per-test validation.

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

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.

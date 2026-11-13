# ESP32 second-core (APP core) — validation records

Phase 9 (SMP branch). The SMP witnesses live in the ESP32 suite
(`sims/esp32/esp32_body_smp_*.jal`) and run under QEMU and on silicon.
This directory keeps the silicon bring-up witness and the record.

* **QEMU runs the second core.** With the dispatcher fixes the esp32
  machine boots the APP core through the DPORT release and both cores run
  the dispatcher; the suite bodies assert both cores claim tasks, so a run
  where the second core never claims fails. The earlier "QEMU never runs
  the released core" conclusion was an artifact of the compiler bugs
  (release before `.bss` zeroing, no ATOMCTL, the `BNE` operand order, the
  missing main-slot store).
* **SILICON 2026-11-12 (boot).** `esp32_silicon_second_core.jal` prints
  `SC-BOOT`, the register dump, then `SC-APP-OK`. The DPORT map is
  CTRL_A[0]=RESETTING, CTRL_B[0]=CLKGATE_EN, CTRL_C[0]=RUNSTALL,
  CTRL_D=BOOT_ADDR (no OVLD bit), plus the RTC_CNTL_SW_CPU_STALL[7:0]
  clear the ROM requires. The release is emitted at the end of the PRO
  startup, after the `.bss` zeroing loop.
  Note: `app_core_hits` (the boot proof) is bumped by the APP entry and
  can race main's initializer, which writes 0 to it; the witness is a
  bring-up record, not a stable suite check. The suite's proofs are the
  scheduler's own counters.
* **SILICON 2026-11-12 (SMP).** The five suite bodies print `SMP-OK`,
  `SMPC-OK`, `SMPX-OK`, `SMPA-OK` and the core-range error text on the
  owner's board, stable over repeated resets. `S32C1I` needs
  `ATOMCTL = 0x2A` on this part: the reset 0x28 and the RCW value 0x29 both
  return the old word without storing.

## Enabling

```jal
pragma target enable_cores 2
```

No `pragma target second_core` and no `pragma task` anymore: the core count
is one pragma, and the task slot table is sized from the task declarations.
See `doc/dita/jalv3/second_core.dita` for the model, the
`_task_start core(...)` clause and the `_task_core(name())` read.

## Running a witness on the board

Build with the serial includes, convert the HEX, and flash the full image
at 0:

```
bin/beta/jalv3-x86-64 -s "lib;demo_lib;sims/shared;third_party/jallib/lib" \
  -no-codfile -asm /tmp/img.asm -hex /tmp/img.hex sims/esp32/esp32_body_smp_dispatch.jal
python3 sims/esp32_hex2flashbin.py /tmp/img.hex /tmp/img.bin
esptool.py --chip esp32 --port /dev/ttyUSB0 --after no-reset \
  write-flash --flash-mode dout --flash-freq 40m 0x0 /tmp/img.bin
```

The converter produces the full 4 MB flash image with the app already at
0x1000, so it is written at 0x0. Reset with an RTS pulse and capture
UART0 at 115200 (an open serial reader blocks esptool: kill it first).

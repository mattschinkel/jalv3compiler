# JALv3 Compiler

JALv3 is a fork of the JALv2 compiler, originally developed by Kyle A. York. 
JAL is a high-level programming language designed to simplify development 
for Microchip PIC microcontrollers by abstracting away many of the complexities 
of low-level hardware programming. Its syntax is loosely based on Pascal.

**About half of this compiler is new or modified code.** Measured against the last
JALv2 release (2.5r9) it is based on, roughly **50% of the code by line
count** in `src/` is new fork work (51,354 of 101,276 lines), including
two complete back ends. Original copyright notices are retained in every
derived file; modified files carry a Co-author or New author: Matthew Schinkel mark.
See NOTICE for full attribution.

## New in JALv3 — what this means for your programs

- **New target families: PIC32 (MIPS32) and ESP32 (Xtensa LX6)** — in
  addition to the original PIC10–PIC18, with device files generated from
  Microchip DFP data
- **Full pointer support** — data, procedure and function pointers,
  arrays of pointers, `WHEREIS` address-of, passing variables by
  reference through pointer formals
- **Dynamic strings** — `var string` with a compacting heap and `&`
  concatenation, plus fixed-size `var string[N]` with well-defined
  overflow behavior
- **Native bit arrays** (`var bit flags[32]`) and **multidimensional
  arrays**
- **Wide integers up to 128 bits** (`dword`, `qword` and beyond)
- **Cooperative multitasking** — `pragma task N`, `task name() is …`,
  `_task_start`, `_task_yield`
- **Optional procedure parameters** with `default` values
- **IEEE-754 floating point** on every target (round-to-nearest-even,
  gradual underflow)
- **Text conversion casts** — `string(x)`, `hex(x)`, `bin(x)` — and
  `COUNT()` for array/string lengths
- Prebuilt **Linux and Windows x86-64 binaries** in [bin/](bin/), with
  the historical jalv2 releases preserved in `bin/old_versions/`

## Device files and libraries

Device files and include files for classic PICs live in the jallib
repository: https://github.com/jallib/jallib — some samples and fork 
libraries built for jalv3 are in [demo_sample/](demo_sample/) and
[demo_lib/](demo_lib/).

## Building and testing

- Build the compiler: `make -C src` (output: `bin/jalv3-x86-64`;
  cross-build Windows with `make -C src ARCH=win64`)
- Fetch the pristine jallib corpus for the tests:
  `unit-tests/fetch_jallib.sh`
- Run the tests: [unit-tests/](unit-tests/) (corpus-compile gate) and
  [sims/](sims/) (gpsim / QEMU regression suites). Current status,
  all green: **2937/2937 unit, 196/196 gpsim, 170/170 PIC32 QEMU,
  47/47 ESP32 QEMU**

See [README.txt](README.txt) for the release history.

Have fun!

Copyright (c) 2026 Matthew Schinkel (fork), with original work by
Kyle A. York and the jallib authors — see [NOTICE](NOTICE).

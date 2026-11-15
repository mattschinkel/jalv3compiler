# JALv3 Compiler

JALv3 is a fork of the JALv2 compiler, originally developed by Kyle A. York. 
JAL is a high-level programming language designed to simplify development 
for Microchip PIC microcontrollers by abstracting away many of the complexities 
of low-level hardware programming. Its syntax is loosely based on Pascal.

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
- **Cooperative multitasking** — `task name() is …` (slots are sized
  automatically), `_task_start`, `_task_yield`
- **Multiple cores and task affinity (ESP32)** — `pragma target enable_cores 2`
  releases the second core; both run the task scheduler and
  `_task_start core(2) name()` pins a task to a core (constant or runtime
  pin; `_task_core(name())` reports where it last ran)
- **Optional procedure parameters** with `default` values
- **IEEE-754 floating point** on every target (round-to-nearest-even,
  gradual underflow)
- **Runtime error hook** — define `procedure _error_hook()` and the compiler
  calls it wherever a runtime error is raised (string heap full, string
  overflow, string invalid, core pin out of range), with `_error_source`
  and the domain cell (`_string_error`, `_core_error`) set;
  the flags are cleared when the hook returns. No hook defined = no cost, and
  the error flags stay available for polling
- **Runtime error hook library** — `include error_hook` gives you a ready-made
  `_error_hook()`; tailor it with `alias _error_bit is <bit>` (latch a flag)
  and/or `alias _error_device is <device>` (print the error), defined before
  the include
- **Text conversion casts** — `string(x)`, `hex(x)`, `bin(x)` — and
  `COUNT()` for array/string lengths
- Prebuilt **Linux and Windows x86-64 binaries** in [bin/beta/](bin/beta/),
  with the historical jalv2 releases preserved in `bin/old_versions/`

## Device files and libraries

Device files and include files for classic PICs live in the jallib
repository: https://github.com/jallib/jallib — some samples and fork 
libraries built for jalv3 are in [demo_sample/](demo_sample/) and
[demo_lib/](demo_lib/).

## Building and testing

- Build the compiler: `make -C src` (output: `bin/beta/jalv3-x86-64`;
  cross-build Windows with `make -C src ARCH=win64`, output:
  `bin/beta/jalv3-x86-64.exe`)
- Fetch the pristine jallib corpus for the tests:
  `unit-tests/fetch_jallib.sh`
- Run the tests: [unit-tests/](unit-tests/) (corpus-compile gate) and
  [sims/](sims/) (gpsim / QEMU regression suites). Current status,
  all green: **2937/2937 unit, 196/196 gpsim, 170/170 PIC32 QEMU,
  47/47 ESP32 QEMU**

**About half of this compiler is new or modified code.** Measured against the last
JALv2 release (2.5r9) it is based on, roughly **50% of the code by line
count** in `src/` is new fork work (51,354 of 101,276 lines), including
two complete back ends. Original copyright notices are retained in every
derived file; modified files carry a Co-author or New author: Matthew Schinkel mark.
See NOTICE for full attribution.

Have fun!

Copyright (c) 2026 Matthew Schinkel (fork), with original work by
Kyle A. York and the jallib authors — see [NOTICE](NOTICE).

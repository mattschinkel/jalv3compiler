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
- **Runtime error hook** — define `procedure _error_hook()` and the compiler
  calls it wherever a runtime error is raised (string heap full, string
  overflow, string invalid), with `_error_source` and `_string_error` set;
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

### Runtime error hook

A failed dynamic-string operation sets `_string_error` (0 none, 1 heap full,
2 overflow, 3 invalid) and `_error_source`. Define a procedure named
`_error_hook` and the compiler calls it at the point of failure — no
parameters, no registration; the name is the opt-in. The flags are cleared
when the hook returns (delivered once, then gone); with no hook they are left
set for polling.

```jal
-- define it before the code that can fail (after the include it needs)
procedure _error_hook() is
   var byte err
   if _error_source == _ERROR_SOURCE_STRING then
      err = _string_error                   -- read ONCE: the read clears it
      if err != _STRING_ERROR_NONE then
         if err == _STRING_ERROR_HEAP_FULL then
            print_string(serial, "[heap full]")
         else
            print_string(serial, "[string error: ")
            print_byte_dec(serial, err)
            print_string(serial, "]")
         end if
         print_crlf(serial)
      end if
   end if
end procedure
```

Two things to keep in mind: `_string_error` is consume-on-read, so read it into a
variable first and compare that (testing the cell and then reading it sees 0);
and JAL has no `else if` — nest the second test in an `else` block.

A ready-made handler lives in `demo_lib/error_hook.jal` — include it instead of
writing your own, and customise it with two optional aliases defined before the
include:

```jal
include print
alias _error_device is serial_data     -- print the error there
alias _error_bit    is led             -- latch a flag/LED (cleared by the app)
include error_hook
```

The handler reports every error the compiler can raise (`[string heap full]`,
`[string overflow]`, `[string invalid]`, any other code as `[string error: N]`,
`[user error]` for application raises, and `[error: N]` for future subsystems)
through the aliased device, sets the aliased bit (NOT cleared: it is a latched
flag), and the error cells are cleared when the handler returns. `demo_sample/16f1825_error_hook_library.jal`
is a complete example.

Constants: `_STRING_ERROR_NONE`/`_HEAP_FULL`/`_OVERFLOW`/`_INVALID` and
`_ERROR_SOURCE_STRING`/`_ERROR_SOURCE_USER` (JAL identifiers are
case-insensitive). Application code can raise through the same hook:
`_error_source = _ERROR_SOURCE_USER`, `_error_hook()`, `_error_source = 0`.

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

See [README.txt](README.txt) for the release history.

Have fun!

Copyright (c) 2026 Matthew Schinkel (fork), with original work by
Kyle A. York and the jallib authors — see [NOTICE](NOTICE).

JalV2 README
============
Included in this archive:

README.txt        : this file
src/              : compiler sources (build with make in src/)
bin/              : jalv3-x86-64 (Linux) and jalv3-x86-64.exe (Windows);
                    bin/old_versions/ holds the jalv2 release binaries
demo_lib/         : fork libraries and 32-bit device files
demo_sample/      : sample programs
sims/             : gpsim / QEMU simulator regression suites
unit-tests/       : corpus-compile unit gate (see README there)

Compiler issues can be registered and found at:
https://github.com/mattschinkel/jalv3compiler/issues

For device files and include files visit:
https://github.com/jallib/jallib/tree/master/include/device

History
=======
Forked from jalv2 2.5r9 (12 january 2025). The earlier jalv2 release
history (0.90 .. 2.5r9) is preserved in the upstream repository:
https://github.com/jallib/jalv2compiler

3.0a1-beta -- 29 october 2026
         * JALv3: new PIC32 (MIPS32) and ESP32 (Xtensa LX6) back ends
         * full pointer support (data/proc/func pointers, arrays of
           pointers, WHEREIS, open bit formals)
         * dynamic string heap with compaction; fixed var string[N]
         * native bit arrays, multidimensional arrays, 128-bit integers
         * shared libcore algorithm kernels across all back ends
         * cooperative multitasking, optional parameters, IEEE754 floats
         * simulator regression suites (gpsim, QEMU) and a corpus
           unit gate under sims/ and unit-tests/

/* mips_clock.h -- PIC32MZ clock-tree solver (pragma target OSC/clock)
 *
 * Part of the jalv3compiler fork.  Author: Matthew Schinkel, 2026.
 * Design: docs/investigations/pic32_clock_select_plan.md
 */
#ifndef MIPS_CLOCK_H__
#define MIPS_CLOCK_H__

#include "../libcore/pfile.h"

/* Called from the front end right after `pragma target clock` / `OSC` /
 * `POSC` when the target CPU is MIPS32.  No-op unless `target_osc` is
 * defined; solves the DEVCFG1/2 clock fields from target_clock (+ POSC
 * for POSC* modes) and materializes the _osc_* consts the device-file
 * osc_select() helper composes at run time.  Returns FALSE only on an
 * unsolvable request (error already logged). */
boolean_t mips_clock_solve(pfile_t *pf);

/* Solved config words for the HEX writer (valid after a successful
 * solve); dcfg[0..3] map to DEVCFG0..3 at MIPS_FLASH_BASE+0xFFC0. */
boolean_t mips_clock_fuses_get(unsigned long dcfg[4]);

/* Bits of DEVCFG word `word` (0..3) that the clock solver and the
 * WDT/DMT/DEBUG/IESO/SOSC/MCLR keyword pragmas compute.  The hex writer
 * refuses device-file fuse_def overrides that touch these (the user
 * expresses them through `pragma target clock`/`OSC`/keywords instead).
 * Family-aware: MZ EF and MX1/2 field positions differ. */
unsigned long mips_clock_owned_mask(pfile_t *pf, int word);

#endif

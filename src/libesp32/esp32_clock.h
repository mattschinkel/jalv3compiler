/************************************************************
 **
 ** esp32_clock.h : ESP32 clock-tree solver (pragma target OSC/clock)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** Design: docs/investigations/pic32_clock_select_plan.md + the ESP32
 ** addendum in fixes/fix_pic32_clock_select.md (same user contract).
 **
 ************************************************************/
#ifndef ESP32_CLOCK_H__
#define ESP32_CLOCK_H__

#include "../libcore/pfile.h"

/* Called from the front end after `pragma target clock` / `OSC` /
 * `oscillator` when the target CPU is ESP32.  No-op unless `target_osc`
 * is defined; validates the (mode, target_clock[, oscillator]) pair and
 * materializes the _esp32_clk_* consts demo_lib/esp32_clock.jal's
 * esp32_clock_init() composes at run time.  Returns FALSE only on an
 * unsolvable request (error already logged). */
boolean_t esp32_clock_solve(pfile_t *pf);

#endif

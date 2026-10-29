/* esp32_clock.c -- ESP32 clock validation + solved-field export
 *
 * Part of the jalv3compiler fork.  Author: Matthew Schinkel, 2026.
 *
 * The ESP32 clock tree has no user fuses -- configuration is runtime
 * registers only, so unlike the PIC32 solver there are no DEVCFG words
 * to emit.  This validates the pragma pair against the discrete,
 * hardware-verified operating points of demo_lib/esp32_clock.jal and
 * exports the fields its single esp32_clock_init() entry composes:
 *
 *   CRYSTAL : 40 MHz module crystal direct (the cold boot state -- the
 *             init call is a no-op, zero code needed)
 *   PLL     : 80 / 160 / 240 MHz from the 480 MHz BBPLL (div 6/3/2;
 *             core voltage 1.25 V at 240, 1.10 V otherwise)
 *
 * The 8 MHz internal RC (CK8M) source is deliberately NOT offered: the
 * switch sequence is unverified on hardware (see the fix doc).
 */
#include <errno.h>
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/variable.h"
#include "../libcore/value.h"
#include "../libcore/pf_log.h"
#include "esp32.h"
#include "esp32_clock.h"

/* mode values -- front-end keyword table (jal_incl.c) */
#define ESP32_OSC_CRYSTAL 2UL
#define ESP32_OSC_PLL     3UL

#define ESP32_XTAL_HZ 40000000UL

/* demo_lib/esp32_clock.jal pre-declares inert defaults under these
 * names (guarded by defined()); update in place when present. */
static void clk_const_alloc(pfile_t *pf, const char *name,
  unsigned long val)
{
  variable_def_t def;
  variable_t     var;

  var = pfile_variable_find(pf, PFILE_LOG_NONE, name, 0);
  if (var) {
    variable_const_set(var, variable_def_get(var), 0,
      (variable_const_t) val);
    variable_release(var);
    return;
  }
  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
    VARIABLE_DEF_FLAG_CONST, 1);
  if (!def) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  if (RESULT_OK != pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
      name, def, VARIABLE_NONE, &var)) {
    variable_def_free(def);
    return;
  }
  variable_const_set(var, def, 0, (variable_const_t) val);
  variable_release(var);
}

boolean_t esp32_clock_solve(pfile_t *pf)
{
  value_t        v;
  unsigned long  mode = 0;
  unsigned long  target = 0;
  unsigned long  xtal = ESP32_XTAL_HZ;
  unsigned long  div = 0;
  unsigned long  boost = 0;

  v = pfile_value_find(pf, PFILE_LOG_NONE, "target_osc");
  if (!v) {
    return BOOLEAN_TRUE;                  /* pragma not used: no-op */
  }
  mode = (unsigned long) value_const_get(v);
  value_release(v);

  v = pfile_value_find(pf, PFILE_LOG_NONE, "target_clock");
  if (v) {
    target = (unsigned long) value_const_get(v);
    value_release(v);
  }
  v = pfile_value_find(pf, PFILE_LOG_NONE, "target_oscillator");
  if (v) {
    xtal = (unsigned long) value_const_get(v);
    value_release(v);
  }

  /* order-lenient: clock may come after the OSC pragma; the clock
   * pragma re-solves. Incomplete-at-end errors in esp32_code_cleanup. */
  if (!target) {
    return BOOLEAN_TRUE;
  }

  switch (mode) {
    case ESP32_OSC_CRYSTAL:
      /* The ROM boots the CPU at its default 80 MHz PLL -- the crystal
       * is NOT the cold state -- and the XTAL-source switch sequence
       * is not yet hardware-verified (first attempt left the CPU on
       * the PLL; same class of measured quirk as the PLL flip's
       * read-and-double UART fix). Fence it instead of running 2x
       * slow against the declared clock. */
      pfile_log(pf, PFILE_LOG_ERR,
        "target OSC CRYSTAL: crystal-direct switching is not "
        "hardware-verified yet (the ROM default is the 80 MHz PLL); "
        "use PLL with 80/160/240 MHz");
      return BOOLEAN_FALSE;

    case ESP32_OSC_PLL:
      if (xtal != ESP32_XTAL_HZ) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC PLL: the BBPLL bring-up values are calibrated "
          "for the 40 MHz module crystal (pragma target oscillator "
          "says %lu Hz)", xtal);
        return BOOLEAN_FALSE;
      }
      switch (target) {
        case  80000000UL: div = 6; break;
        case 160000000UL: div = 3; break;
        case 240000000UL: div = 2; boost = 1; break;
        default:
          pfile_log(pf, PFILE_LOG_ERR,
            "target OSC PLL: %lu Hz is not an ESP32 CPU frequency "
            "(80_000_000, 160_000_000 or 240_000_000)", target);
          return BOOLEAN_FALSE;
      }
      break;

    default:
      pfile_log(pf, PFILE_LOG_ERR,
        "unknown target OSC mode %lu for ESP32 (CRYSTAL, PLL)", mode);
      return BOOLEAN_FALSE;
  }

  /* 80/160 need NO PLL programming: the cold-boot BBPLL is 320 MHz
   * (proven: the ROM boots at exactly 80.0 MHz = 320/4 with SEL=0 --
   * the min5 no-op measurement). Reprogramming it was the 80/160
   * killer; 80 = the boot state itself (selector 0, no PLL writes),
   * 160 = selector write only (320/2). Only 240 reprograms (->480). */
  clk_const_alloc(pf, "_esp32_clk_ready", 1);
  clk_const_alloc(pf, "_esp32_clk_div", div);
  clk_const_alloc(pf, "_esp32_clk_pll", (2 == div) ? 1 : 0);
  clk_const_alloc(pf, "_esp32_clk_boost", boost);
  return BOOLEAN_TRUE;
}

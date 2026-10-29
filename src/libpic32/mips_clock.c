/* mips_clock.c -- PIC32MZ EF clock-tree solver
 *
 * Part of the jalv3compiler fork.  Author: Matthew Schinkel, 2026.
 *
 * The user states the speed and the source:
 *   pragma target clock 200_000_000
 *   pragma target OSC    FRC_SPLL
 * and this file derives the DEVCFG1 FNOSC/FCKSM/POSCMOD and DEVCFG2
 * SPLL fields (DFP ATDF encodings; probe: PIC32MZ-EF_DFP 1.6.179),
 * emits them into the HEX, and materializes the _osc_* consts the
 * device-file osc_select() helper composes at run time.
 *
 * Encodings (PIC32MZ1024EFH064.atdf):
 *   FNOSC   (DEVCFG1 2:0)   FRCDIV=0 SPLL=1 POSC=2   (MZ: 110 reserved)
 *   FCKSM   (DEVCFG1 16:15) CSECMD=1 (switching must stay enabled)
 *   POSCMOD (DEVCFG1 13:12) EC=0 HS=2 OFF=3
 *   FPLLIDIV (DEVCFG2 2:0)  field = div-1 (DIV_1=0 .. DIV_8=7)
 *   FPLLRNG  (DEVCFG2 6:4)  1=5-10 2=8-16 3=13-26 4=21-42 5=34-68 MHz
 *   FPLLICLK (DEVCFG2 7)    1=FRC 0=POSC
 *   FPLLMULT (DEVCFG2 14:8) field = mult-1 (MUL_50=0x31)
 *   FPLLODIV (DEVCFG2 18:16) 2x=1 4x=2 8x=3 16x=4 32x=7  (no /1!)
 * Runtime SFR form (osc_select): SPLLCON PLLMULT field = mult (0x32
 * = x50, proven by SPLLCON 0x01320081 at 200 MHz); OSCCON NOSC:
 * FRC=0 SPLL=1 FRCDIV=7; FRCDIV divide = OSCCON<26:24>, 1..128 pow2.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../libutils/mem.h"
__extension__ typedef unsigned __int128 mips_clock_u128_t;
#include "../libcore/pfile.h"
#include "../libcore/variable.h"
#include "../libcore/value.h"
#include "../libcore/pf_log.h"
#include "mips.h"
#include "mips_clock.h"

/* device-file mode consts (demo_lib/32mz1024efh064.jal) */
#define OSC_MODE_FRC        1UL
#define OSC_MODE_FRCDIV     2UL
#define OSC_MODE_FRC_SPLL   3UL
#define OSC_MODE_POSC       4UL
#define OSC_MODE_POSC_SPLL  5UL
#define OSC_MODE_EC         6UL   /* external clock module into OSC1 */
#define OSC_MODE_EC_SPLL    7UL

#define FRC_HZ          8000000UL
/* PIC32MZ EF datasheet (DS60001320) SPLL FVCO range 340-700 MHz */
#define VCO_MIN     340000000UL
#define VCO_MAX     700000000UL

/* proven non-clock config content (10/10 HW-validated own-boot set);
 * dcfg[] is DEVCFG0..3 -- DEVCFG0 lands at 0xBFC0FFCC, DEVCFG3 at
 * 0xBFC0FFC0 (physical order reversed, DFP p32mz1024efh064.h) */
#define DCFG0_PROVEN   0x403FF753UL   /* DEVCFG0 @0xBFC0FFCC */
#define DCFG1_PROVEN   0x5F740439UL   /* DEVCFG1 @0xBFC0FFC8 (FNOSC..) */
#define DCFG2_FSRS     0x40000000UL   /* DEVCFG2 FSRS=4, proven set */
#define DCFG3_PROVEN   0x41000000UL   /* DEVCFG3 @0xBFC0FFC0 */
/* clock fields the MZ solve owns (DFP device-file masks): FNOSC 2:0,
 * POSCMOD 9:8, FCKSM 15:14 (CSECMD = 01).  The historical
 * 0x18000|0x3000 mask wrote FCKSM into bits 16:15 and POSCMOD into
 * 13:12 -- wrong fields that happened to be reserved/harmless. */
#define DCFG1_CLOCK_MASK (0x7UL | 0x300UL | 0xC000UL)

static unsigned long clock_dcfg[4];
static int           clock_dcfg_valid;

static unsigned long osc_input_hz(unsigned long mode, unsigned long posc_hz)
{
  if ((OSC_MODE_POSC == mode) || (OSC_MODE_POSC_SPLL == mode)
    || (OSC_MODE_EC == mode) || (OSC_MODE_EC_SPLL == mode)) {
    return posc_hz;
  }
  return FRC_HZ;
}

static int spll_band(unsigned long in_hz)
{
  /* overlapping bands: prefer the highest (8 MHz -> 8-16, matching the
   * proven DEVCFG2 0x400131A0 RNG code 2) */
  if (in_hz >= 34000000UL && in_hz <= 68000000UL) return 5;
  if (in_hz >= 21000000UL && in_hz <= 42000000UL) return 4;
  if (in_hz >= 13000000UL && in_hz <= 26000000UL) return 3;
  if (in_hz >= 8000000UL && in_hz <= 16000000UL) return 2;
  if (in_hz >= 5000000UL && in_hz <= 10000000UL) return 1;
  return 0;                       /* no SPLL input band contains it */
}

/* FPLLRNG describes the PLL's OWN input (after FPLLIDIV) */
static int spll_band_pll_in(unsigned long in_hz, unsigned long idiv)
{
  return spll_band(in_hz / idiv);
}

static unsigned long fpllodiv_code(unsigned long odiv)
{
  switch (odiv) {
    case 2:  return 1;
    case 4:  return 2;
    case 8:  return 3;
    case 16: return 4;
    case 32: return 7;
  }
  return 0;
}

static boolean_t is_pow2_1_128(unsigned long n)
{
  if (0 == n || n > 128UL) {
    return BOOLEAN_FALSE;
  }
  return (0UL == (n & (n - 1UL))) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* Solve in*mult/idiv/odiv == target with the VCO inside the window.
 * Preference: smallest output divider, then smallest input divider. */
static boolean_t spll_solve(pfile_t *pf, unsigned long in_hz,
  unsigned long target_hz, unsigned long *pidiv, unsigned long *pmult,
  unsigned long *podiv)
{
  static const unsigned long odivs[5] = { 2, 4, 8, 16, 32 };
  unsigned long odiv;
  unsigned long idiv;
  unsigned long mult;

  if (spll_band(in_hz) == 0) {
    pfile_log(pf, PFILE_LOG_ERR,
      "target OSC: %lu Hz is outside every SPLL input band "
      "(5-10, 8-16, 13-26, 21-42, 34-68 MHz)", in_hz);
    return BOOLEAN_FALSE;
  }
  for (odiv = 0; odiv < 5; odiv++) {
    for (idiv = 1; idiv <= 8; idiv++) {
      /* SYSCLK = in/idiv * mult / odiv  =>  mult = target*idiv*odiv/in */
      mips_clock_u128_t num =
        (mips_clock_u128_t) target_hz * idiv * odivs[odiv];
      unsigned long vco;

      if ((num % in_hz) != 0) {
        continue;                         /* mult would be fractional */
      }
      mult = (unsigned long) (num / in_hz);
      if (!mult || (mult > 128UL)) {
        continue;
      }
      vco = (in_hz / idiv) * mult;
      if ((vco < VCO_MIN) || (vco > VCO_MAX)) {
        continue;
      }
      *pidiv = idiv;
      *pmult = mult;
      *podiv = odivs[odiv];
      return BOOLEAN_TRUE;
    }
  }
  pfile_log(pf, PFILE_LOG_ERR,
    "target OSC: cannot produce %lu Hz from %lu Hz via the system PLL "
    "(FPLLIDIV 1-8, FPLLMULT 1-128, FPLLODIV 2-32, VCO %lu-%lu MHz)",
    target_hz, in_hz, VCO_MIN / 1000000UL, VCO_MAX / 1000000UL);
  return BOOLEAN_FALSE;
}

/* The device file pre-declares inert defaults for these (so osc_select's
 * body parses before any pragma); update the value when present. */
static void osc_const_alloc(pfile_t *pf, const char *name,
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
    VARIABLE_DEF_FLAG_CONST, 4);
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

/* User config pragmas (fix_pic32_clock_select.md addendum 3): mask the
 * requested bit into word w. Absent pragma = leave the proven default
 * already in clock_dcfg[w] (byte-identical images without pragmas). */
static void config_flag_apply(pfile_t *pf, const char *name,
  int w, unsigned long mask)
{
  value_t v = pfile_value_find(pf, PFILE_LOG_NONE, name);

  if (v) {
    if (value_const_get(v)) {
      clock_dcfg[w] |= mask;
    } else {
      clock_dcfg[w] &= ~mask;
    }
    value_release(v);
  }
}

/* field map, DFP device-file masks (32mz1024efh064.jal DEVCFGn_*):
 * word index matches clock_dcfg[] DEVCFG0..3.  Fixed 2026-10-24: the
 * historical constants were off-field (FWDTEN at a DMTCNT bit, FDMTEN
 * at a reserved bit, IESO/FSOSCEN four bits high, DBGPER two of three
 * bits) so every keyword pragma silently flipped the wrong field --
 * absent pragmas kept the proven words, which is why nothing broke.
 * Proven defaults below decode from DCFG0/1_PROVEN under the DFP
 * positions. */
#define CFG1_WDT   0x00800000UL   /* FWDTEN  bit 23 (proven: off)     */
#define CFG1_DMT   0x80000000UL   /* FDMTEN  bit 31 (proven: on)      */
#define CFG1_IESO  0x00000080UL   /* IESO    bit 7  (proven: off)     */
#define CFG1_SOSC  0x00000040UL   /* FSOSCEN bit 6  (proven: off)     */
#define CFG0_DBG   0x00007000UL   /* DBGPER  14:12 (proven: PG_NONE;
                                   * ENABLED sets PG_ALL, DISABLED -> 0) */
#define CFG0_MCLR  0x00008000UL   /* SMCLR   bit 15 (proven: off)     */
/* MZ DEVCFG2 SPLL fields the solver writes (device-file masks) */
#define DCFG2_FPLLIDIV 0x00000007UL
#define DCFG2_FPLLRNG  0x00000070UL
#define DCFG2_FPLLICLK 0x00000080UL
#define DCFG2_FPLLMULT 0x00007F00UL
#define DCFG2_FPLLODIV 0x00070000UL


/* ====================================================================
 * PIC32MX1/2 (MIPS32 M4K) clock tree
 *
 * Different part, same solver shape:
 *   FSYS = FRC(or POSC) / FPLLIDIV * FPLLMUL / FPLLODIV
 *   PLL input (FPLLIN)  3.92..5 MHz
 *   VCO (FSYS before FPLLODIV) 60..120 MHz
 *   FPLLIDIV codes 0..7 = 1,2,3,4,5,6,10,12
 *   FPLLMUL  codes 0..7 = 15,16,17,18,19,20,21,24
 *   FPLLODIV codes 0..7 = 1,2,4,8,16,32,64,256
 * FNOSC: FRC=0, FRCPLL=1, POSC=2, POSC/PLL=3, FRCDIV=7.
 * DEVCFG words live at 0xBFC00BF0..FC.  Base words cross-checked with
 * XC32 4.35 for this part (FRC /2 x20 /2 = 40 MHz).
 * ==================================================================== */

#define MX_FRC_HZ        8000000UL
#define MX_VCO_MIN       60000000UL
#define MX_VCO_MAX       120000000UL
#define MX_PLLIN_MIN     3920000UL
#define MX_PLLIN_MAX     5000000UL
#define MX_PLLIN_CENTER  4460000UL

/* XC32-checked base words: FWDTEN=0, WINDIS=1, FCKSM=CSECMD,
 * FPBDIV=1 (/2), POSCMOD=OFF, FNOSC=FRCPLL; DEVCFG2 /2 x20 /2 with
 * UPLLEN=1 (off), UPLLIDIV=1.  Unimplemented bits stay 1. */
#define MX_DCFG0_PROVEN  0x7FFFFFFBUL
#define MX_DCFG1_PROVEN  0xFF605F59UL
#define MX_DCFG2_PROVEN  0xFFF9F9D9UL
#define MX_DCFG3_PROVEN  0xFFFFFFFFUL

#define MX_DEVCFG1_FNOSC   0x00000007UL
#define MX_DEVCFG1_POSCMOD 0x00000300UL
#define MX_DEVCFG1_FCKSM   0x0000C000UL
#define MX_DEVCFG1_FSOSCEN 0x00000020UL
#define MX_DEVCFG1_IESO    0x00000080UL
#define MX_DEVCFG1_FWDTEN  0x00800000UL
#define MX_DEVCFG2_FPLLIDIV 0x00000007UL
#define MX_DEVCFG2_FPLLMUL  0x00000070UL
#define MX_DEVCFG2_FPLLODIV 0x00070000UL
#define MX_DEVCFG0_DEBUG    0x00000003UL

static const unsigned long mx_idivs[8] = { 1, 2, 3, 4, 5, 6, 10, 12 };
static const unsigned long mx_muls[8]  = { 15, 16, 17, 18, 19, 20, 21, 24 };
static const unsigned long mx_odivs[8] = { 1, 2, 4, 8, 16, 32, 64, 256 };

static unsigned long mx_absdiff(unsigned long a, unsigned long b)
{
  return (a > b) ? (a - b) : (b - a);
}

/* Solve input/idiv*mul/odiv == target.  Preference: smallest output
 * divider, then PLL input closest to the band centre (so a 20 MHz
 * crystal uses /5 = 4 MHz rather than the 5 MHz band edge), then
 * smallest multiplier. */
static boolean_t mx_pll_solve(pfile_t *pf, unsigned long in_hz,
  unsigned long target, unsigned long *pidiv_code, unsigned long *pmul,
  unsigned long *podiv)
{
  int oi;
  int ii;
  int mi;

  if (!in_hz) {
    pfile_log(pf, PFILE_LOG_ERR, "target OSC: no clock source frequency");
    return BOOLEAN_FALSE;
  }
  for (oi = 0; oi < 8; oi++) {
    unsigned long odiv = mx_odivs[oi];
    int order[8];
    int n = 0;
    int a;
    int b;

    for (ii = 0; ii < 8; ii++) {
      unsigned long idiv = mx_idivs[ii];
      unsigned long pin;

      if ((in_hz % idiv) != 0) {
        continue;
      }
      pin = in_hz / idiv;
      if ((pin < MX_PLLIN_MIN) || (pin > MX_PLLIN_MAX)) {
        continue;
      }
      order[n++] = ii;
    }
    for (a = 1; a < n; a++) {
      int k = order[a];
      unsigned long kd = mx_absdiff(in_hz / mx_idivs[k], MX_PLLIN_CENTER);

      for (b = a; b > 0; b--) {
        unsigned long bd =
          mx_absdiff(in_hz / mx_idivs[order[b - 1]], MX_PLLIN_CENTER);

        if (bd <= kd) {
          break;
        }
        order[b] = order[b - 1];
      }
      order[b] = k;
    }
    for (a = 0; a < n; a++) {
      unsigned long idiv = mx_idivs[order[a]];
      unsigned long pin  = in_hz / idiv;

      for (mi = 0; mi < 8; mi++) {
        unsigned long mul = mx_muls[mi];
        unsigned long vco = pin * mul;

        if ((vco < MX_VCO_MIN) || (vco > MX_VCO_MAX)) {
          continue;
        }
        if ((vco % odiv) != 0) {
          continue;
        }
        if ((vco / odiv) != target) {
          continue;
        }
        *pidiv_code = (unsigned long) order[a];
        *pmul       = (unsigned long) mi;   /* 3-bit code, not the value:
                                             * mx_muls = 15..21,24 */
        *podiv      = odiv;
        return BOOLEAN_TRUE;
      }
    }
  }
  pfile_log(pf, PFILE_LOG_ERR,
    "target OSC: cannot produce %lu Hz from %lu Hz on this PIC32MX "
    "(PLL input %lu-%lu Hz, FPLLIDIV 1,2,3,4,5,6,10,12; FPLLMUL "
    "15..21,24; FPLLODIV 1,2,4..256; VCO %lu-%lu MHz; SYSCLK max 40 MHz)",
    target, in_hz, MX_PLLIN_MIN, MX_PLLIN_MAX,
    MX_VCO_MIN / 1000000UL, MX_VCO_MAX / 1000000UL);
  return BOOLEAN_FALSE;
}

static boolean_t mx_pow2_1_256(unsigned long n)
{
  if (!n || (n > 256UL)) {
    return BOOLEAN_FALSE;
  }
  return (0UL == (n & (n - 1UL))) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static unsigned long mx_log2(unsigned long n)
{
  unsigned long c = 0;

  while (n > 1UL) {
    n >>= 1;
    c++;
  }
  return c;
}

/* MX runtime consts: OSCCON encodes PLLMULT as mult-15 and
 * PLLODIV/FRCDIV as log2 codes. */
static boolean_t mips_clock_solve_mx(pfile_t *pf)
{
  value_t       v;
  unsigned long mode = 0;
  unsigned long target = 0;
  unsigned long posc_hz = 0;
  unsigned long idiv_code = 0;
  unsigned long mult = 1;
  unsigned long odiv = 1;
  unsigned long frcdiv = 0;              /* code (0 = /1) */
  unsigned long fnosc;
  unsigned long poscmod = 3;
  unsigned long nosec;
  unsigned long dcfg1;
  unsigned long dcfg2;
  boolean_t     pll = BOOLEAN_FALSE;
  boolean_t     use_posc = BOOLEAN_FALSE;

  v = pfile_value_find(pf, PFILE_LOG_NONE, "target_osc");
  if (!v) {
    return BOOLEAN_TRUE;
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
    posc_hz = (unsigned long) value_const_get(v);
    value_release(v);
  }
  if (!target) {
    return BOOLEAN_TRUE;                 /* defer to the clock pragma */
  }

  switch (mode) {
    case OSC_MODE_FRC:
      fnosc = 0;
      nosec = 0;
      if (target != MX_FRC_HZ) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC FRC runs at %lu Hz; use FRCDIV or FRC_SPLL",
          MX_FRC_HZ);
        return BOOLEAN_FALSE;
      }
      break;
    case OSC_MODE_FRCDIV:
      fnosc = 7;
      nosec = 7;
      if (!target || (0 != (MX_FRC_HZ % target))
          || !mx_pow2_1_256(MX_FRC_HZ / target)) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC FRCDIV: %lu Hz unreachable from 8 MHz; FRC divides "
          "by powers of two from 1 to 256", target);
        return BOOLEAN_FALSE;
      }
      frcdiv = mx_log2(MX_FRC_HZ / target);
      break;
    case OSC_MODE_POSC:
    case OSC_MODE_EC:
      if (!posc_hz) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC %s needs `pragma target POSC <source Hz>`",
          (OSC_MODE_EC == mode) ? "EC" : "POSC");
        return BOOLEAN_FALSE;
      }
      if (target != posc_hz) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC %s: target clock must equal the source (%lu Hz); "
          "use %s for PLL speeds",
          (OSC_MODE_EC == mode) ? "EC" : "POSC", posc_hz,
          (OSC_MODE_EC == mode) ? "EC_SPLL" : "POSC_SPLL");
        return BOOLEAN_FALSE;
      }
      fnosc  = 2;
      poscmod = (OSC_MODE_EC == mode) ? 0 : 2;
      nosec  = 2;
      break;
    case OSC_MODE_FRC_SPLL:
      if (!mx_pll_solve(pf, MX_FRC_HZ, target, &idiv_code, &mult, &odiv)) {
        return BOOLEAN_FALSE;
      }
      pll    = BOOLEAN_TRUE;
      fnosc  = 1;
      nosec  = 1;
      poscmod = 3;
      break;
    case OSC_MODE_POSC_SPLL:
    case OSC_MODE_EC_SPLL:
      if (!posc_hz) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC %s needs `pragma target POSC <source Hz>`",
          (OSC_MODE_EC_SPLL == mode) ? "EC_SPLL" : "POSC_SPLL");
        return BOOLEAN_FALSE;
      }
      if (!mx_pll_solve(pf, posc_hz, target, &idiv_code, &mult, &odiv)) {
        return BOOLEAN_FALSE;
      }
      pll      = BOOLEAN_TRUE;
      use_posc = BOOLEAN_TRUE;
      fnosc    = 3;
      poscmod  = (OSC_MODE_EC_SPLL == mode) ? 0 : 2;
      nosec    = 3;
      break;
    default:
      pfile_log(pf, PFILE_LOG_ERR,
        "unknown target OSC mode %lu (FRC, FRCDIV, FRC_SPLL, POSC, "
        "POSC_SPLL, EC, EC_SPLL)", mode);
      return BOOLEAN_FALSE;
  }

  dcfg1 = (MX_DCFG1_PROVEN & ~(MX_DEVCFG1_FNOSC | MX_DEVCFG1_POSCMOD))
        | fnosc | (poscmod << 8);
  dcfg2 = MX_DCFG2_PROVEN;
  if (pll) {
    dcfg2 = (dcfg2 & ~(MX_DEVCFG2_FPLLIDIV | MX_DEVCFG2_FPLLMUL
                     | MX_DEVCFG2_FPLLODIV))
          | idiv_code | (mult << 4) | (mx_log2(odiv) << 16);
  }
  clock_dcfg[0] = MX_DCFG0_PROVEN;
  clock_dcfg[1] = dcfg1;
  clock_dcfg[2] = dcfg2;
  clock_dcfg[3] = MX_DCFG3_PROVEN;
  clock_dcfg_valid = 1;

  /* ---- config pragmas (MX field positions) ---- */
  {
    value_t d = pfile_value_find(pf, PFILE_LOG_NONE, "target_debug");

    if (d) {
      if (value_const_get(d)) {
        clock_dcfg[0] &= ~MX_DEVCFG0_DEBUG;   /* DEBUG=0: on */
      } else {
        clock_dcfg[0] |= MX_DEVCFG0_DEBUG;    /* DEBUG=3: off */
      }
      value_release(d);
    }
  }
  config_flag_apply(pf, "target_wdt", 1, MX_DEVCFG1_FWDTEN);
  config_flag_apply(pf, "target_ieso", 1, MX_DEVCFG1_IESO);
  config_flag_apply(pf, "target_sosc", 1, MX_DEVCFG1_FSOSCEN);
  {
    value_t d = pfile_value_find(pf, PFILE_LOG_NONE, "target_dmt");

    if (d) {
      pfile_log(pf, PFILE_LOG_WARN,
        "pragma target DMT: this PIC32 family has no Deadman Timer "
        "(pragma ignored)");
      value_release(d);
    }
    d = pfile_value_find(pf, PFILE_LOG_NONE, "target_mclr");
    if (d) {
      pfile_log(pf, PFILE_LOG_WARN,
        "pragma target MCLR: this PIC32 family has no SMCLR config bit "
        "(pragma ignored)");
      value_release(d);
    }
  }

  /* ---- runtime (OSCCON) fields for pic32_clock.jal ---- */
  osc_const_alloc(pf, "_osc_family", 2);
  osc_const_alloc(pf, "_osc_nosc", nosec);
  osc_const_alloc(pf, "_osc_frcdiv", frcdiv);
  osc_const_alloc(pf, "_osc_spll_iclk", use_posc ? 0 : 1);
  osc_const_alloc(pf, "_osc_spll_ready", pll ? 1 : 0);
  osc_const_alloc(pf, "_osc_pllrange", 0);
  osc_const_alloc(pf, "_osc_pllidiv", pll ? idiv_code : 0);
  /* mx_pll_solve returns the 3-bit FPLLMUL code (the mx_muls index),
   * the same code the DEVCFG2 fuse field and OSCCON PLLMULT<18:16>
   * take -- no -15 adjustment here. */
  osc_const_alloc(pf, "_osc_pllmult", pll ? mult : 0);
  osc_const_alloc(pf, "_osc_pllodiv", pll ? mx_log2(odiv) : 0);
  return BOOLEAN_TRUE;
}

boolean_t mips_clock_solve(pfile_t *pf)
{
  value_t        v;
  unsigned long  mode = 0;
  unsigned long  target = 0;
  unsigned long  posc_hz = 0;
  unsigned long  in_hz;
  unsigned long  idiv = 1;
  unsigned long  mult = 1;
  unsigned long  odiv = 2;
  unsigned long  frcdiv = 1;
  unsigned long  fnosc;
  unsigned long  poscmod = 0x3UL;         /* OFF */
  unsigned long  nosec;
  unsigned long  dcfg2;

  if (MIPS_FAMILY_MX1 == mips_family(pf)) {
    return mips_clock_solve_mx(pf);
  }

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
    posc_hz = (unsigned long) value_const_get(v);
    value_release(v);
  }

  /* order-lenient: `pragma target clock` may come AFTER the OSC mode
   * (the sample format puts oscillator first); defer silently and let
   * the clock-pragma re-solve complete it. A pragma that never
   * completes errors at hex-write time (mips_hex.c). */
  if (!target) {
    return BOOLEAN_TRUE;
  }

  in_hz = osc_input_hz(mode, posc_hz);
  nosec = 0;                              /* runtime NOSC: FRC */

  switch (mode) {
    case OSC_MODE_FRC:
      fnosc = 0;                          /* FRC (FRCDIV /1) */
      if (target != FRC_HZ) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC FRC runs at %lu Hz; use FRCDIV for divided or "
          "FRC_SPLL for PLL speeds", FRC_HZ);
        return BOOLEAN_FALSE;
      }
      break;

    case OSC_MODE_FRCDIV:
      fnosc = 0;
      nosec = 7;                          /* runtime NOSC: FRCDIV */
      if (!target || (0 != (FRC_HZ % target)) ||
          !is_pow2_1_128(FRC_HZ / target)) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC FRCDIV: %lu Hz unreachable from 8 MHz; FRC divides "
          "by powers of two from 1 to 128 (8 MHz down to 62500 Hz)",
          target);
        return BOOLEAN_FALSE;
      }
      frcdiv = FRC_HZ / target;
      break;

    case OSC_MODE_FRC_SPLL:
    case OSC_MODE_POSC_SPLL:
    case OSC_MODE_EC_SPLL:
      if (!target) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC: set `pragma target clock` before the OSC mode");
        return BOOLEAN_FALSE;
      }
      if ((OSC_MODE_POSC_SPLL == mode) || (OSC_MODE_EC_SPLL == mode)) {
        if (!posc_hz) {
          pfile_log(pf, PFILE_LOG_ERR,
            "target OSC %s needs `pragma target POSC <source Hz>`",
            (OSC_MODE_EC_SPLL == mode) ? "EC_SPLL" : "POSC_SPLL");
          return BOOLEAN_FALSE;
        }
        poscmod = (OSC_MODE_EC_SPLL == mode) ? 0x0UL : 0x2UL; /* EC : HS */
        nosec = 2;                        /* runtime NOSC: POSC */
      } else {
        nosec = 1;                        /* runtime NOSC: SPLL */
      }
      fnosc = 1;
      if (!spll_solve(pf, in_hz, target, &idiv, &mult, &odiv)) {
        return BOOLEAN_FALSE;
      }
      break;

    case OSC_MODE_POSC:
    case OSC_MODE_EC:
      if (!posc_hz) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC %s needs `pragma target POSC <source Hz>`",
          (OSC_MODE_EC == mode) ? "EC" : "POSC");
        return BOOLEAN_FALSE;
      }
      if (target != posc_hz) {
        pfile_log(pf, PFILE_LOG_ERR,
          "target OSC %s: target clock must equal the source "
          "(%lu Hz); use %s for PLL speeds",
          (OSC_MODE_EC == mode) ? "EC" : "POSC", posc_hz,
          (OSC_MODE_EC == mode) ? "EC_SPLL" : "POSC_SPLL");
        return BOOLEAN_FALSE;
      }
      fnosc = 2;
      poscmod = (OSC_MODE_EC == mode) ? 0x0UL : 0x2UL; /* EC : HS */
      nosec = 2;
      break;

    default:
      pfile_log(pf, PFILE_LOG_ERR,
        "unknown target OSC mode %lu (FRC, FRCDIV, FRC_SPLL, POSC, "
        "POSC_SPLL, EC, EC_SPLL)", mode);
      return BOOLEAN_FALSE;
  }

  clock_dcfg[0] = DCFG0_PROVEN;
  clock_dcfg[1] = (DCFG1_PROVEN & ~DCFG1_CLOCK_MASK)
                | fnosc                  /* FNOSC 2:0 */
                | (0x1UL << 14)          /* FCKSM = CSECMD (01) */
                | (poscmod << 8);        /* POSCMOD 9:8 */
  dcfg2 = DCFG2_FSRS
        | ((unsigned long) spll_band_pll_in(in_hz, idiv) << 4);
  if ((OSC_MODE_POSC == mode) || (OSC_MODE_POSC_SPLL == mode)
    || (OSC_MODE_EC == mode) || (OSC_MODE_EC_SPLL == mode)) {
    dcfg2 |= 0x0UL << 7;                  /* FPLLICLK: POSC */
  } else {
    dcfg2 |= 0x1UL << 7;                  /* FPLLICLK: FRC */
  }
  if ((OSC_MODE_FRC_SPLL == mode) || (OSC_MODE_POSC_SPLL == mode)
    || (OSC_MODE_EC_SPLL == mode)) {
    dcfg2 |= (idiv - 1)                   /* FPLLIDIV: field = div-1 */
           | ((mult - 1) << 8)            /* FPLLMULT: field = mult-1 */
           | (fpllodiv_code(odiv) << 16); /* FPLLODIV */
  } else {
    /* seed the SPLL inert (x1 in, /2 out): never selected at boot, and
     * harmless if something switches to it before reprogramming */
    dcfg2 |= 0x0UL | (0x0UL << 8) | (0x1UL << 16);
  }
  clock_dcfg[2] = dcfg2;
  clock_dcfg[3] = DCFG3_PROVEN;
  clock_dcfg_valid = 1;

  /* user config pragmas (absent = proven bits, above) */
  config_flag_apply(pf, "target_wdt", 1, CFG1_WDT);
  config_flag_apply(pf, "target_dmt", 1, CFG1_DMT);
  config_flag_apply(pf, "target_ieso", 1, CFG1_IESO);
  config_flag_apply(pf, "target_sosc", 1, CFG1_SOSC);
  config_flag_apply(pf, "target_debug", 0, CFG0_DBG);
  config_flag_apply(pf, "target_mclr", 0, CFG0_MCLR);

  /* runtime fields for the device-file osc_select() helper */
  osc_const_alloc(pf, "_osc_nosc", nosec);
  osc_const_alloc(pf, "_osc_frcdiv", is_pow2_1_128(frcdiv)
    ? frcdiv : 0);
  osc_const_alloc(pf, "_osc_spll_iclk",
    (OSC_MODE_POSC == mode || OSC_MODE_POSC_SPLL == mode) ? 0 : 1);
  osc_const_alloc(pf, "_osc_spll_ready",
    (OSC_MODE_FRC_SPLL == mode || OSC_MODE_POSC_SPLL == mode) ? 1 : 0);
  osc_const_alloc(pf, "_osc_pllrange",
    spll_band(in_hz) ? 1 : 0);          /* SPLLCON.PLlrANGE bit 0 */
  osc_const_alloc(pf, "_osc_pllidiv", idiv - 1);
  osc_const_alloc(pf, "_osc_pllmult", mult);  /* SFR form: field = mult */
  osc_const_alloc(pf, "_osc_pllodiv", fpllodiv_code(odiv));
  return BOOLEAN_TRUE;
}

boolean_t mips_clock_fuses_get(unsigned long dcfg[4])
{
  if (!clock_dcfg_valid) {
    return BOOLEAN_FALSE;
  }
  memcpy(dcfg, clock_dcfg, sizeof(clock_dcfg));
  return BOOLEAN_TRUE;
}

unsigned long mips_clock_owned_mask(pfile_t *pf, int word)
{
  if ((word < 0) || (word > 3)) {
    return 0;
  }
  if (MIPS_FAMILY_MX1 == mips_family(pf)) {
    static const unsigned long mx_owned[4] = {
      MX_DEVCFG0_DEBUG,
      MX_DEVCFG1_FNOSC | MX_DEVCFG1_POSCMOD | MX_DEVCFG1_FCKSM
        | MX_DEVCFG1_FWDTEN | MX_DEVCFG1_FSOSCEN | MX_DEVCFG1_IESO,
      MX_DEVCFG2_FPLLIDIV | MX_DEVCFG2_FPLLMUL | MX_DEVCFG2_FPLLODIV,
      0
    };

    return mx_owned[word];
  } else {
    static const unsigned long mz_owned[4] = {
      CFG0_DBG | CFG0_MCLR,
      DCFG1_CLOCK_MASK | CFG1_WDT | CFG1_DMT | CFG1_IESO | CFG1_SOSC,
      DCFG2_FPLLIDIV | DCFG2_FPLLRNG | DCFG2_FPLLICLK | DCFG2_FPLLMULT
        | DCFG2_FPLLODIV,
      0
    };

    return mz_owned[word];
  }
}

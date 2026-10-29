/************************************************************
 **
 ** mips_dev.c : PIC32 device family + part limits
 **
 ** The 32-bit back end was written for the PIC32MZ1024EFH064
 ** (MZ EF, 512 KB SRAM, PRECON, EVIC OFFx, shadow register
 ** sets).  The PIC32MX1/2 family (MIPS32 M4K) is the same
 ** integer core but a different part: 64 KB SRAM, a 3 KB boot
 ** flash, OSCCON PLL fields, a vector table instead of OFFx
 ** and no PRECON/PRISS.  Everything family-dependent is
 ** derived here from data the device file already carries:
 **
 **   _FUSE_BASE[4]  DEVCFG0..3 addresses -- the config block
 **                  address identifies the family (MZ EF has it
 **                  at the top of a 64 KB boot flash, MX1/2 in
 **                  the last 16 bytes of a 3 KB boot flash);
 **   pragma data    RAM extents (the top is the stack top);
 **   pragma code    program-flash size (_code_size).
 **
 ** No new device-file constants are needed, so existing MZ
 ** device files keep producing byte-identical images.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libcore/pfile.h"
#include "../libcore/value.h"
#include "../libcore/pf_log.h"
#include "mips.h"

/* DEVCFG0 address at or above this = MZ EF (config at 0xBFC0FFCC);
 * MX1/2 has it at 0xBFC00BFC. */
#define MIPS_MZ_CONFIG_MIN 0xBFC0F000UL

mips_family_t mips_family(pfile_t *pf)
{
  value_t       base;
  unsigned long devcfg0 = 0;

  /* Device file may declare the family explicitly (MX files do). */
  base = pfile_value_find(pf, PFILE_LOG_NONE, "_mips_family");
  if (base) {
    unsigned long fam = (unsigned long) value_const_get(base);

    value_release(base);
    if (2UL == fam) {
      return MIPS_FAMILY_MX1;
    }
    if (1UL == fam) {
      return MIPS_FAMILY_MZ_EF;
    }
  }

  base = pfile_value_find(pf, PFILE_LOG_NONE, "_fuse_base");
  if (base) {
    if (value_is_array(base) && (value_ct_get(base) >= 4)) {
      value_t e = value_subscript_set(base, 0);

      if (e) {
        devcfg0 = (unsigned long) value_const_get(e);
        value_release(e);
      }
    } else {
      devcfg0 = (unsigned long) value_const_get(base);
    }
    value_release(base);
  }
  if (devcfg0 >= MIPS_MZ_CONFIG_MIN) {
    return MIPS_FAMILY_MZ_EF;
  }
  if (devcfg0) {
    return MIPS_FAMILY_MX1;
  }
  /* No fuse data (hand-written device file): keep the historical
   * MZ behaviour, but let the presence of PRECON select MZ too. */
  {
    variable_t v = pfile_variable_find(pf, PFILE_LOG_NONE, "precon", 0);

    if (v) {
      variable_release(v);
      return MIPS_FAMILY_MZ_EF;
    }
  }
  return MIPS_FAMILY_MZ_EF;
}

/* Top of SRAM (stack grows down).  From the device file's highest
 * `pragma data` byte, rounded up to a word: 0x8007FFFE -> 0x80080000
 * (MZ, unchanged), 0x8000FFFF -> 0x80010000 (MX270F256B). */
unsigned long mips_ram_top(pfile_t *pf)
{
  unsigned long hi = pfile_data_high_get(pf);

  if ((hi >= MIPS_RAM_BASE) && (hi < 0xA0000000UL)) {
    return (hi + 4UL) & ~3UL;
  }
  return MIPS_STACK_TOP;
}

/* Highest data offset from MIPS_RAM_BASE, leaving a stack reserve.
 * 64 KB on a >=128 KB part (MZ: unchanged 0x70000), 8 KB otherwise
 * (MX270F256B: 0xE000).  mips_var.c compares offsets. */
unsigned long mips_data_max(pfile_t *pf)
{
  unsigned long top  = mips_ram_top(pf);
  unsigned long size = (top > MIPS_RAM_BASE) ? (top - MIPS_RAM_BASE) : 0;
  unsigned long reserve;

  if (size >= 0x20000UL) {
    reserve = 0x10000UL;
  } else {
    reserve = 0x2000UL;
  }
  if (size <= reserve) {
    reserve = size / 4UL;
  }
  return size - reserve;
}

/* Stack reserve (bytes) that mips_data_max leaves below ram_top. */
unsigned long mips_stack_reserve(pfile_t *pf)
{
  unsigned long top  = mips_ram_top(pf);
  unsigned long size = (top > MIPS_RAM_BASE) ? (top - MIPS_RAM_BASE) : 0;
  unsigned long reserve;

  if (size >= 0x20000UL) {
    reserve = 0x10000UL;
  } else {
    reserve = 0x2000UL;
  }
  if (size <= reserve) {
    reserve = size / 4UL;
  }
  return reserve;
}

/* pragma code size (program flash bytes); 0 when the device file has
 * no `pragma code`.  The generated PIC32 files always carry it. */
unsigned long mips_code_size(pfile_t *pf)
{
  value_t v = pfile_value_find(pf, PFILE_LOG_NONE, "_code_size");
  unsigned long sz = 0;

  if (v) {
    sz = (unsigned long) value_const_get(v);
    value_release(v);
  }
  return sz;
}

/* End of the app region for the current image shape:
 *   loader/PFM: app base + program-flash size (fallback old bound);
 *   boot flash: MIPS_FLASH_BASE + usable reset/BEV bytes (MX: 0xBF0,
 *               the config words start there; MZ: 0 = no bound).
 * Returns 0 when there is no bound. */
unsigned long mips_image_end(pfile_t *pf)
{
  if (mips_loader_pfm(pf)) {
    unsigned long sz = mips_code_size(pf);

    if (MIPS_FAMILY_MZ_EF == mips_family(pf)) {
      return MIPS_PFM_USB_TOP;   /* unchanged MZ USB-HID reserve */
    }
    if (sz) {
      return mips_code_base(pf) + sz;
    }
    return 0;
  }
  if (MIPS_FAMILY_MX1 == mips_family(pf)) {
    return MIPS_FLASH_BASE + 0xBF0UL;   /* DEVCFG3 starts here */
  }
  return 0;
}

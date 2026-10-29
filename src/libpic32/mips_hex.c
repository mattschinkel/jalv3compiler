/************************************************************
 **
 ** mips_hex.c : PIC32 (MIPS32) Intel HEX writer
 **
 ** Walks the laid-out mips_code_t list and emits one little-endian
 ** 32-bit instruction word per node at its boot-flash address
 ** (0xBFC00000 + offset). The libcore pfile_write_hex() helper
 ** handles the extended-linear-address records, line buffering
 ** and the trailing EOF record, so this writer only supplies
 ** raw bytes.
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libcore/pfile.h"
#include "../libcore/pf_log.h"
#include "mips.h"
#include "mips_clock.h"

/* Config word address for DEVCFG0..3.  The device file's _FUSE_BASE
 * array (already used for the defaults) is authoritative; fall back to
 * the MZ EF block top for hand-written device files without it. */
static unsigned long mips_fuse_addr(pfile_t *pf, int index)
{
  value_t       base;
  unsigned long addr = 0;

  base = pfile_value_find(pf, PFILE_LOG_NONE, "_fuse_base");
  if (base) {
    if (value_is_array(base)
        && (value_ct_get(base) > (variable_ct_t) index)) {
      value_t e = value_subscript_set(base, index);

      if (e) {
        addr = (unsigned long) value_const_get(e);
        value_release(e);
      }
    }
    value_release(base);
  }
  if (!addr) {
    addr = MIPS_FLASH_BASE + 0xFFC0UL
         + (unsigned long) ((3 - index) * 4);
  }
  return addr;
}

/* TRUE when the user's fuse pragmas touched any DEVCFG word (the front
 * end ORs each applied fuse_def field mask into _FUSES_EDIT -- see
 * jal_fuse_edit_mark). */
static boolean_t mips_fuse_user_changed(pfile_t *pf)
{
  value_t       edits;
  boolean_t     changed_any = BOOLEAN_FALSE;
  variable_ct_t ii;

  edits = pfile_value_find(pf, PFILE_LOG_NONE, "_fuses_edit");
  if (!edits || !value_is_array(edits)) {
    if (edits) {
      value_release(edits);
    }
    return BOOLEAN_FALSE;
  }
  for (ii = 0; ii < value_ct_get(edits); ii++) {
    value_t ve;

    ve = value_subscript_set(edits, ii);
    if (ve) {
      if (value_const_get(ve)) {
        changed_any = BOOLEAN_TRUE;
      }
      value_release(ve);
    }
  }
  value_release(edits);
  return changed_any;
}

/* Apply the user's device-file fuse_def edits onto the solved DEVCFG
 * words: for every field the front end marked in _FUSES_EDIT, take the
 * (already-mutated) _FUSES bits over the solved word.  Fields the clock
 * solver and the WDT/DMT/DEBUG/IESO/SOSC/MCLR keyword pragmas compute
 * are refused -- those are expressed through `pragma target clock`/`OSC`
 * and the keywords.  Returns FALSE when a conflict was logged. */
static boolean_t mips_fuse_user_overrides_apply(pfile_t *pf,
  unsigned long dcfg[4])
{
  value_t          fuses;
  value_t          edits;
  boolean_t        ok = BOOLEAN_TRUE;
  variable_ct_t    ii;

  fuses = pfile_value_find(pf, PFILE_LOG_NONE, "_fuses");
  edits = pfile_value_find(pf, PFILE_LOG_NONE, "_fuses_edit");
  if (!edits) {
    /* hand-written device file without the edit channel: the solved
     * words stand as-is (boot-fuses images still emit _FUSES raw) */
    if (fuses) {
      value_release(fuses);
    }
    return BOOLEAN_TRUE;
  }
  if (!fuses || !value_is_array(fuses) || !value_is_array(edits)
      || (value_ct_get(fuses) != value_ct_get(edits))
      || (value_ct_get(edits) > 4)) {
    pfile_log(pf, PFILE_LOG_WARN,
        "_fuses/_fuses_edit shape mismatch; user fuse pragmas ignored");
    if (fuses) {
      value_release(fuses);
    }
    value_release(edits);
    return BOOLEAN_TRUE;
  }
  for (ii = 0; ii < value_ct_get(edits); ii++) {
    value_t       ve;
    value_t       vf;
    unsigned long edit;
    unsigned long val;
    unsigned long owned;

    ve = value_subscript_set(edits, ii);
    vf = value_subscript_set(fuses, ii);
    if (!ve || !vf) {
      continue;
    }
    edit = (unsigned long) value_const_get(ve);
    val  = (unsigned long) value_const_get(vf);
    value_release(ve);
    value_release(vf);
    if (!edit) {
      continue;
    }
    owned = mips_clock_owned_mask(pf, (int) ii);
    if (edit & owned) {
      pfile_log(pf, PFILE_LOG_ERR,
          "DEVCFG%d: fuse pragma bits 0x%08lX overlap the clock-solved"
          " fields; set those via `pragma target clock`/`OSC` or the"
          " WDT/DMT/DEBUG/IESO/SOSC/MCLR keywords",
          (int) ii, edit & owned);
      ok = BOOLEAN_FALSE;
    } else {
      dcfg[ii] = (dcfg[ii] & ~edit) | (val & edit);
    }
  }
  value_release(fuses);
  value_release(edits);
  return ok;
}

/* Emit the DEVCFG0-3 config dwords into the HEX at their boot-flash
 * addresses.  The device file declares `_FUSES[4]` (values) and
 * `_FUSE_BASE[4]` (addresses, e.g. 0xBFC0FFC0..CC for MZ EF).  This
 * mirrors libpic12's pic_config_dump but writes 32-bit little-endian
 * words, since PIC32 config registers are full dwords in boot flash.
 * Solved clock words (pragma target OSC) emit for EVERY image shape --
 * for the loader flow they replace the splice tool's DEVCFG defaults. */
static void mips_fuses_emit(pfile_t *pf)
{
  value_t fuses;
  value_t base;

  /* Device-file _fuses defaults FIRST (boot-fuses images only): the
   * solved clock words below must win -- emitting the defaults AFTER
   * the solve (the old order) silently erased DEVCFG0-3 for every
   * boot-flash image carrying `pragma target OSC`, so a full-chip
   * erase left the part on erased fuses and the image never ran
   * (bench 2026-10-17). */
  if (pfile_flag_test(pf, PFILE_FLAG_BOOT_FUSES)
      && !mips_loader_pfm(pf)) {
  fuses = pfile_value_find(pf, PFILE_LOG_NONE, "_fuses");
  if (!fuses) {
    return;
  }
  base = pfile_value_find(pf, PFILE_LOG_ERR, "_fuse_base");
  if (value_is_array(fuses)) {
    variable_ct_t ii;
    variable_ct_t ct;

    ct = value_ct_get(fuses);
    if (base && (value_ct_get(base) != ct)) {
      pfile_log(pf, PFILE_LOG_ERR,
          "_fuses and _fuses_base must contain the same number of entries");
    }
    for (ii = 0; ii < ct; ii++) {
      value_t          tf;
      value_t          tb;
      unsigned long    val;
      unsigned long    addr;

      tf = value_subscript_set(fuses, ii);
      val = (unsigned long) value_const_get(tf);
      value_release(tf);
      if (base) {
        tb   = value_subscript_set(base, ii);
        addr = (unsigned long) value_const_get(tb);
        value_release(tb);
      } else {
        addr = MIPS_FLASH_BASE + 0xFFC0UL
             + (unsigned long) ((ct - 1 - ii) * 4);
      }
      pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 0), (unsigned char) (val        & 0xFF));
      pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 1), (unsigned char) ((val >>  8) & 0xFF));
      pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 2), (unsigned char) ((val >> 16) & 0xFF));
      pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 3), (unsigned char) ((val >> 24) & 0xFF));
    }
    value_release(base);
  } else {
    unsigned long val;

    val  = (unsigned long) value_const_get(fuses);
    pfile_write_hex(pf, (ulong) mips_hex_addr(pf, MIPS_FLASH_BASE + 0xFFC0UL),
        (unsigned char) (val        & 0xFF));
    pfile_write_hex(pf, (ulong) mips_hex_addr(pf, MIPS_FLASH_BASE + 0xFFC1UL),
        (unsigned char) ((val >>  8) & 0xFF));
    pfile_write_hex(pf, (ulong) mips_hex_addr(pf, MIPS_FLASH_BASE + 0xFFC2UL),
        (unsigned char) ((val >> 16) & 0xFF));
    pfile_write_hex(pf, (ulong) mips_hex_addr(pf, MIPS_FLASH_BASE + 0xFFC3UL),
        (unsigned char) ((val >> 24) & 0xFF));
  }
  value_release(fuses);  }

  {
    unsigned long dcfg[4];

    if (!mips_clock_fuses_get(dcfg)) {
      /* deferred solve that never completed: OSC pragma without a
       * clock pragma (order-lenient solver -- see mips_clock.c) */
      value_t osc = pfile_value_find(pf, PFILE_LOG_NONE, "target_osc");

      if (osc) {
        pfile_log(pf, PFILE_LOG_ERR,
          "pragma target OSC requires `pragma target clock`");
        value_release(osc);
      } else if (mips_fuse_user_changed(pf)) {
        /* fuse pragmas mutated _FUSES, but without a clock solve (or
         * the boot-fuses default path) those bits never reach the HEX */
        pfile_log(pf, PFILE_LOG_WARN,
            "fuse pragma bits are not emitted: add `pragma target clock`"
            " (solved DEVCFG) or use a boot-fuses image");
      }
    } else {
      int ii;

      mips_fuse_user_overrides_apply(pf, dcfg);
      /* dcfg[] is DEVCFG0..3; the device file's _FUSE_BASE[ii] is
       * DEVCFG0..3 in the same order (MZ EF: 0xBFC0FFCC..C0, MX1/2:
       * 0xBFC00BFC..F0). */
      for (ii = 0; ii < 4; ii++) {
        unsigned long addr = mips_fuse_addr(pf, ii);
        int k;

        for (k = 0; k < 4; k++) {
          pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + (ulong) k),
              (unsigned char) ((dcfg[ii] >> (8 * k)) & 0xFF));
        }
      }
    }
  }


}

void mips_hex_write(pfile_t *pf)
{
  mips_code_t c;

  for (c = mips_code_list_head(pf); c; c = c->link) {
    unsigned long word;
    unsigned long addr;

    if ((MIPS_OP_LABEL == c->op) || (MIPS_OP_ORG == c->op)) {
      continue;                   /* labels / ORG take no space */
    }
    word = mips_inst_encode(c);
    addr = c->addr;
    pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 0), (unsigned char) (word        & 0xFF));
    pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 1), (unsigned char) ((word >>  8) & 0xFF));
    if (mips_inst_size(c) > 2) {
      pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 2), (unsigned char) ((word >> 16) & 0xFF));
      pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + 3), (unsigned char) ((word >> 24) & 0xFF));
    }
  }
  mips_const_pool_hex_write(pf);
  mips_fuses_emit(pf);
  {
    unsigned long end   = mips_image_end(pf);
    unsigned long ebase = mips_ebase_page(pf);
    unsigned long base  = mips_code_base(pf);
    unsigned long last  = 0;
    boolean_t     loader = mips_loader_pfm(pf);
    boolean_t     mx     = (MIPS_FAMILY_MX1 == mips_family(pf));

    /* The MX loader flow keeps its vector/restart page in the top 4 KB
     * of program flash: the app must stay below it. */
    if (loader && mx && (ebase > base) && ((!end) || (ebase < end))) {
      end = ebase;
    }
    if (end) {
      for (c = mips_code_list_head(pf); c; c = c->link) {
        unsigned long a = c->addr;
        unsigned long e;

        if (!mips_inst_size(c)) {
          continue;
        }
        if (loader) {
          /* skip the boot-flash init block and the vector page */
          if ((a < base) || (a >= base + 0x100000UL)) {
            continue;
          }
          if ((a >= ebase) && (a < ebase + 0x2000UL)) {
            continue;
          }
        } else if ((a < MIPS_FLASH_BASE)
                || (a >= MIPS_FLASH_BASE + 0x10000UL)) {
          continue;
        }
        e = a + (unsigned long) mips_inst_size(c);
        if (e > last) {
          last = e;
        }
      }
      if (last > end) {
        pfile_log(pf, PFILE_LOG_ERR,
            "PIC32 image ends at 0x%08lx, past the %s limit 0x%08lx",
            last, loader ? "program-flash" : "boot-flash", end);
      }
    }
  }
}

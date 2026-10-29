/************************************************************
 **
 ** mips_const.c : PIC32 flash const pool (string literals, const arrays)
 **
 ** Const arrays (`_cstrN`, `const byte tbl[N] = {...}`) have no RAM.
 ** They are placed in boot flash immediately after the last instruction
 ** (qemu's EC boot flash is 64 KB, so the pool must follow the code,
 ** not sit at a fixed high address).  Addresses are not known at emit
 ** time: LUI+ORI pairs are recorded and patched after layout.
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/variable.h"
#include "mips.h"

struct mips_const_item {
  variable_t    var;
  unsigned      sz;
  unsigned long ofs;
};

struct mips_const_fixup {
  mips_code_t   lui;
  mips_code_t   ori;
  variable_t    var;
  unsigned long extra;
};

static struct mips_const_item  *mips_cpool;
static size_t                   mips_cpool_ct;
static size_t                   mips_cpool_cap;
static unsigned long            mips_cpool_bytes;
static unsigned long            mips_cpool_base;

static struct mips_const_fixup *mips_cfix;
static size_t                   mips_cfix_ct;
static size_t                   mips_cfix_cap;

static unsigned long mips_const_ofs_of(variable_t v)
{
  size_t ii;

  for (ii = 0; ii < mips_cpool_ct; ii++) {
    if (mips_cpool[ii].var == v) {
      return mips_cpool[ii].ofs;
    }
  }
  return 0;
}

boolean_t mips_var_is_flash_const(variable_t v)
{
  if (!v || !variable_is_const(v)) {
    return BOOLEAN_FALSE;
  }
  return variable_data_get(v) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* A pool entry is live only when codegen materialized its address
 * (an LUI/ORI fixup exists).  Reads that fold into immediates never
 * need the stored bytes. */
static boolean_t mips_const_referenced(variable_t v)
{
  size_t ii;

  for (ii = 0; ii < mips_cfix_ct; ii++) {
    if (mips_cfix[ii].var == v) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

void mips_const_pool_add(variable_t v)
{
  unsigned sz;
  size_t   ii;

  if (!mips_var_is_flash_const(v)) {
    return;
  }
  sz = (unsigned) variable_sz_get(v);
  if (!sz) {
    return;
  }
  for (ii = 0; ii < mips_cpool_ct; ii++) {
    if (mips_cpool[ii].var == v) {
      return;
    }
  }
  if (mips_cpool_ct >= mips_cpool_cap) {
    size_t ncap;

    ncap = mips_cpool_cap ? mips_cpool_cap * 2 : 16;
    mips_cpool = REALLOC(mips_cpool, ncap * sizeof(*mips_cpool));
    mips_cpool_cap = ncap;
  }
  if (!mips_cpool) {
    return;
  }
  /* 4-byte start so later LHU/LW of word/dword tables stay aligned
   * after an odd-length string / byte array. */
  {
    unsigned long ofs;

    ofs = (mips_cpool_bytes + 3UL) & ~3UL;
    mips_cpool[mips_cpool_ct].var = v;
    mips_cpool[mips_cpool_ct].sz  = sz;
    mips_cpool[mips_cpool_ct].ofs = ofs;
    mips_cpool_ct++;
    mips_cpool_bytes = ofs + sz;
  }
}

void mips_emit_flash_addr(unsigned reg, variable_t v, unsigned long extra)
{
  mips_code_t lui;
  mips_code_t ori;

  lui = mips_code_alloc(MIPS_OP_LUI, reg, 0, 0);
  ori = mips_code_alloc(MIPS_OP_ORI, reg, reg, 0);
  if (mips_cfix_ct >= mips_cfix_cap) {
    size_t ncap;

    ncap = mips_cfix_cap ? mips_cfix_cap * 2 : 16;
    mips_cfix = REALLOC(mips_cfix, ncap * sizeof(*mips_cfix));
    mips_cfix_cap = ncap;
  }
  if (!mips_cfix) {
    return;
  }
  mips_cfix[mips_cfix_ct].lui   = lui;
  mips_cfix[mips_cfix_ct].ori   = ori;
  mips_cfix[mips_cfix_ct].var   = v;
  mips_cfix[mips_cfix_ct].extra = extra;
  mips_cfix_ct++;
}

void mips_emit_var_addr(unsigned reg, variable_t v, unsigned long extra)
{
  if (mips_var_is_flash_const(v)) {
    mips_emit_flash_addr(reg, v, extra);
  } else {
    mips_emit_ram_ptr(reg, mips_var_ram(v) + extra);
  }
}

void mips_const_pool_reset(void)
{
  if (mips_cpool) {
    FREE(mips_cpool);
  }
  if (mips_cfix) {
    FREE(mips_cfix);
  }
  mips_cpool       = 0;
  mips_cpool_ct    = 0;
  mips_cpool_cap   = 0;
  mips_cpool_bytes = 0;
  mips_cpool_base  = 0;
  mips_cfix        = 0;
  mips_cfix_ct     = 0;
  mips_cfix_cap    = 0;
}

void mips_const_pool_resolve(pfile_t *pf)
{
  mips_code_t   c;
  unsigned long pool;
  size_t        ii;

  pool = mips_code_base(pf);
  for (c = mips_code_list_head(pf); c; c = c->link) {
    if ((MIPS_OP_LABEL != c->op) && (MIPS_OP_ORG != c->op)
        && mips_inst_size(c)) {
      unsigned long next;
      unsigned long ebase = mips_ebase_page(pf);

      next = c->addr + (unsigned long) mips_inst_size(c);
      /* Loader images carry a boot-flash init/vector block whose
       * KSEG1 addresses (0xBFC0xxxx) sit numerically ABOVE the KSEG0
       * PFM app (0x9Dxxxxxx), plus an EBASE vector page (0x9FC01000
       * on MZ, the top 4 KB of program flash on MX): the global max
       * would place the const pool into boot flash / the vector page.
       * Pool only after the app's own region. */
      if (mips_loader_pfm(pf)
          && ((c->addr >= MIPS_FLASH_BASE)
              || ((ebase != mips_code_base(pf))
                  && (c->addr >= ebase)
                  && (c->addr < (ebase + 0x2000UL))))) {
        continue;
      }
      if (next > pool) {
        pool = next;
      }
    }
  }
  pool = (pool + 3UL) & ~3UL;
  mips_cpool_base = pool;

  /* The variable walk registers EVERY const variable, but a const only
   * needs pool storage when its address was materialized (fixup above);
   * device files declare hundreds of routing/fuse constants that only
   * ever fold into immediates.  Drop the unreferenced entries and
   * re-compact before any size is used (same policy as the ESP32
   * esp_const_pool_place gate and the classic ROM-table lowering). */
  {
    size_t        rd;
    unsigned long ofs;

    rd  = 0;
    ofs = 0;
    for (ii = 0; ii < mips_cpool_ct; ii++) {
      if (!mips_const_referenced(mips_cpool[ii].var)) {
        continue;
      }
      ofs = (ofs + 3UL) & ~3UL;
      mips_cpool[rd].var = mips_cpool[ii].var;
      mips_cpool[rd].sz  = mips_cpool[ii].sz;
      mips_cpool[rd].ofs = ofs;
      rd++;
      ofs += mips_cpool[ii].sz;
    }
    mips_cpool_ct    = rd;
    mips_cpool_bytes = ofs;
  }

  /* The pool itself must also stay inside the image: MX loader apps are
   * capped below the EBASE vector page (top 4 KB of program flash). */
  {
    unsigned long end = mips_image_end(pf);

    if (mips_loader_pfm(pf) && (MIPS_FAMILY_MX1 == mips_family(pf))
        && !mips_mx_mvec_active(pf)) {
      unsigned long eb = mips_ebase_page(pf);

      if (eb && ((!end) || (eb < end))) {
        end = eb;
      }
    } else if (!mips_loader_pfm(pf)) {
      /* boot-flash MZ images have no bound; MX boot images are rejected
       * in mips_cmd_generate before reaching here. */
      end = 0;
    }
    if (end && ((pool + mips_cpool_bytes) > end)) {
      pfile_log(pf, PFILE_LOG_ERR,
        "PIC32 flash const pool ends at 0x%08lx, past the image limit "
        "0x%08lx", pool + mips_cpool_bytes, end);
    }
  }

  for (ii = 0; ii < mips_cfix_ct; ii++) {
    unsigned long addr;

    addr = mips_cpool_base + mips_const_ofs_of(mips_cfix[ii].var)
         + mips_cfix[ii].extra;
    if (mips_cfix[ii].lui) {
      mips_cfix[ii].lui->imm = (int) ((addr >> 16) & 0xFFFF);
    }
    if (mips_cfix[ii].ori) {
      mips_cfix[ii].ori->imm = (int) (addr & 0xFFFF);
    }
  }
}

void mips_const_pool_hex_write(pfile_t *pf)
{
  size_t ii;

  for (ii = 0; ii < mips_cpool_ct; ii++) {
    const unsigned char *data;
    unsigned             jj;
    unsigned long        addr;

    data = (const unsigned char *) variable_data_get(mips_cpool[ii].var);
    if (!data) {
      continue;
    }
    addr = mips_cpool_base + mips_cpool[ii].ofs;
    for (jj = 0; jj < mips_cpool[ii].sz; jj++) {
      pfile_write_hex(pf, (ulong) mips_hex_addr(pf, addr + jj), data[jj]);
    }
  }
}

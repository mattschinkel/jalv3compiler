/************************************************************
 **
 ** mips_var.c : PIC32 (MIPS32) linear variable allocator
 **
 ** MIPS has a flat 32-bit address space (no banking). Variables that
 ** need storage and do not already have an explicit `at` address are
 ** placed consecutively in RAM starting at MIPS_RAM_BASE (0x80000000),
 ** each naturally aligned to its own size.
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/variable.h"
#include "../libcore/value.h"
#include "../libcore/vardef.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pf_log.h"
#include "mips.h"

/* Compute the real RAM storage size of a variable def for the MIPS flat
 * model.  The built-in "pointer" type def is baked with a nominal size of 2
 * (PIC's FSR width) in jal_blck.c's basetypes table, which is wrong for MIPS
 * (32-bit pointers).  variable_def_sz_get() returns that nominal size, so
 * using it directly would under-allocate pointer storage.  This mirrors
 * libpic12's pic_variable_def_storage_need_get() but uses the
 * architecture-aware pfile_pointer_width_get() (4 for PIC32) and omits the
 * PIC16-only pointer-slot padding (irrelevant to MIPS's flat address space).
 * Recurses through arrays/structures/unions so pointer members are sized
 * correctly everywhere. */
variable_sz_t mips_def_storage_sz(pfile_t *pf, variable_def_t def)
{
  return pfile_flat_def_storage_sz(pf, def);
}

/* High-water RAM offset (from MIPS_RAM_BASE).  Kept after the alloc
 * pass so emit-time buffers (`_mips_bitarg_N`) can be placed without
 * overlapping the linear layout. */
static unsigned long mips_ram_next;

/* ---------- shared indirect-call protocol (libcore indirect_alg) -----
 * The inbox sizing, caller marshalling and callee PROC_ENTER/LEAVE
 * copies live once in indirect_alg.c; this table supplies the MIPS
 * byte movers.  The params of INDIRECT procs keep their LINEAR homes
 * (no re-basing): params living in the inbox would be clobbered by
 * any nested indirect call the proc makes (the ESP32 back end hit
 * exactly that defect). */

/* A4 teardown: the next compile allocates RAM from the bottom again. */
void mips_var_state_reset(void)
{
  mips_ram_next = 0;
}

static unsigned mips_ialg_def_bytes(pfile_t *pf, variable_def_t def)
{
  return (unsigned) mips_def_storage_sz(pf, def);
}

static void mips_ialg_scalar_copy(pfile_t *pf, value_t dst, value_t src)
{
  mips_load_value(pf, src, MIPS_REG_T1);
  mips_store_value(pf, dst, MIPS_REG_T1);
}

const ialg_ops_t mips_ialg_ops = {
  mips_ialg_def_bytes,
  mips_is_wide,
  mips_wide_assign,
  mips_ialg_scalar_copy
};

static void mips_var_overlay_indirect(pfile_t *pf)
{
  unsigned need = ialg_max_signature(pf, &mips_ialg_ops);

  ialg_inbox_set(0, 0);
  if (!need) {
    return;
  }
  mips_ram_next = (mips_ram_next + 3UL) & ~3UL;
  mips_ram_next += need;
  ialg_inbox_set(mips_ram_next - need, need);
}

void mips_var_place(pfile_t *pf, variable_t v, variable_sz_t sz)
{
  unsigned long align;

  if (!v || !sz) {
    return;
  }
  /* Power-of-two only: (sz & ~(sz-1)) is not alignment when sz is 3. */
  if (sz >= 4) {
    align = 4;
  } else if (sz >= 2) {
    align = 2;
  } else {
    align = 1;
  }
  mips_ram_next = (mips_ram_next + align - 1) & ~(align - 1);
  variable_base_set(v, (variable_base_t) mips_ram_next, 0);
  mips_ram_next += sz;
  if (pf && (mips_ram_next > mips_data_max(pf))) {
    pfile_log(pf, PFILE_LOG_ERR,
        "PIC32 data (%lu bytes) exceeds SRAM reserve (max %lu)",
        mips_ram_next, mips_data_max(pf));
  }
}

/* Give `v` a RAM offset if it still needs one.  POINTER formals for
 * `bit in bits[]` are created with FLAG_BIT copied from the pointee, so
 * pfile_block_variable_fixup packs them into _bitbucket as a few bits;
 * a MIPS pointer is a 4-byte aligned slot, so unhook the master and
 * place it.  `_name_count` on an open array is a proc-def member that
 * may have a variable with no block entry — still needs a base. */
void mips_var_ensure(pfile_t *pf, variable_t v)
{
  variable_def_t def;
  variable_t     master;
  variable_sz_t  sz;

  if (!v) {
    return;
  }
  def = variable_def_get(v);
  if (def && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))) {
    if (variable_master_get(v)) {
      /* Was packed into _bitbucket because FLAG_BIT leaked onto the
       * pointer def from the pointee.  Own base is not a real slot. */
      variable_master_set(v, VARIABLE_NONE);
      variable_bit_offset_set(v, 0);
      mips_var_place(pf, v, pfile_pointer_width_get(pf));
      return;
    }
    if (VARIABLE_BASE_UNKNOWN == variable_base_get(v, 0)) {
      mips_var_place(pf, v, pfile_pointer_width_get(pf));
    }
    return;
  }
  master = variable_master_get(v);
  if (master) {
    /* `_btmp` / `_bitbucket` aliases: place the packed master, not the bit. */
    mips_var_ensure(pf, master);
    return;
  }
  if (VARIABLE_BASE_UNKNOWN != variable_base_get(v, 0)) {
    return;
  }
  if (variable_is_pure_const(v)) {
    mips_const_pool_add(v);
    return;
  }
  sz = mips_def_storage_sz(pf, def);
  if (!sz && def && variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)) {
    sz = 1;
  }
  if (sz) {
    mips_var_place(pf, v, sz);
  }
}

void mips_value_ensure(pfile_t *pf, value_t val)
{
  variable_t     v;
  variable_def_t def;
  variable_sz_t  sz;

  if (!val || value_is_const(val) || value_is_label(val)
      || value_is_value(val)) {
    return;
  }
  v = value_variable_get(val);
  if (v) {
    mips_var_ensure(pf, v);
    return;
  }
  def = value_def_get(val);
  if (!def) {
    return;
  }
  sz = mips_def_storage_sz(pf, def);
  if (!sz) {
    sz = (variable_sz_t) value_byte_sz_get(val);
  }
  if (!sz) {
    return;
  }
  v = variable_alloc(TAG_NONE, def);
  if (!v) {
    return;
  }
  mips_var_place(pf, v, sz);
  value_variable_set(val, v);
  variable_release(v);
}

/* Assign storage to a proc parameter value that has a def but no variable
 * (the function return value, which has no tag and so was not allocated a
 * block variable). Creates a variable, gives it the next RAM offset, and
 * attaches it to the value so the body's writes/reads resolve.  Also
 * places an existing param variable that still has an unknown base
 * (`_name_count` on an open array). */
static void mips_var_alloc_param_value(pfile_t *pf, pfile_proc_t *proc)
{
  size_t ii;
  size_t ct = pfile_proc_param_ct_get(proc);

  for (ii = 0; ii < ct; ii++) {
    value_t        pval = pfile_proc_param_get(proc, ii);
    variable_t     v;
    variable_def_t def;
    variable_sz_t  sz;

    if (!pval) continue;
    v = value_variable_get(pval);
    if (v) {
      mips_var_ensure(pf, v);
      continue;
    }
    def = value_def_get(pval);
    if (!def) continue;
    sz = mips_def_storage_sz(pf, def);
    if (!sz) continue;
    {
      variable_t nv = variable_alloc(TAG_NONE, def);
      if (nv) {
        mips_var_place(pf, nv, sz);
        value_variable_set(pval, nv);
        variable_release(nv);
      }
    }
  }
}

void mips_var_alloc(pfile_t *pf)
{
  pfile_proc_t  *proc;

  /* variable_base_t is unsigned (32-bit KSEG `at` + GPR offsets from
   * MIPS_RAM_BASE). Offsets past the device's data ceiling
   * (mips_data_max: RAM top minus the stack reserve) would collide
   * with the stack. */
  mips_ram_next = 0;
  ialg_inbox_set(0, 0);
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc);
         blk;
         blk = pfile_block_next(blk)) {
      variable_t v;

      for (v = pfile_block_variable_list_head(blk);
           v;
           v = variable_link_get(v)) {
        mips_var_ensure(pf, v);
      }
    }
    /* allocate storage for the function return value (no block variable)
     * and any other param that still has no RAM offset */
    mips_var_alloc_param_value(pf, proc);
  }
  mips_task_alloc(pf);
  mips_var_overlay_indirect(pf);
  if (mips_ram_next > mips_data_max(pf)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "PIC32 data (%lu bytes) exceeds SRAM reserve (max %lu)",
        mips_ram_next, mips_data_max(pf));
  }
}

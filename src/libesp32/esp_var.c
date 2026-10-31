/************************************************************
 **
 ** esp_var.c : ESP32 linear variable allocator
 **
 ** Xtensa under the call0 ABI has a flat 32-bit address space.
 ** Variables that need storage and do not already have an
 ** explicit `at` address are placed consecutively in DRAM from
 ** ESP32_RAM_BASE (0x3FFB1000), naturally aligned to their size.
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
#include "../libutils/mem.h"
#include "../libcore/pf_log.h"
#include "esp.h"

void esp_const_pool_add(variable_t v);

/* Compute the real DRAM storage size of a variable def for the ESP32
 * flat model (mirrors mips_def_storage_sz; pointers are 4 bytes). */
variable_sz_t esp_def_storage_sz(pfile_t *pf, variable_def_t def)
{
  return pfile_flat_def_storage_sz(pf, def);
}

static unsigned long esp_ram_next;

void esp_var_place(pfile_t *pf, variable_t v, variable_sz_t sz)
{
  unsigned long align;

  if (!v || !sz) {
    return;
  }
  if (sz >= 4) {
    align = 4;
  } else if (sz >= 2) {
    align = 2;
  } else {
    align = 1;
  }
  esp_ram_next = (esp_ram_next + align - 1UL) & ~(align - 1UL);
  variable_base_set(v, (variable_base_t) esp_ram_next, 0);
  esp_ram_next += sz;
  if (pf && (esp_ram_next > ESP32_SRAM_DATA_MAX)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32 data (%lu bytes) exceeds SRAM reserve (max %lu)",
        esp_ram_next, (unsigned long) ESP32_SRAM_DATA_MAX);
  }
}

/* Give `v` a DRAM offset if it still needs one.  Mirrors skip:
 * const vars (flash pool), function types and packed
 * bit aliases (master) are not linearly placed.  POINTER formals for
 * `bit in bits[]` are hooked into _bitbucket by
 * pfile_block_variable_fixup AFTER the alloc pass (FLAG_BIT leaks from
 * the pointee), so the unhook must also happen from the emit-time
 * ensure -- hence this is public (mips_var_ensure rule). */
void esp_var_ensure(pfile_t *pf, variable_t v)
{
  variable_def_t def;
  variable_t     master;
  variable_sz_t  sz;

  if (!v) {
    return;
  }
  if (VARIABLE_DEF_TYPE_FUNCTION == variable_type_get(v)) {
    return;
  }
  def = variable_def_get(v);
  if (def && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))) {
    if (variable_master_get(v)) {
      /* Packed into _bitbucket because FLAG_BIT leaked onto the
       * pointer def from the pointee.  That base is not a real slot:
       * unhook the master and place a 4-byte aligned one. */
      variable_master_set(v, VARIABLE_NONE);
      variable_bit_offset_set(v, 0);
      esp_var_place(pf, v, pfile_pointer_width_get(pf));
      return;
    }
    if (VARIABLE_BASE_UNKNOWN == variable_base_get(v, 0)) {
      esp_var_place(pf, v, pfile_pointer_width_get(pf));
    }
    return;
  }
  if (variable_is_pure_const(v)) {
    /* 3e: const data gets IRAM data nodes (see variable_is_pure_const
     * for why the assignment count matters) */
    esp_const_pool_add(v);
    return;
  }
  master = variable_master_get(v);
  if (master) {
    /* `_btmp` / `_bitbucket` aliases: place the packed master. */
    esp_var_ensure(pf, master);
    return;
  }
  if (VARIABLE_BASE_UNKNOWN != variable_base_get(v, 0)) {
    return;
  }
  sz = esp_def_storage_sz(pf, def);
  if (!sz && def && variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)) {
    sz = 1;
  }
  if (sz) {
    esp_var_place(pf, v, sz);
  }
}

/* Emit-time placement guard for a value operand (mips_value_ensure
 * analogue).  Consts stay out: esp_const_pool_add does not dedup, and
 * const data was registered during the alloc pass.  Const-marked
 * variables with live assignments still need a home (see
 * esp_var_ensure). */
void esp_value_ensure(pfile_t *pf, value_t val)
{
  variable_t v;

  if (!val || value_is_const(val) || value_is_label(val)
      || value_is_value(val)) {
    return;
  }
  v = value_variable_get(val);
  if (!v || variable_is_pure_const(v)) {
    return;
  }
  esp_var_ensure(pf, v);
}

/* Assign storage to a proc parameter value that has a def but no
 * variable (the function return value has no tag and so no block
 * variable; `_name_count` on an open array) -- mirrors
 * mips_var_alloc_param_value. */
/* A4 teardown: the next compile allocates RAM from the bottom again. */
void esp_var_state_reset(void)
{
  esp_ram_next = 0;
}

static void esp_var_alloc_param_value(pfile_t *pf, pfile_proc_t *proc)
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
      esp_var_ensure(pf, v);
      continue;
    }
    def = value_def_get(pval);
    if (!def) continue;
    sz = esp_def_storage_sz(pf, def);
    if (!sz) continue;
    {
      variable_t nv = variable_alloc(TAG_NONE, def);
      if (nv) {
        esp_var_place(pf, nv, sz);
        value_variable_set(pval, nv);
        variable_release(nv);
      }
    }
  }
}

/* ------------- indirect-call shared argument area -------------
 * Volatile IN/OUT formals are call-by-thunk: the caller stores the
 * hidden getter/setter's address into the formal slot and each callee
 * use is an indirect call through it.  The shared area is a
 * MARSHALLING INBOX only: indirect callers (which cannot see the
 * callee's param vars) marshal by member-def order, and every
 * INDIRECT-flagged callee copies inbox -> its own linear param homes
 * at PROC_ENTER (OUT params copied back at PROC_LEAVE -- esp_op.c).
 * The params must NOT live in the inbox: a proc whose params sat
 * there would be clobbered by any nested indirect call it makes (a
 * pointer-called proc printing through a device thunk marshalled
 * every character through the same bytes -- demo_sample/esp32_pointers
 * printed "Proc A: 32" instead of 42).  The whole protocol lives in
 * libcore indirect_alg.c; this places the inbox. */

static void esp_var_overlay_indirect(pfile_t *pf)
{
  unsigned need = ialg_max_signature(pf, &esp_ialg_ops);

  ialg_inbox_set(0, 0);
  if (!need) {
    return;
  }
  esp_ram_next = (esp_ram_next + 3UL) & ~3UL;
  esp_ram_next += need;
  ialg_inbox_set(esp_ram_next - need, need);
}

void esp_var_alloc(pfile_t *pf)
{
  pfile_proc_t *proc;

  esp_ram_next = 0;
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
        esp_var_ensure(pf, v);
      }
    }
    esp_var_alloc_param_value(pf, proc);
  }
  esp_var_overlay_indirect(pf);
  esp_task_alloc(pf);
}

/* End of the linear data section (absolute), 4-rounded: the startup
 * zeroes [ESP32_RAM_BASE, this) because QEMU's ESP32 ROM boot leaves
 * staging garbage in DRAM and uninitialized JAL globals (string
 * handles, string temps) must read as zero. */
unsigned long esp_data_end(void)
{
  return ESP32_RAM_BASE + ((esp_ram_next + 3UL) & ~3UL);
}

/************************************************************
 **
 ** indirect_alg.c : shared indirect-call argument protocol
 **
 ** See indirect_alg.h.  The walks below were lifted from the ESP32
 ** implementation (esp_var.c / esp_op.c); that back end's wiring is a
 ** pure code motion, and PIC32 gets the same protocol through its own
 ** ops table (which also lands the MIPS twin of the inbox fix).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "pfile.h"
#include "pf_proc.h"
#include "variable.h"
#include "value.h"
#include "vardef.h"
#include "indirect_alg.h"

static unsigned long ialg_inbox_base;
static unsigned      ialg_inbox_lim;

/* the one align rule: every offset in the protocol derives from it */
static unsigned ialg_align(unsigned ofs, unsigned sz)
{
  unsigned align;

  if (sz >= 4) {
    align = 4;
  } else if (sz >= 2) {
    align = 2;
  } else {
    align = 1;
  }
  return (ofs + align - 1U) & ~(align - 1U);
}

void ialg_inbox_set(unsigned long base, unsigned limit)
{
  ialg_inbox_base = base;
  ialg_inbox_lim  = limit;
}

unsigned ialg_inbox_limit(void)
{
  return ialg_inbox_lim;
}

variable_def_t ialg_call_fn_def(value_t proc_val)
{
  variable_def_t        def;
  variable_def_member_t mbr;

  def = proc_val ? value_def_get(proc_val) : VARIABLE_DEF_NONE;
  if (def && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))) {
    mbr = variable_def_member_get(def);
    def = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  }
  if (!def || (VARIABLE_DEF_TYPE_FUNCTION != variable_def_type_get(def))) {
    return VARIABLE_DEF_NONE;
  }
  return def;
}

value_t ialg_slot_value(pfile_t *pf, const ialg_ops_t *ops,
    variable_def_t def, unsigned ofs)
{
  variable_t v;
  value_t    val;

  UNUSED(pf);
  if (!def || (ofs + ops->def_bytes(pf, def) > ialg_inbox_lim)) {
    return VALUE_NONE;
  }
  v = variable_alloc(TAG_NONE, def);
  if (!v) {
    return VALUE_NONE;
  }
  variable_base_set(v, (variable_base_t) (ialg_inbox_base + ofs), 0);
  val = value_alloc(v);
  variable_release(v);
  return val;
}

static void ialg_copy(pfile_t *pf, const ialg_ops_t *ops, value_t dst,
    value_t src)
{
  if (ops->is_wide(dst, src, VALUE_NONE)) {
    ops->wide_assign(pf, dst, src);
  } else {
    ops->scalar_copy(pf, dst, src);
  }
}

/* byte size of a function def's parameter list (deref the pointer
 * wrapper the front end puts around fn defs) */
static unsigned ialg_fn_def_param_bytes(pfile_t *pf, const ialg_ops_t *ops,
    variable_def_t def)
{
  variable_def_member_t mbr;
  unsigned              ofs;
  unsigned              sz;

  if (!def) {
    return 0;
  }
  if (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def)) {
    mbr = variable_def_member_get(def);
    def = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  }
  if (!def || (VARIABLE_DEF_TYPE_FUNCTION != variable_def_type_get(def))) {
    return 0;
  }
  ofs = 0;
  for (mbr = variable_def_member_get(def);
      mbr;
      mbr = variable_def_member_link_get(mbr)) {
    sz = ops->def_bytes(pf, variable_def_member_def_get(mbr));
    if (!sz) {
      continue;
    }
    ofs = ialg_align(ofs, sz) + sz;
  }
  return ofs;
}

/* byte size of a proc's parameter list */
static unsigned ialg_proc_param_bytes(pfile_t *pf, const ialg_ops_t *ops,
    pfile_proc_t *proc)
{
  size_t   ii;
  size_t   ct;
  unsigned ofs;
  unsigned sz;
  value_t  pval;

  ct = pfile_proc_param_ct_get(proc);
  ofs = 0;
  for (ii = 0; ii < ct; ii++) {
    pval = pfile_proc_param_get(proc, ii);
    if (!pval) {
      continue;
    }
    sz = ops->def_bytes(pf, value_def_get(pval));
    if (!sz) {
      sz = (unsigned) value_byte_sz_get(pval);
    }
    if (!sz) {
      continue;
    }
    ofs = ialg_align(ofs, sz) + sz;
  }
  return ofs;
}

unsigned ialg_max_signature(pfile_t *pf, const ialg_ops_t *ops)
{
  pfile_proc_t *proc;
  unsigned      need;
  unsigned      n;

  need = 0;
  for (proc = pfile_proc_root_get(pf);
      proc;
      proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)) {
      n = ialg_proc_param_bytes(pf, ops, proc);
      if (n > need) {
        need = n;
      }
    }
    {
      pfile_block_t *blk;

      for (blk = pfile_proc_block_root_get(proc);
          blk;
          blk = pfile_block_next(blk)) {
        variable_t v;

        for (v = pfile_block_variable_list_head(blk);
            v;
            v = variable_link_get(v)) {
          n = ialg_fn_def_param_bytes(pf, ops, variable_def_get(v));
          if (n > need) {
            need = n;
          }
        }
      }
    }
  }
  return need;
}

void ialg_marshal_in(pfile_t *pf, const ialg_ops_t *ops, value_t proc_val,
    value_t *params, size_t ct)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  size_t                ii;
  unsigned              ofs;

  def = ialg_call_fn_def(proc_val);
  if (!def) {
    return;
  }
  ofs = 0;
  ii = 0;
  for (mbr = variable_def_member_get(def);
      mbr && (ii < ct);
      mbr = variable_def_member_link_get(mbr), ii++) {
    variable_def_t mdef;
    unsigned       sz;
    value_t        slot;
    value_t        arg;

    mdef = variable_def_member_def_get(mbr);
    sz = ops->def_bytes(pf, mdef);
    if (!sz) {
      continue;
    }
    ofs = ialg_align(ofs, sz);
    arg = params ? params[ii] : VALUE_NONE;
    if (arg && variable_def_flag_test(mdef, VARIABLE_DEF_FLAG_IN)) {
      if (ofs + sz > ialg_inbox_lim) {
        pfile_log(pf, PFILE_LOG_ERR,
            "indirect call arguments exceed shared parameter area");
        return;
      }
      slot = ialg_slot_value(pf, ops, mdef, ofs);
      if (slot) {
        ialg_copy(pf, ops, slot, arg);
        value_release(slot);
      }
    }
    ofs += sz;
  }
}

void ialg_marshal_out(pfile_t *pf, const ialg_ops_t *ops, value_t proc_val,
    value_t *params, size_t ct)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  size_t                ii;
  unsigned              ofs;

  def = ialg_call_fn_def(proc_val);
  if (!def) {
    return;
  }
  ofs = 0;
  ii = 0;
  for (mbr = variable_def_member_get(def);
      mbr && (ii < ct);
      mbr = variable_def_member_link_get(mbr), ii++) {
    variable_def_t mdef;
    unsigned       sz;
    value_t        slot;
    value_t        arg;

    mdef = variable_def_member_def_get(mbr);
    sz = ops->def_bytes(pf, mdef);
    if (!sz) {
      continue;
    }
    ofs = ialg_align(ofs, sz);
    arg = params ? params[ii] : VALUE_NONE;
    if (arg && variable_def_flag_test(mdef, VARIABLE_DEF_FLAG_OUT)
        && !value_is_const(arg) && !value_is_label(arg)
        && !value_is_function(arg)
        && !(value_variable_get(arg)
          && variable_is_const(value_variable_get(arg)))) {
      if (ofs + sz <= ialg_inbox_lim) {
        slot = ialg_slot_value(pf, ops, mdef, ofs);
        if (slot) {
          ialg_copy(pf, ops, arg, slot);
          value_release(slot);
        }
      }
    }
    ofs += sz;
  }
}

void ialg_param_in(pfile_t *pf, const ialg_ops_t *ops, pfile_proc_t *proc)
{
  size_t   ii;
  size_t   ct = pfile_proc_param_ct_get(proc);
  unsigned ofs = 0;

  for (ii = 0; ii < ct; ii++) {
    value_t         pval = pfile_proc_param_get(proc, ii);
    variable_def_t  pdef;
    variable_t      pv;
    unsigned        sz;
    value_t         slot;

    if (!pval) {
      continue;
    }
    pdef = value_def_get(pval);
    sz   = ops->def_bytes(pf, pdef);
    if (!sz) {
      sz = (unsigned) value_byte_sz_get(pval);
    }
    if (!sz) {
      continue;
    }
    ofs = ialg_align(ofs, sz);
    if (pdef && variable_def_flag_test(pdef, VARIABLE_DEF_FLAG_IN)) {
      pv = value_variable_get(pval);
      if (pv && (VARIABLE_BASE_UNKNOWN != variable_base_get(pv, 0))
          && (ofs + sz <= ialg_inbox_lim)) {
        slot = ialg_slot_value(pf, ops, pdef, ofs);
        if (slot) {
          ialg_copy(pf, ops, pval, slot);
          value_release(slot);
        }
      }
    }
    ofs += sz;
  }
}

void ialg_param_out(pfile_t *pf, const ialg_ops_t *ops, pfile_proc_t *proc)
{
  size_t   ii;
  size_t   ct = pfile_proc_param_ct_get(proc);
  unsigned ofs = 0;

  for (ii = 0; ii < ct; ii++) {
    value_t         pval = pfile_proc_param_get(proc, ii);
    variable_def_t  pdef;
    variable_t      pv;
    unsigned        sz;
    value_t         slot;

    if (!pval) {
      continue;
    }
    pdef = value_def_get(pval);
    sz   = ops->def_bytes(pf, pdef);
    if (!sz) {
      sz = (unsigned) value_byte_sz_get(pval);
    }
    if (!sz) {
      continue;
    }
    ofs = ialg_align(ofs, sz);
    if (pdef && variable_def_flag_test(pdef, VARIABLE_DEF_FLAG_OUT)) {
      pv = value_variable_get(pval);
      if (pv && (VARIABLE_BASE_UNKNOWN != variable_base_get(pv, 0))
          && (ofs + sz <= ialg_inbox_lim)) {
        slot = ialg_slot_value(pf, ops, pdef, ofs);
        if (slot) {
          ialg_copy(pf, ops, slot, pval);
          value_release(slot);
        }
      }
    }
    ofs += sz;
  }
}

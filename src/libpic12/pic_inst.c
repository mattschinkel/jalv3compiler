/************************************************************
 **
 ** pic_inst.c : PIC instruction generation definitions
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "piccolst.h"
#include "pic12.h"
#include "pic14.h"
#include "pic14h.h"
#include "pic16.h"
#include "pic_code.h"
#include "pic_inst.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/pfile.h"

static flag_t pic_instr_default_flags;

/* Sequential emit-time BSR bank (PIC18: MOVLB literal; hybrid: MOVLB literal).
 * PIC_EMIT_BSR_UNK: no known bank for the next data access on the linear emit
 * path (invalidated by control flow, labels, skip reorder in daop, etc.). */
#define PIC_EMIT_BSR_UNK (0x200U)
static unsigned pic_emit_movl_bsr = PIC_EMIT_BSR_UNK;

/* A4 teardown between compiles: forget the default flags and the linear
 * emit-path bank cache. pic_instr_emit_bsr_cache_reset() below is the
 * mid-compile invalidation and stays as it is. */
void pic_inst_state_reset(void)
{
  pic_instr_default_flags = 0;
  pic_emit_movl_bsr        = PIC_EMIT_BSR_UNK;
}

void pic_instr_emit_bsr_cache_reset(void)
{
  pic_emit_movl_bsr = PIC_EMIT_BSR_UNK;
}

static void pic_emit_bsr_note_appended(pfile_t *pf, pic_code_t code)
{
  pic_opcode_t  op;
  label_t       lbl;
  value_t       lit;

  if (!code) {
    return;
  }
  if (!pic_is_16bit(pf) && !pic_is_14bit_hybrid(pf)) {
    return;
  }
  lbl = pic_code_label_get(code);
  if (LABEL_NONE != lbl) {
    pic_emit_movl_bsr = PIC_EMIT_BSR_UNK;
    return;
  }
  op = pic_code_op_get(code);
  switch (op) {
    case PIC_OPCODE_MOVLB:
      lit = pic_code_literal_get(code);
      if (lit && value_is_const(lit)) {
        pic_emit_movl_bsr = (unsigned) (value_const_get(lit) & 0xffU);
      } else if (!lit) {
        /* append_w_k attaches literal after pic_instr_alloc; do not force
         * PIC_EMIT_BSR_UNK between alloc and pic_code_literal_set + second
         * pic_emit_bsr_note_appended (would defeat skip-over-GOTO cache and
         * redundant literal MOVLB suppression). */
        break;
      } else {
        pic_emit_movl_bsr = PIC_EMIT_BSR_UNK;
      }
      break;
    case PIC_OPCODE_GOTO:
    case PIC_OPCODE_BRA:
      {
        pic_code_t pr;

        pr = pic_code_prev_get(code);
        if (pr) {
          pic_opcode_t pop;

          pop = pic_code_op_get(pr);
          if ((PIC_OPCODE_BTFSC == pop) || (PIC_OPCODE_BTFSS == pop)
              || (PIC_OPCODE_INCFSZ == pop) || (PIC_OPCODE_DECFSZ == pop)) {
            /* Skip may execute the GOTO/BRA slot without branching; the listing
             * successor is then reached with the same BSR as before this insn.
             * Clearing the cache here forces a redundant MOVLB on that edge
             * (e.g. strings_dynamic many-handle compact). */
            break;
          }
        }
      }
      pic_emit_movl_bsr = PIC_EMIT_BSR_UNK;
      break;
    case PIC_OPCODE_CALL:
    case PIC_OPCODE_RCALL:
    case PIC_OPCODE_RETURN:
    case PIC_OPCODE_RETLW:
    case PIC_OPCODE_RETFIE:
    case PIC_OPCODE_RESET:
    case PIC_OPCODE_BC:
    case PIC_OPCODE_BN:
    case PIC_OPCODE_BNC:
    case PIC_OPCODE_BNN:
    case PIC_OPCODE_BNOV:
    case PIC_OPCODE_BNZ:
    case PIC_OPCODE_BOV:
    case PIC_OPCODE_BZ:
      pic_emit_movl_bsr = PIC_EMIT_BSR_UNK;
      break;
    default:
      break;
  }
}

void pic_instr_default_flag_set(pfile_t *pf, flag_t flags)
{
  UNUSED(pf);

  pic_instr_default_flags |= flags;
}

void pic_instr_default_flag_clr(pfile_t *pf, flag_t flags)
{
  UNUSED(pf);

  pic_instr_default_flags &= ~flags;
}

boolean_t pic_instr_default_flag_test(pfile_t *pf, flag_t flag)
{
  UNUSED(pf);

  return (pic_instr_default_flags & flag) == flag;
}

flag_t pic_instr_default_flag_get_all(pfile_t *pf)
{
  UNUSED(pf);

  return pic_instr_default_flags;
}


pic_code_t pic_instr_alloc(pfile_t *pf, label_t lbl, pic_opcode_t op)
{
  pic_code_t code;

  if (pic_is_16bit(pf)) {
    if (PIC_OPCODE_RLF == op) {
      op = PIC_OPCODE_RLCF;
    } else if (PIC_OPCODE_RRF == op) {
      op = PIC_OPCODE_RRCF;
    }
  } 
  code = pic_code_alloc(lbl, op, pic_instr_default_flag_get_all(pf));
  pic_code_list_append(pf, code);
  pic_emit_bsr_note_appended(pf, code);
  return code;
}


pic_code_t pic_instr_append_label(pfile_t *pf, label_t lbl)
{
  return pic_instr_alloc(pf, lbl, PIC_OPCODE_NONE);
}

/* 
 * determine if var exists in the region defined by {lo,hi}
 */
static boolean_t pic_variable_region_test(variable_t var, size_t lo, 
  size_t hi)
{
  size_t    ii;
  boolean_t rc;

  while (variable_master_get(var)) {
    var = variable_master_get(var);
  }
  for (rc = BOOLEAN_FALSE, ii = 0; !rc && (ii < VARIABLE_MIRROR_CT); ii++) {
    variable_base_t base;

    base = variable_base_get(var, ii);
    rc = (lo <= base) && (base <= hi);
  }
  return rc;
}

/* 
 * returns
 *   PIC_OPCODE_NONE       : no code needed
 *   PIC_OPCODE_DATALO_CLR
 *   PIC_OPCODE_DATALO_SET
 */
pic_opcode_t pic_value_datalo_get(pfile_t *pf, value_t val)
{
  pic_opcode_t op;
  unsigned     bank_sz;

  bank_sz = pic_target_bank_size_get(pf);
  if (value_is_const(val)) {
    variable_const_t cn;

    cn = value_const_get(val);
    op = (cn & bank_sz) ? PIC_OPCODE_DATALO_SET : PIC_OPCODE_DATALO_CLR;
  } else {
    variable_t var;

    var = value_variable_get(val);

    /* if this exists in both region 0 & 2, nothing need be done with rp0 */
    if (!(pic_variable_region_test(var,        0x0000,      bank_sz - 1) 
          && pic_variable_region_test(var,     bank_sz, 2 * bank_sz - 1))
        && !(pic_variable_region_test(var, 2 * bank_sz, 3 * bank_sz - 1) 
          && pic_variable_region_test(var, 3 * bank_sz, 4 * bank_sz - 1))) {
      variable_base_t base;

      base = value_base_get(val);
      op = (base & bank_sz) 
           ? PIC_OPCODE_DATALO_SET
           : PIC_OPCODE_DATALO_CLR;
    } else {
      op = PIC_OPCODE_NONE;
    }
  }
  return op;
}

/*
 * returns
 *   PIC_OPCODE_NONE      : no code needed
 *   PIC_OPCODE_DATAHI_CLR
 *   PIC_OPCODE_DATAHI_SET
 */
pic_opcode_t pic_value_datahi_get(pfile_t *pf, value_t val)
{
  pic_opcode_t op;
  unsigned     bank_sz;

  bank_sz = pic_target_bank_size_get(pf);
  if (value_is_const(val)) {
    variable_const_t cn;

    cn = value_const_get(val);
    op = (cn & (2 * bank_sz)) ? PIC_OPCODE_DATAHI_SET : PIC_OPCODE_DATAHI_CLR;
  } else {
    variable_t var;

    var = value_variable_get(val);

    /* if this exists in both region 1 & 3, nothing need be done with rp1 */
    if (!(pic_variable_region_test(var,         0x0000,     bank_sz - 1) 
          && pic_variable_region_test(var, 2 * bank_sz, 3 * bank_sz - 1))
        && !(pic_variable_region_test(var,     bank_sz, 2 * bank_sz - 1) 
          && pic_variable_region_test(var, 3 * bank_sz, 4 * bank_sz - 1))) {
      variable_base_t base;

      base = value_base_get(val);
      op   = (base & (2 * bank_sz))
             ? PIC_OPCODE_DATAHI_SET
             : PIC_OPCODE_DATAHI_CLR;
    } else {
      op = PIC_OPCODE_NONE;
    }
  }
  return op;
}

/* Classic PIC14: every MOVWF must run daop (even under NO_OPTIMIZE) so file
 * bank select is present; pic_code_databits_post_analyze drops redundant DATALO.
 * When the list tail is MOVLW, the next file op also needs the same (movlw does
 * not set RP0/RP1; see print_crlf). */
static boolean_t
pic_instr_force_daop_pic14(pfile_t *pf, pic_opcode_t op)
{
  pic_code_t tl;

  if (!pic_is_14bit(pf) || pic_is_14bit_hybrid(pf)) {
    return BOOLEAN_FALSE;
  }
  if (PIC_OPCODE_MOVWF == op) {
    return BOOLEAN_TRUE;
  }
  tl = pic_code_list_tail_get(pf);
  return (boolean_t) (tl && (PIC_OPCODE_MOVLW == pic_code_op_get(tl)));
}

/* pic_emit_movl_bsr can still hold a bank from earlier code while BSR was
 * cleared (movlb 0); redundant suppression would skip MOVLB before incf/decf.
 * Invalidate so append_daop always considers emitting MOVLB for these ops. */
static void pic_instr_emit_bsr_cache_reset_before_hybrid_incdec(pfile_t *pf,
    pic_opcode_t op)
{
  if (!pic_is_14bit_hybrid(pf)) {
    return;
  }
  switch (op) {
    case PIC_OPCODE_INCF:
    case PIC_OPCODE_INCFSZ:
    case PIC_OPCODE_DECF:
    case PIC_OPCODE_DECFSZ:
      pic_instr_emit_bsr_cache_reset();
      break;
    default:
      break;
  }
}

boolean_t pic_are_variable_databits_different(pfile_t *pf, value_t a, 
    value_t b)
{
  boolean_t rc;

  pic_opcode_t a_lo;
  pic_opcode_t a_hi;
  pic_opcode_t b_lo;
  pic_opcode_t b_hi;

  a_lo = pic_value_datalo_get(pf, a);
  a_hi = pic_value_datahi_get(pf, a);
  b_lo = pic_value_datalo_get(pf, b);
  b_hi = pic_value_datahi_get(pf, b);

  rc = ((PIC_OPCODE_NONE != a_lo) 
      && (PIC_OPCODE_NONE != b_lo) 
      && (a_lo != b_lo))
    || ((PIC_OPCODE_NONE != a_hi)
      && (PIC_OPCODE_NONE != b_hi)
      && (a_hi != b_hi));
  return rc;
}

/*
 * Classic PIC14: pic_instr_append_f(MOVWF) always runs append_daop (force) so
 * bank bits are set before the file write. picdelay.c already emits a 2-cycle
 * RP preamble (datalo/datahi or NOP pad) immediately before MOVLW; MOVLW does
 * not change RP, so emitting the same datalo/datahi again is redundant ROM.
 * Detect the common case: two instructions before tail MOVLW already match
 * the bank ops we would emit for f (same mapping as append_daop below).
 */
static boolean_t
pic_classic_pic14_movwf_bank_already_selected(pfile_t *pf, value_t f,
    size_t file_ofs)
{
  pic_code_t        tl;
  pic_code_t        p;
  pic_code_t        q;
  variable_base_t   base;
  unsigned long     full_file;
  unsigned          bank_sz;
  pic_opcode_t      raw_lo;
  pic_opcode_t      raw_hi;
  pic_opcode_t      want_lo;
  pic_opcode_t      want_hi;

  if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
    return BOOLEAN_FALSE;
  }
  tl = pic_code_list_tail_get(pf);
  if (!tl || PIC_OPCODE_MOVLW != pic_code_op_get(tl)) {
    return BOOLEAN_FALSE;
  }
  p = pic_code_prev_get(tl);
  q = p ? pic_code_prev_get(p) : PIC_CODE_NONE;
  if (!p || !q) {
    return BOOLEAN_FALSE;
  }

  base = value_base_get(f);
  full_file = (unsigned long) base;
  if (VARIABLE_BASE_UNKNOWN != base) {
    value_t bofs;

    bofs = value_baseofs_get(f);
    if (bofs && value_is_const(bofs)) {
      full_file += (unsigned long) value_const_get(bofs);
    }
    full_file += (unsigned long) (value_bit_offset_get(f) / 8);
    full_file += (unsigned long) file_ofs;
  }

  bank_sz = pic_target_bank_size_get(pf);

  raw_lo = pic_value_datalo_get(pf, f);
  raw_hi = pic_value_datahi_get(pf, f);

  want_lo = raw_lo;
  if (PIC_OPCODE_NONE != raw_lo && VARIABLE_BASE_UNKNOWN != base) {
    want_lo = (full_file & (unsigned long) bank_sz)
        ? PIC_OPCODE_DATALO_SET
        : PIC_OPCODE_DATALO_CLR;
  }
  want_hi = raw_hi;
  if (PIC_OPCODE_NONE != raw_hi && VARIABLE_BASE_UNKNOWN != base) {
    want_hi = (full_file & (unsigned long) (2U * bank_sz))
        ? PIC_OPCODE_DATAHI_SET
        : PIC_OPCODE_DATAHI_CLR;
  }

  if (PIC_OPCODE_NONE == want_lo && PIC_OPCODE_NONE == want_hi) {
    return BOOLEAN_FALSE;
  }

  /* pic12_delay_emit_data_bank_preamble uses pic_value_datalo/datahi_get
   * (declaration base), not full_file remapping. */
  if (PIC_OPCODE_NONE != raw_lo && PIC_OPCODE_NONE != raw_hi) {
    if (pic_code_op_get(q) == raw_lo && pic_code_op_get(p) == raw_hi) {
      return BOOLEAN_TRUE;
    }
  }

  /* Older q, newer p, then MOVLW at tl — matches picdelay preamble order. */
  if (PIC_OPCODE_NONE != want_lo && PIC_OPCODE_NONE != want_hi) {
    return (boolean_t) (pic_code_op_get(q) == want_lo
        && pic_code_op_get(p) == want_hi);
  }
  if (PIC_OPCODE_NONE != want_lo && PIC_OPCODE_NONE == want_hi) {
    return (boolean_t) (pic_code_op_get(q) == want_lo
        && PIC_OPCODE_NOP == pic_code_op_get(p));
  }
  return (boolean_t) (PIC_OPCODE_NOP == pic_code_op_get(q)
      && pic_code_op_get(p) == want_hi);
}

void pic_instr_append_daop(pfile_t *pf, value_t f, pic_opcode_t opcode,
    boolean_t force, size_t file_ofs)
{
  if ((force || !pic_instr_default_flag_test(pf, PIC_CODE_FLAG_NO_OPTIMIZE))
      && (PIC_OPCODE_MOVLW != opcode)
      && (PIC_OPCODE_RETLW != opcode)
      && (VALUE_NONE != f)
      && !value_is_const(f)
      && !value_is_shared(f)) {
    variable_base_t base;
    unsigned long   full_file;
    unsigned        bank_sz;
    pic_opcode_t    op;
    variable_t      bvar;
    pic_code_t      brcode;
    pic_code_t      brlist;
    boolean_t       peeled_skips;

    /* adjacency-edge metadata for the PIC18 access-window candidate
     * layout; see pic_var.c */
    bvar = value_variable_get(f);
    if (bvar) {
      pic_var_banked_access_bump(bvar);
    }

    /*
     * check the last code in the chain. If it's a conditional, insert
     * these instructions before it. If that was wrong, it will be
     * caught later.
     */
    brlist = PIC_CODE_NONE;
    brcode = pic_code_list_tail_get(pf);
    peeled_skips = BOOLEAN_FALSE;
    while ((PIC_OPCODE_BTFSC == pic_code_op_get(brcode))
        || (PIC_OPCODE_BTFSS == pic_code_op_get(brcode))
        || (PIC_OPCODE_DECFSZ == pic_code_op_get(brcode))
        || (PIC_OPCODE_INCFSZ == pic_code_op_get(brcode))) {
      peeled_skips = BOOLEAN_TRUE;
      pic_code_list_remove(pf, brcode);
      pic_code_next_set(brcode, brlist);
      brlist = brcode;
      brcode = pic_code_list_tail_get(pf);
    }
    if (peeled_skips) {
      /* Peeling skips from the tail breaks the linear emit cache; if the
       * instruction immediately before the peeled skip chain is MOVLB, the
       * bank is still known for the following banked daop (same as if the
       * skips had not been temporarily removed). Otherwise stay conservative. */
      pic_emit_movl_bsr = PIC_EMIT_BSR_UNK;
      if (brcode
          && (PIC_OPCODE_MOVLB == pic_code_op_get(brcode))) {
        value_t mlit;

        mlit = pic_code_literal_get(brcode);
        if (mlit && value_is_const(mlit)) {
          pic_emit_movl_bsr = (unsigned) (value_const_get(mlit) & 0xffU);
        }
      }
    }
    /*
     * insert the appropriate data operation
     */
    base = value_base_get(f);

    if ((pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
        && (2 == pic_code_gen_pass_get(pf))) {
      if (VARIABLE_BASE_UNKNOWN == base) {
        variable_t    vv;
        const char   *nm;

        vv = value_variable_get(f);
        nm = (vv) ? variable_name_get(vv) : "?";
        pfile_log(pf, PFILE_LOG_ERR,
            "banked data op (MOVLB) for operand with unknown file address (%s); "
            "0xFFFF/256 is not a valid bank select",
            nm);
      }
    }
    /* Match pic_code_value_operand_for_pcode: bank select uses full file
     * address (base + const baseofs + bit byte + array/record ofs). */
    full_file = (unsigned long) base;
    if (VARIABLE_BASE_UNKNOWN != base) {
      value_t bofs;

      bofs = value_baseofs_get(f);
      if (bofs && value_is_const(bofs)) {
        full_file = (unsigned long) pic_var_chunk_addr_resolve(
            value_variable_get(f),
            (variable_base_t) (full_file + (unsigned long) value_const_get(bofs)));
      }
      full_file += (unsigned long) (value_bit_offset_get(f) / 8);
      full_file += (unsigned long) file_ofs;
    }
    if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
      /* PIC18 MOVLB and the 14H bank select share ONE opcode and ONE rule:
       * bank = full file address / the device's bank size. Until now this
       * was two byte-identical blocks differing only in a hardcoded 256UL
       * vs 128UL -- the same window rule pic_val_movl_bank_equal applies,
       * and the same "written twice" shape that produced the 2026-10-28
       * scaling defect. */
      pic_code_t         mclb;
      value_t            vop;
      variable_const_t   bank_lit;
      unsigned           bank_u;

      bank_lit = (variable_const_t) (full_file
          / (unsigned long) pic_target_bank_size_get(pf));
      bank_u   = (unsigned) bank_lit & 0xffU;
      mclb     = PIC_CODE_NONE;
      if (!force
          && (PIC_EMIT_BSR_UNK != pic_emit_movl_bsr)
          && (pic_emit_movl_bsr == bank_u)) {
        /* Sequential emit already selected this bank; mirror MOVLB use bump
         * in pic_variable_counters_set (pic.c) when no MOVLB insn exists. */
        value_use_ct_bump(f, CTR_BUMP_INCR);
      } else {
        mclb = pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLB, bank_lit);
        if (mclb) {
          vop = value_clone(f);
          if (vop) {
            pic_code_value_set(mclb, vop);
            /* Same file byte as the following data op; pic_opt_verify_skip
             * compares pic_code_ofs to the skip insn (see append_f_b). */
            pic_code_ofs_set(mclb, file_ofs);
            pic_instr_bank_op_linear_try_record(pf, mclb);
          } else {
            pfile_log_syserr(pf, ENOMEM);
          }
        }
      }
    } else {
      /* PIC14 (non-hybrid): pic_value_datalo/datahi use declaration base only.
       * Packed bits and byte offsets (file_ofs) live past bank 0 — match pcode
       * (pic_code_value_operand_for_pcode) and pic_opt_verify14 using full_file. */
      if (PIC_OPCODE_MOVWF == opcode
          && pic_classic_pic14_movwf_bank_already_selected(pf, f, file_ofs)) {
        while (brlist) {
          brcode = brlist;
          brlist = pic_code_next_get(brlist);
          pic_code_next_set(brcode, PIC_CODE_NONE);
          pic_code_list_append(pf, brcode);
        }
        return;
      }

      bank_sz = pic_target_bank_size_get(pf);
      op = pic_value_datalo_get(pf, f);
      if (PIC_OPCODE_NONE != op) {
        pic_code_t code;

        if (VARIABLE_BASE_UNKNOWN != base) {
          op = (full_file & (unsigned long) bank_sz)
              ? PIC_OPCODE_DATALO_SET
              : PIC_OPCODE_DATALO_CLR;
        }
        code = pic_instr_alloc(pf, LABEL_NONE, op);
        if (code) {
          pic_code_value_set(code, f);
          pic_code_ofs_set(code, file_ofs);
        }
      }

      op = pic_value_datahi_get(pf, f);
      if (PIC_OPCODE_NONE != op) {
        pic_code_t code;

        if (VARIABLE_BASE_UNKNOWN != base) {
          op = (full_file & (unsigned long) (2U * bank_sz))
              ? PIC_OPCODE_DATAHI_SET
              : PIC_OPCODE_DATAHI_CLR;
        }
        code = pic_instr_alloc(pf, LABEL_NONE, op);
        if (code) {
          pic_code_value_set(code, f);
          pic_code_ofs_set(code, file_ofs);
        }
      }
    }
    /* ...and do any fixup */
    while (brlist) {
      brcode = brlist;
      brlist = pic_code_next_get(brlist);
      pic_code_next_set(brcode, PIC_CODE_NONE);
      pic_code_list_append(pf, brcode);
    }
  }
}

pic_code_t pic_instr_append_f_d(pfile_t *pf, pic_opcode_t op, value_t f, 
    size_t ofs, pic_opdst_t dst)
{
  pic_code_t code;
  boolean_t  indirect;
  value_t    logical;

  indirect = BOOLEAN_FALSE;
  logical  = VALUE_NONE;
  if (value_is_indirect(f)) {
    indirect = BOOLEAN_TRUE;
    logical  = value_clone(f);
    f = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
    ofs = 0;
  }
  pic_instr_emit_bsr_cache_reset_before_hybrid_incdec(pf, op);
  pic_instr_append_daop(pf, f, op,
      pic_instr_force_daop_pic14(pf, op) ? BOOLEAN_TRUE : BOOLEAN_FALSE,
      ofs);
  code = pic_instr_alloc(pf, LABEL_NONE, op);
  if (code) {
    pic_code_value_set(code, f);
    pic_code_ofs_set(code, ofs);
    pic_code_dst_set(code, dst);
    if (VALUE_NONE != logical) {
      pic_code_memval_set(code, logical);
      value_release(logical);
    }
    pic_instr_bank_op_linear_try_record(pf, code);
  } else if (VALUE_NONE != logical) {
    value_release(logical);
  }
  if (indirect) {
    value_release(f);
  }
  return code;
}

pic_code_t pic_instr_append_lfsr(pfile_t *pf, unsigned which,
    value_t val, size_t ofs)
{
  pic_code_t code;

  code = pic_instr_alloc(pf, LABEL_NONE, PIC_OPCODE_LFSR);
  pic_code_fsr_n_set(code, which);
  pic_code_value_set(code, val);
  pic_code_ofs_set(code, ofs);
  return code;
}

pic_code_t pic_instr_append_reg_d(pfile_t *pf, pic_opcode_t op, const char *reg,
  pic_opdst_t dst)
{
  value_t    src;
  pic_code_t code;

  src = pfile_value_find(pf, PFILE_LOG_ERR, reg);
  if (src) {
    code = pic_instr_append_f_d(pf, op, src, 0, dst);
    value_release(src);
  } else {
    code = PIC_CODE_NONE;
  }
  return code;
}


pic_code_t pic_instr_append_f(pfile_t *pf, pic_opcode_t op, value_t f, 
    size_t ofs)
{
  pic_code_t code;
  boolean_t  indirect;
  value_t    logical;

  indirect = BOOLEAN_FALSE;
  logical  = VALUE_NONE;
  if (value_is_indirect(f)) {
    indirect = BOOLEAN_TRUE;
    logical  = value_clone(f);
    f = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
    ofs = 0;
  }

  pic_instr_emit_bsr_cache_reset_before_hybrid_incdec(pf, op);
  pic_instr_append_daop(pf, f, op,
      pic_instr_force_daop_pic14(pf, op) ? BOOLEAN_TRUE : BOOLEAN_FALSE,
      ofs);
  code = pic_instr_alloc(pf, LABEL_NONE, op);
  value_variable_get(f);
  if (code) {
    pic_code_value_set(code, f);
    pic_code_ofs_set(code, ofs);
    if (VALUE_NONE != logical) {
      pic_code_memval_set(code, logical);
      value_release(logical);
    }
    pic_instr_bank_op_linear_try_record(pf, code);
  } else if (VALUE_NONE != logical) {
    value_release(logical);
  }
  if (indirect) {
    value_release(f);
  }
  return code;
}

pic_code_t pic_instr_append_reg(pfile_t *pf, pic_opcode_t op, const char *reg)
{
  value_t    tmp;
  pic_code_t code;

  tmp = pfile_value_find(pf, PFILE_LOG_ERR, reg);
  if (tmp) {
    code = pic_instr_append_f(pf, op, tmp, 0);
    value_release(tmp);
  } else {
    code = PIC_CODE_NONE;
  }
  return code;
}

/*
 * Load FSR0 with an ABSOLUTE file address on the wide cores.
 *
 * PIC18 and the 14H both address through FSR0L/FSR0H, and this exact
 * four-instruction sequence was hand-written at five sites in the
 * indirect path. They must never disagree: every indirect read/write
 * goes through whatever was loaded here last, so a drifted copy is a
 * silent wrong-address access (the shape of the 2026-10-28 defects).
 * The classic 14-bit core uses FSR + IRP instead and does NOT come
 * through this helper.
 */
void pic_fsr0_load_addr(pfile_t *pf, variable_base_t base)
{
  unsigned lsb;
  unsigned msb;

  lsb = (unsigned) base & 0xff;
  msb = ((unsigned) base >> 8) & 0xff;
  /* CLRF for a zero byte (one word instead of MOVLW+ MOVWF). Flags are
   * unchanged by this: MOVLW 0 sets Z, CLRF of a zero byte also sets Z,
   * and every intermediate MOVWF leaves the flags alone -- so the final
   * Z/W state matches the unrolled form byte for byte in behaviour. */
  if (lsb) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, lsb);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
  } else {
    pic_instr_append_reg(pf, PIC_OPCODE_CLRF, "_fsr0l");
  }
  if (msb) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, msb);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
  } else {
    pic_instr_append_reg(pf, PIC_OPCODE_CLRF, "_fsr0h");
  }
}


pic_code_t pic_instr_append(pfile_t *pf, pic_opcode_t op)
{
  /* this might be a hack, but the 12 bit cores do not have
   * a simple return instruction; instead `retlw 0' is used */
  return (pic_is_12bit(pf) && (PIC_OPCODE_RETURN == op))
    ? pic_instr_append_w_kn(pf, PIC_OPCODE_RETLW, 0)
    : pic_instr_alloc(pf, LABEL_NONE, op);
}

pic_code_t pic_instr_append_f_b(pfile_t *pf, pic_opcode_t op, value_t f, 
    size_t ofs, value_t bit)
{
  pic_code_t code;
  boolean_t  indirect;
  value_t    logical;

  indirect = BOOLEAN_FALSE;
  logical  = VALUE_NONE;
  if (value_is_indirect(f)) {
    indirect = BOOLEAN_TRUE;
    logical  = value_clone(f);
    f = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
    ofs = 0;
  }
  pic_instr_emit_bsr_cache_reset_before_hybrid_incdec(pf, op);
  pic_instr_append_daop(pf, f, op,
      pic_instr_force_daop_pic14(pf, op) ? BOOLEAN_TRUE : BOOLEAN_FALSE,
      ofs);
  code = pic_instr_alloc(pf, LABEL_NONE, op);
  if (code) {
    pic_code_value_set(code, f);
    pic_code_ofs_set(code, ofs);
    if (VALUE_NONE != logical) {
      pic_code_memval_set(code, logical);
      value_release(logical);
    }
    if (bit) {
      value_use_ct_bump(bit, CTR_BUMP_INCR);
    }
    pic_code_literal_set(code, bit);
    pic_instr_bank_op_linear_try_record(pf, code);
  } else if (VALUE_NONE != logical) {
    value_release(logical);
  }
  if (indirect) {
    value_release(f);
  }
  return code;
}

pic_code_t pic_instr_append_reg_bn(pfile_t *pf, pic_opcode_t op, 
    const char *reg, variable_const_t bit)
{
  value_t    vreg;
  pic_code_t code;

  code = PIC_CODE_NONE;
  vreg = pfile_value_find(pf, PFILE_LOG_ERR, reg);
  if (vreg) {
    value_t vflag;

    vflag = pfile_constant_get(pf, bit, VARIABLE_DEF_NONE);
    if (vflag) {
      code = pic_instr_append_f_b(pf, op, vreg, 0, vflag);
      value_release(vflag);
    }
    value_release(vreg);
  }
  return code;
}

pic_code_t pic_instr_append_reg_flag(pfile_t *pf, pic_opcode_t op, 
    const char *reg, const char *flag)
{
  value_t    vreg;
  pic_code_t code;

  code = PIC_CODE_NONE;
  vreg = pfile_value_find(pf, PFILE_LOG_ERR, reg);
  if (vreg) {
    value_t vflag;

    vflag = pfile_value_find(pf, PFILE_LOG_ERR, flag);
    if (vflag) {
      code = pic_instr_append_f_b(pf, op, vreg, 0, vflag);
      value_release(vflag);
    }
    value_release(vreg);
  }
  return code;
}

pic_code_t pic_instr_append_f_bn(pfile_t *pf, pic_opcode_t op,
    value_t f, size_t ofs, variable_const_t n)
{
  value_t    vn;
  pic_code_t code;

  vn = pfile_constant_get(pf, n, VARIABLE_DEF_NONE);
  if (vn) {
    code = pic_instr_append_f_b(pf, op, f, ofs
        /*+ value_const_get(value_baseofs_get(f))*/, vn);
    value_release(vn);
  } else {
    code = PIC_CODE_NONE;
  }
  return code;
}

pic_code_t pic_instr_append_w_k(pfile_t *pf, pic_opcode_t op, 
    value_t k)
{
  pic_code_t code;

  pic_instr_append_daop(pf, k, op, BOOLEAN_FALSE, 0);
  if ((PIC_OPCODE_MOVLB == op)
      && (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
      && (VALUE_NONE != k)
      && value_is_const(k)
      && !pic_instr_default_flag_test(pf, PIC_CODE_FLAG_NO_OPTIMIZE)) {
    unsigned bank_u;

    bank_u = (unsigned) (value_const_get(k) & 0xffU);
    if ((PIC_EMIT_BSR_UNK != pic_emit_movl_bsr)
        && (pic_emit_movl_bsr == bank_u)) {
      /* Literal MOVLB bypasses append_daop (const k); same as append_daop's
       * skip when the sequential BSR bank already matches. */
      value_use_ct_bump(k, CTR_BUMP_INCR);
      return PIC_CODE_NONE;
    }
  }
  code = pic_instr_alloc(pf, LABEL_NONE, op);
  if (code) {
    pic_code_literal_set(code, k);
    pic_emit_bsr_note_appended(pf, code);
  }
  return code;
}

pic_code_t pic_instr_append_w_kn(pfile_t *pf, pic_opcode_t op,
    variable_const_t n)
{
  value_t    vn;
  pic_code_t code;

  vn = pfile_constant_get(pf, n, VARIABLE_DEF_NONE);
  if (vn) {
    code = pic_instr_append_w_k(pf, op, vn);
    value_release(vn);
  } else {
    code = PIC_CODE_NONE;
  }
  return code;
}

pic_code_t pic_instr_append_n(pfile_t *pf, pic_opcode_t op, label_t dst)
{
  pic_code_t code;

  if (dst 
      && !(pic_instr_default_flag_test(pf, PIC_CODE_FLAG_NO_OPTIMIZE))
      && !pic_is_16bit(pf)) {
    /* if the previous code is a skip conditional insert the 
     * PCLATH instructions *before* code_pv, otherwise insert
     * them *after* code_pv */
    pic_code_t brcode;
    pic_code_t brlist;
    value_t    code_sz;
    unsigned   page_sz;

    code_sz = pfile_value_find(pf, PFILE_LOG_ERR, "_code_size");
    page_sz = pic_target_page_size_get(pf);
    brlist = PIC_CODE_NONE;
    brcode = pic_code_list_tail_get(pf);
    while ((PIC_OPCODE_BTFSC == pic_code_op_get(brcode))
        || (PIC_OPCODE_BTFSS == pic_code_op_get(brcode))
        || (PIC_OPCODE_DECFSZ == pic_code_op_get(brcode))
        || (PIC_OPCODE_INCFSZ == pic_code_op_get(brcode))
        || (PIC_OPCODE_MOVLB == pic_code_op_get(brcode))) {
      pic_code_list_remove(pf, brcode);
      pic_code_next_set(brcode, brlist);
      brlist = brcode;
      brcode = pic_code_list_tail_get(pf);
    }
    /* note: we've not enough information to set the branchbits yet,
     *       just make sure they appear *before* any conditionals */
    if (pic_is_14bit_hybrid(pf)) {
      code = pic_instr_alloc(pf, LABEL_NONE, PIC_OPCODE_MOVLP);
      pic_code_brdst_set(code, dst);
      pic_code_ofs_set(code, 1);
    } else {
      if (value_const_get(code_sz) > page_sz) {
        code = pic_instr_alloc(pf, LABEL_NONE, PIC_OPCODE_BRANCHLO_CLR);
        pic_code_brdst_set(code, dst);
      }
      if (value_const_get(code_sz) > (2 * page_sz)) {
        code = pic_instr_alloc(pf, LABEL_NONE, PIC_OPCODE_BRANCHHI_CLR);
        pic_code_brdst_set(code, dst);
      }
    }

    while (brlist) {
      brcode = brlist;
      brlist = pic_code_next_get(brlist);
      pic_code_next_set(brcode, PIC_CODE_NONE);
      pic_code_list_append(pf, brcode);
    }
    value_release(code_sz);
  }
  code = pic_instr_alloc(pf, LABEL_NONE, op);
  pic_code_brdst_set(code, dst);
  return code;
}

/* Shared with pic_code_to_pcode: f-field and literal for the value operand.
 * When p_bsr_file_addr is not NULL, set *p_bsr_has_file for a direct banked
 * file address; *p_bsr_file_addr is the full file address (before the f
 * field mask) used by pic_opt_verify16. */
static void
pic_code_value_operand_for_pcode(
  pfile_t *pf,
  const pic_code_t code,
  value_t cv,
  boolean_t lit_assigned,
  unsigned *pval,
  unsigned *pliteral,
  boolean_t *p_bsr_has_file,
  variable_base_t *p_bsr_file_addr)
{
  unsigned val;
  unsigned bank_mask;

  bank_mask = (unsigned) (pic_target_bank_size_get(pf) - 1U);
  if (p_bsr_has_file) {
    *p_bsr_has_file = BOOLEAN_FALSE;
  }
  /* Const-pointee reads use a value typed as const byte but the operand is
   * still the pointer variable's storage (file address). value_is_const is
   * true from the pointee def; value_const_get would return initializer data,
   * not the pointer slot address (see 16f877 const-pointer gpsim tests).
   * Unnamed/named const *variables* (pfile_constant_get pool) still have a
   * file address; using value_const_get for F-addressing opcodes produced
   * bare movwf 0 / wrong-bank stores (e.g. _string_error := 0). Only use the
   * raw numeric path when there is no backing variable_t. */
  if (value_is_const(cv) && !value_variable_get(cv)) {
    val = (unsigned) value_const_get(cv);
  } else if (value_is_indirect(cv)) {
    value_t ind;

    ind = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
    val = (unsigned) value_base_get(ind);
    value_release(ind);
  } else {
    unsigned full;
    value_t    bofs;

    full = (unsigned) value_base_get(cv);
    bofs = value_baseofs_get(cv);
    if (bofs && value_is_const(bofs)) {
      full = (unsigned) pic_var_chunk_addr_resolve(value_variable_get(cv),
          (variable_base_t) (full + (unsigned) value_const_get(bofs)));
    }
    full += (unsigned) (value_bit_offset_get(cv) / 8)
        + (unsigned) pic_code_ofs_get(code);
    if (pic_code_flag_test(code, PIC_CODE_FLAG_LABEL_HIGH)) {
      val = full >> 8;
    } else if (pic_code_flag_test(code, PIC_CODE_FLAG_LABEL_UPPER)) {
      val = full >> 16;
    } else {
      val = full;
    }
    if (value_base_get(cv) == VARIABLE_BASE_UNKNOWN) {
      if (pfile_debug_sub_test(pf, PFILE_DBGSUB_UNKBASE)) {
        fprintf(stderr, "%lx (%x + %lu): ",
            (unsigned long) pic_code_pc_get(code),
            (unsigned) value_base_get(cv),
            (ulong) pic_code_ofs_get(code));
        variable_dump(value_variable_get(cv), stderr);
        fprintf(stderr, "\n");
      }
      /*assert(0);*/
    } else {
      if (!lit_assigned) {
        *pliteral = val;
      }
    }
    val &= bank_mask;
    if (p_bsr_has_file && p_bsr_file_addr
        && !pic_code_flag_test(code, PIC_CODE_FLAG_LABEL_HIGH)
        && !pic_code_flag_test(code, PIC_CODE_FLAG_LABEL_UPPER)
        && (VARIABLE_BASE_UNKNOWN != value_base_get(cv))) {
      *p_bsr_has_file = BOOLEAN_TRUE;
      *p_bsr_file_addr = (variable_base_t) full;
    }
  }
  *pval = val;
  /* MOVLB attaches pic_code_literal (bank) plus a file value for ref; do not
   * replace the bank literal with the operand's bit index. Other opcodes keep
   * the value_is_bit + pliteral behaviour. */
  if (value_is_bit(cv)
      && !(lit_assigned && PIC_OPCODE_MOVLB == pic_code_op_get(code))) {
    *pliteral  = (unsigned) value_bit_offset_get(cv);
  }
}

boolean_t
pic_code_bsr_verify_data_addr(pfile_t *pf, const pic_code_t code,
    unsigned long *paddr)
{
  value_t         cv;
  boolean_t       has;
  boolean_t       lit_assigned;
  variable_base_t faddr;
  unsigned        vtmp;
  unsigned        ltmp;

  if (!paddr) {
    return BOOLEAN_FALSE;
  }
  *paddr = 0;
  if (!pic_code_value_get(code)) {
    return BOOLEAN_FALSE;
  }
  lit_assigned = BOOLEAN_FALSE;
  if (pic_code_brdst_get(code) || pic_code_literal_get(code)) {
    lit_assigned = BOOLEAN_TRUE;
  }
  cv = pic_code_value_get(code);
  ltmp = 0;
  vtmp = 0;
  has  = BOOLEAN_FALSE;
  faddr = 0;
  pic_code_value_operand_for_pcode(pf, code, cv, lit_assigned, &vtmp, &ltmp,
      &has, &faddr);
  if (!has) {
    return BOOLEAN_FALSE;
  }
  *paddr = (unsigned long) faddr;
  return BOOLEAN_TRUE;
}

void pic_instr_bank_op_linear_try_record(pfile_t *pf, pic_code_t code)
{
  pic_opcode_t op;
  unsigned long addr;

  if (!code || (!pic_is_16bit(pf) && !pic_is_14bit_hybrid(pf))) {
    return;
  }
  op = pic_code_op_get(code);
  switch (op) {
    case PIC_OPCODE_BTFSC:
    case PIC_OPCODE_BTFSS:
    case PIC_OPCODE_DECFSZ:
    case PIC_OPCODE_DCFSNZ:
    case PIC_OPCODE_INCFSZ:
    case PIC_OPCODE_INFSNZ:
    case PIC_OPCODE_MOVLB:
      break;
    default:
      return;
  }
  if (pic_code_bsr_verify_data_addr(pf, code, &addr)) {
    pic_code_bank_op_linear_set(code, addr, BOOLEAN_TRUE);
  } else {
    pic_code_bank_op_linear_set(code, 0, BOOLEAN_FALSE);
  }
}

boolean_t pic_code_to_pcode(pfile_t *pf, const pic_code_t code, 
    pic_code_to_pcode_t *dst)
{
  unsigned  literal;
  unsigned  val;
  boolean_t lit_assigned;
  boolean_t rc;

  lit_assigned = BOOLEAN_FALSE;

  if (pic_code_brdst_get(code)) {
    literal = label_pc_get(pic_code_brdst_get(code));
    lit_assigned = BOOLEAN_TRUE;
    /* this is kind of a hack, but I if there's a branch
       destination, I use offset 0 for the LSB, and offset
       1 for the MSB */
    switch (pic_code_ofs_get(code)) {
      case 0: break;
      case 1: literal >>= 8; break;
      case 2: literal >>= 16; break;
    }
  } else if (pic_code_literal_get(code)) {
    lit_assigned = BOOLEAN_TRUE;
    if (value_is_const(pic_code_literal_get(code))) {
      literal = value_const_get(pic_code_literal_get(code));
    } else {
      literal = value_base_get(pic_code_literal_get(code))
        + value_const_get(value_baseofs_get(pic_code_literal_get(code)));
    }
  } else {
    literal = 0x0000;
  }

  if (pic_code_value_get(code)) {
    value_t cv;

    cv = pic_code_value_get(code);
    pic_code_value_operand_for_pcode(pf, code, cv, lit_assigned, &val,
        &literal, 0, 0);
  } else {
    val = 0xffff;
  }

  rc = BOOLEAN_FALSE;
  switch (pic_target_cpu_get(pf)) {
    case PIC_TARGET_CPU_NONE:
    case PIC_TARGET_CPU_SX_12:
      pfile_log(pf, PFILE_LOG_ERR, "core type not set");
      break;
    case PIC_TARGET_CPU_12BIT:
      rc = pic12_code_to_pcode(pf, code, val, literal, dst);
      break;
    case PIC_TARGET_CPU_14BIT:
      rc = pic14_code_to_pcode(pf, code, val, literal, dst);
      break;
    case PIC_TARGET_CPU_14HBIT:
      rc = pic14h_code_to_pcode(pf, code, val, literal, dst);
      break;
    case PIC_TARGET_CPU_16BIT:
      rc = pic16_code_to_pcode(pf, code, val, literal, dst);
      break;
    case PIC_TARGET_CPU_MIPS32:
      break;
    case PIC_TARGET_CPU_ESP32:
      break;  /* ESP32 codegen is in libesp32; never reached here */
  }
  return rc;
}

boolean_t pic_opcode_is_branch_bit(pic_opcode_t op)
{
  return (PIC_OPCODE_BRANCHLO_CLR == op)
    || (PIC_OPCODE_BRANCHLO_SET == op)
    || (PIC_OPCODE_BRANCHLO_NOP == op)
    || (PIC_OPCODE_BRANCHHI_CLR == op)
    || (PIC_OPCODE_BRANCHHI_SET == op)
    || (PIC_OPCODE_BRANCHHI_NOP == op);
}

boolean_t pic_opcode_is_data_bit(pic_opcode_t op)
{
  return (PIC_OPCODE_DATALO_CLR == op)
    || (PIC_OPCODE_DATALO_SET == op)
    || (PIC_OPCODE_DATAHI_CLR == op)
    || (PIC_OPCODE_DATAHI_SET == op);
}


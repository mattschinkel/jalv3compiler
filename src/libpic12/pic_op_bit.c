/************************************************************
 **
 ** pic_op_bit.c : PIC operator generation -- bit and bitfield assignment
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **           (c) 2020-2025, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: split out of pic_op.c (Tier-A decomposition, 2026-10-31);
 **          code moved verbatim, forward declarations replaced by
 **          pic_op_priv.h prototypes.
 **
 ************************************************************/

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <math.h>

#include "../libcore/pf_proc.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pf_op.h"
#include "pic_opfn.h"
#include "pic_stvar.h"
#include "pic_var.h"
#include "piccolst.h"
#include "pic_inst.h"
#include "pic_op.h"
#include "pic_op_priv.h"

/* defined in this TU (prototypes keep definition order free) */
static value_t pic_bit_overlay_create(value_t val);
static void pic_dynamic_bit_ref_release(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref);
static boolean_t pic_dynamic_bit_ref_prepare(pfile_t *pf, value_t val,
    pic_dynamic_bit_ref_t *ref);
static void pic_dynamic_bit_ref_linear_addr_setup(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref, variable_base_t base);
static void pic_dynamic_bit_ref_index_ge_chunk(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref, variable_sz_t chunk_sz, label_t lbl_ge);
static void pic_dynamic_bit_ref_emit_set(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref);
static void pic_dynamic_bit_ref_emit_clear(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref);
static boolean_t pic_dynamic_field_width_get(value_t val,
    variable_sz_t *width);
static boolean_t pic_dynamic_field_load_to_w(pfile_t *pf, value_t src);
static boolean_t pic_assign_to_dynamic_field(pfile_t *pf, value_t dst,
    value_t src);
static boolean_t pic_assign_to_dynamic_bit(pfile_t *pf, value_t dst,
    value_t src);

/* create an integer variable that overlays a bit variable */
static value_t pic_bit_overlay_create(value_t val)
{
  variable_t     var;
  variable_def_t def;
  unsigned       bit_ofs;
  unsigned       sz;
  value_t        dst;

  bit_ofs = value_bit_offset_get(val);
  sz      = value_sz_get(val);

  /* create a byte array covering dst */
  def   = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, (bit_ofs + sz + 7) / 8);
  var   = variable_alloc(TAG_NONE, def);
  variable_master_set(var, variable_master_get(value_variable_get(val)));

  dst   = value_alloc(var);
  variable_release(var);
  return dst;
}

boolean_t pic_value_is_dynamic_bit_array_element(value_t val)
{
  value_t baseofs;
  variable_t var;

  baseofs = value_baseofs_get(val);
  var = value_variable_get(val);
  return value_is_bit(val)
    && value_flag_test(val, VALUE_FLAG_ARRAY)
    && ((baseofs && !value_is_const(baseofs))
      || (var && (pic_variable_chunk_ct_get(var) > 1)));
}

static void pic_dynamic_bit_ref_release(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  value_release(ref->byte);
  if (ref->mask) {
    pic_var_temp_release(pf, ref->mask);
  }
  if (ref->bit_index) {
    pic_var_temp_release(pf, ref->bit_index);
  }
  if (ref->byte_index) {
    pic_var_temp_release(pf, ref->byte_index);
  }
  ref->byte       = VALUE_NONE;
  ref->mask       = VALUE_NONE;
  ref->byte_index = VALUE_NONE;
  ref->bit_index  = VALUE_NONE;
}

static boolean_t pic_dynamic_bit_ref_prepare(pfile_t *pf, value_t val,
    pic_dynamic_bit_ref_t *ref)
{
  value_t       baseofs;
  value_t       cval;
  boolean_t     baseofs_release;
  variable_t    var;
  variable_t    master;
  variable_def_t def;
  variable_sz_t idx_sz;
  unsigned      bit_base;

  ref->byte       = VALUE_NONE;
  ref->mask       = VALUE_NONE;
  ref->byte_index = VALUE_NONE;
  ref->bit_index  = VALUE_NONE;

  baseofs_release = BOOLEAN_FALSE;
  baseofs = value_baseofs_get(val);
  var     = value_variable_get(val);
  if (!var) {
    return BOOLEAN_FALSE;
  }
  if (!baseofs) {
    baseofs = pfile_constant_get(pf, 0, VARIABLE_DEF_NONE);
    baseofs_release = BOOLEAN_TRUE;
  }
  if (!baseofs) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  bit_base = value_bit_offset_get(val);
  /*
   * Open `bit in bits[]` formals read buffer-relative bits: the normalize
   * buffer re-bases the packed actual at bit 0, but the formal binding
   * leaks the actual's intra-byte offset (classic reserved bitbucket bit 0)
   * onto the value — drop it so bits[i] resolves at buffer bit i on every
   * core (same contract as the static-offset arm in pic_assign_from_bit).
   * fix_pic18_open_bit_formal_indirect.md
   */
  if (bit_base && var && variable_is_pointer(var)
      && value_is_indirect(val) && baseofs
      && !value_is_const(baseofs)) {
    bit_base = 0;
  }
  idx_sz = value_byte_sz_get(baseofs);
  if (idx_sz < 1) {
    idx_sz = 1;
  }
  if ((idx_sz < 2) && (bit_base + variable_sz_get(var) > 255)) {
    idx_sz = 2;
  }

  ref->byte_index = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, idx_sz);
  ref->bit_index  = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  ref->mask       = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  if (!ref->byte_index || !ref->bit_index || !ref->mask) {
    if (baseofs_release) {
      value_release(baseofs);
    }
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }

  pic_op(pf, OPERATOR_ASSIGN, ref->byte_index, baseofs, VALUE_NONE);
  if (baseofs_release) {
    value_release(baseofs);
  }
  if (bit_base) {
    cval = pfile_constant_get(pf, bit_base, VARIABLE_DEF_NONE);
    pic_op(pf, OPERATOR_ADD, ref->byte_index, ref->byte_index, cval);
    value_release(cval);
  }

  cval = pfile_constant_get(pf, 7, VARIABLE_DEF_NONE);
  pic_op(pf, OPERATOR_ANDB, ref->bit_index, ref->byte_index, cval);
  value_release(cval);

  cval = pfile_constant_get(pf, 3, VARIABLE_DEF_NONE);
  pic_op(pf, OPERATOR_SHIFT_RIGHT, ref->byte_index, ref->byte_index, cval);
  value_release(cval);

  cval = pfile_constant_get(pf, 1, VARIABLE_DEF_NONE);
  pic_op(pf, OPERATOR_SHIFT_LEFT, ref->mask, cval, ref->bit_index);
  value_release(cval);

  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, 1);
  if (!def) {
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  master = variable_master_get(var);
  if (!master) {
    master = var;
  }
  var = variable_alloc(TAG_NONE, def);
  if (!var) {
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  variable_master_set(var, master);
  ref->byte = value_alloc(var);
  variable_release(var);
  if (!ref->byte) {
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  value_flag_set(ref->byte, VALUE_FLAG_ARRAY);
  value_baseofs_set(ref->byte, ref->byte_index);
  return BOOLEAN_TRUE;
}

static void pic_dynamic_bit_ref_linear_addr_setup(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref, variable_base_t base)
{
  variable_sz_t   idx_sz;

  idx_sz = value_byte_sz_get(ref->byte_index);
  if (idx_sz < 1) {
    idx_sz = 1;
  }
  pic_stvar_fsr_mark(pf);
  if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, ref->byte_index, 0,
        PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_ADDWFc, ref->byte_index, 1,
          PIC_OPDST_W);
    } else {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
    }
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
  } else {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, ref->byte_index, 0,
        PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, ref->byte_index, 1,
          PIC_OPDST_W);
    }
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref->bit_index, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
    pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, ref->bit_index, 0, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
  }
}

static void pic_dynamic_bit_ref_index_ge_chunk(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref, variable_sz_t chunk_sz, label_t lbl_ge)
{
  variable_sz_t idx_sz;

  idx_sz = value_byte_sz_get(ref->byte_index);
  if (idx_sz < 1) {
    idx_sz = 1;
  }
  if (chunk_sz >= 256) {
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->byte_index, 1,
          PIC_OPDST_W);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ge);
    }
  } else {
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->byte_index, 1,
          PIC_OPDST_W);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ge);
    }
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, chunk_sz);
    pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, ref->byte_index, 0,
        PIC_OPDST_W);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ge);
  }
}

void pic_dynamic_bit_ref_addr_setup(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  variable_t      var;
  variable_ct_t  chunk_ct;

  var = value_variable_get(ref->byte);
  chunk_ct = var ? pic_variable_chunk_ct_get(var) : 0;
  if (chunk_ct > 1) {
    label_t done;
    variable_ct_t ii;

    done = pfile_label_alloc(pf, 0);
    for (ii = 0; ii < chunk_ct; ii++) {
      label_t         next;
      variable_base_t chunk_base;
      variable_sz_t   chunk_sz;

      chunk_base = pic_variable_chunk_base_get(var, ii);
      chunk_sz   = pic_variable_chunk_sz_get(var, ii);
      next = LABEL_NONE;
      if (ii + 1 < chunk_ct) {
        next = pfile_label_alloc(pf, 0);
        pic_dynamic_bit_ref_index_ge_chunk(pf, ref, chunk_sz, next);
      }
      pic_dynamic_bit_ref_linear_addr_setup(pf, ref, chunk_base);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);
      if (next) {
        value_t cval;

        pic_instr_append_label(pf, next);
        cval = pfile_constant_get(pf, chunk_sz, VARIABLE_DEF_NONE);
        if (cval) {
          pic_op(pf, OPERATOR_SUB, ref->byte_index, ref->byte_index, cval);
          value_release(cval);
        } else {
          pfile_log_syserr(pf, ENOMEM);
        }
        label_release(next);
      }
    }
    pic_instr_append_label(pf, done);
    label_release(done);
  } else {
    pic_dynamic_bit_ref_linear_addr_setup(pf, ref, value_base_get(ref->byte));
  }
}

static void pic_dynamic_bit_ref_emit_set(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  pic_dynamic_bit_ref_addr_setup(pf, ref);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->byte, 0, PIC_OPDST_W);
  pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, ref->mask, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref->byte, 0);
}

static void pic_dynamic_bit_ref_emit_clear(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  pic_dynamic_bit_ref_addr_setup(pf, ref);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->mask, 0, PIC_OPDST_W);
  pic_instr_append_w_kn(pf, PIC_OPCODE_XORLW, 0xff);
  pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, ref->byte, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref->byte, 0);
}

/* Width-aware load/store for runtime-indexed `bit*N` elements (N in
 * 2..8). The 1-bit dynamic machinery above handles `var bit` arrays;
 * until 2026-10-27 multi-bit fields also routed there and every access
 * collapsed to a single set/clear of bit (idx & 7) of byte idx/8 with
 * the index UNSCALED -- a[k] wrote one bit at the wrong position and
 * reads returned 0/1 (the bit*N runtime defect). These emitters treat
 * the element as an N-bit field at runtime (byte, bit): 16-bit
 * rotate-loop shifts build the positioned mask/value pair, a runtime
 * straddle test (bit_index + N > 8) covers the second byte, and the
 * second address is a re-run of the address setup after a carry-aware
 * increment of the byte-index temp. */
static boolean_t pic_dynamic_field_width_get(value_t val,
    variable_sz_t *width)
{
  variable_sz_t w;

  w = value_sz_get(val);
  if ((w > 1) && (w <= 8)) {
    if (width) {
      *width = w;
    }
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

/* leaves the N-bit field value in W; Z reflects it (callers of the
 * 1-bit loader contract) */
static boolean_t pic_dynamic_field_load_to_w(pfile_t *pf, value_t src)
{
  pic_dynamic_bit_ref_t ref;
  variable_sz_t        w;
  value_t              t0;
  value_t              t1;
  value_t              cnt;
  label_t              lbl_nohi;
  label_t              lbl_loop;
  label_t              lbl_done;

  if (!pic_dynamic_field_width_get(src, &w)) {
    return BOOLEAN_FALSE;
  }
  if (!pic_dynamic_bit_ref_prepare(pf, src, &ref)) {
    return BOOLEAN_FALSE;
  }
  t0 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  t1 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  cnt = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  if (!t0 || !t1 || !cnt) {
    pfile_log_syserr(pf, ENOMEM);
  } else {
    lbl_nohi = pfile_label_alloc(pf, 0);
    lbl_loop = pfile_label_alloc(pf, 0);
    lbl_done = pfile_label_alloc(pf, 0);

    /* t0 = *byte; t1 = straddle ? *(byte+1) : 0 */
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t0, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t1, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW, 8 - w);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_nohi);
    pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 0,
        PIC_OPDST_F);
    if (value_byte_sz_get(ref.byte_index) > 1) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 1,
          PIC_OPDST_F);
    }
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t1, 0);
    pic_instr_append_label(pf, lbl_nohi);

    /* (t1:t0) >>= bit_index, then W = t0 & ((1<<N)-1). The rrf pair
     * order shifts t1's LSB into t0's MSB; bits discarded from t0 can
     * never re-enter t0's low N bits within 7 shifts, so no carry
     * hygiene is needed between iterations. */
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, cnt, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
    pic_instr_append_label(pf, lbl_loop);
    pic_instr_append_f_d(pf, PIC_OPCODE_RRF, t1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RRF, t0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, cnt, 0, PIC_OPDST_F);
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_loop);
    pic_instr_append_label(pf, lbl_done);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t0, 0, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, (1 << w) - 1);

    label_release(lbl_done);
    label_release(lbl_loop);
    label_release(lbl_nohi);
  }
  if (cnt) {
    pic_var_temp_release(pf, cnt);
  }
  if (t1) {
    pic_var_temp_release(pf, t1);
  }
  if (t0) {
    pic_var_temp_release(pf, t0);
  }
  pic_dynamic_bit_ref_release(pf, &ref);
  return (t0 && t1 && cnt) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* N-bit field store at a runtime (byte, bit) */
static boolean_t pic_assign_to_dynamic_field(pfile_t *pf, value_t dst,
    value_t src)
{
  pic_dynamic_bit_ref_t ref;
  variable_sz_t        w;
  value_t              sval;
  value_t              m0;
  value_t              m1;
  value_t              v0;
  value_t              v1;
  value_t              cnt;
  label_t              lbl_nohi;
  label_t              lbl_loop;
  label_t              lbl_done;
  boolean_t            ok;

  if (!pic_dynamic_field_width_get(dst, &w)) {
    return BOOLEAN_FALSE;
  }
  if (!pic_dynamic_bit_ref_prepare(pf, dst, &ref)) {
    return BOOLEAN_FALSE;
  }
  sval = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  m0 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  m1 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  v0 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  v1 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  cnt = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  ok = (sval && m0 && m1 && v0 && v1 && cnt) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  if (!ok) {
    pfile_log_syserr(pf, ENOMEM);
  } else {
    lbl_nohi = pfile_label_alloc(pf, 0);
    lbl_loop = pfile_label_alloc(pf, 0);
    lbl_done = pfile_label_alloc(pf, 0);

    /* sval = src & ((1<<N)-1) */
    if (value_is_const(src)) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (unsigned) (value_const_get(src) & ((1 << w) - 1)));
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 0);
    } else {
      value_t maskv;

      pic_op(pf, OPERATOR_ASSIGN, sval, src, VALUE_NONE);
      maskv = pfile_constant_get(pf, (1 << w) - 1, VARIABLE_DEF_NONE);
      if (maskv) {
        pic_op(pf, OPERATOR_ANDB, sval, sval, maskv);
        value_release(maskv);
      }
    }

    /* (m1:m0) = ((1<<N)-1) << bit_index ; (v1:v0) = sval << bit_index.
     * The interleaved rlf chain is carry-safe: both high bytes stay
     * below bit 7 for N <= 8 and bit_index < 8, so C is 0 between the
     * pairs and each pair zero-fills. */
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (1 << w) - 1);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, m0, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, m1, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, sval, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, v0, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, v1, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, cnt, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
    pic_instr_append_label(pf, lbl_loop);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, m0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, m1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, v0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, v1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, cnt, 0, PIC_OPDST_F);
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_loop);
    pic_instr_append_label(pf, lbl_done);

    /* low byte: *byte = (*byte & ~m0) | v0 */
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_COMF, m0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, m0, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, v0, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref.byte, 0);

    /* high byte only when the field straddles (bit_index + N > 8):
     * SUBLW (8-N) leaves C=1 when bit_index <= 8-N (no straddle) */
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW, 8 - w);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_nohi);
    pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 0,
        PIC_OPDST_F);
    if (value_byte_sz_get(ref.byte_index) > 1) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 1,
          PIC_OPDST_F);
    }
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_COMF, m1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, m1, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, v1, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref.byte, 0);
    pic_instr_append_label(pf, lbl_nohi);

    label_release(lbl_nohi);
    label_release(lbl_loop);
    label_release(lbl_done);
  }
  if (cnt) {
    pic_var_temp_release(pf, cnt);
  }
  if (v1) {
    pic_var_temp_release(pf, v1);
  }
  if (v0) {
    pic_var_temp_release(pf, v0);
  }
  if (m1) {
    pic_var_temp_release(pf, m1);
  }
  if (m0) {
    pic_var_temp_release(pf, m0);
  }
  if (sval) {
    pic_var_temp_release(pf, sval);
  }
  pic_dynamic_bit_ref_release(pf, &ref);
  return ok;
}

boolean_t pic_dynamic_bit_load_to_w(pfile_t *pf, value_t src)
{
  pic_dynamic_bit_ref_t ref;

  if ((value_sz_get(src) > 1)) {
    return pic_dynamic_field_load_to_w(pf, src);
  }
  if (!pic_dynamic_bit_ref_prepare(pf, src, &ref)) {
    return BOOLEAN_FALSE;
  }
  pic_dynamic_bit_ref_addr_setup(pf, &ref);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
  pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, ref.mask, 0, PIC_OPDST_W);
  pic_dynamic_bit_ref_release(pf, &ref);
  return BOOLEAN_TRUE;
}

static boolean_t pic_assign_to_dynamic_bit(pfile_t *pf, value_t dst,
    value_t src)
{
  pic_dynamic_bit_ref_t ref;
  boolean_t             rc;

  if (value_sz_get(dst) > 8) {
    pfile_log(pf, PFILE_LOG_ERR,
        "runtime-indexed bit*%u array elements are not supported yet "
        "(width above 8 bits); use a constant subscript or a byte-array "
        "representation", (unsigned) value_sz_get(dst));
    return BOOLEAN_TRUE;
  }
  if (value_sz_get(dst) > 1) {
    return pic_assign_to_dynamic_field(pf, dst, src);
  }
  rc = pic_dynamic_bit_ref_prepare(pf, dst, &ref);
  if (rc) {
    if (value_is_const(src)) {
      if (value_const_get(src)) {
        pic_dynamic_bit_ref_emit_set(pf, &ref);
      } else {
        pic_dynamic_bit_ref_emit_clear(pf, &ref);
      }
    } else {
      value_t src_tmp;
      label_t lbl_clear;
      label_t lbl_done;

      src_tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
      if (!src_tmp) {
        pfile_log_syserr(pf, ENOMEM);
      } else {
        pic_op(pf, OPERATOR_ASSIGN, src_tmp, src, VALUE_NONE);
        lbl_clear = pfile_label_alloc(pf, 0);
        lbl_done  = pfile_label_alloc(pf, 0);
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src_tmp, 0, PIC_OPDST_F);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clear);
        pic_dynamic_bit_ref_emit_set(pf, &ref);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_label(pf, lbl_clear);
        pic_dynamic_bit_ref_emit_clear(pf, &ref);
        pic_instr_append_label(pf, lbl_done);
        label_release(lbl_done);
        label_release(lbl_clear);
        pic_var_temp_release(pf, src_tmp);
      }
    }
    pic_dynamic_bit_ref_release(pf, &ref);
  }
  return rc;
}

/* dst is a bit, src is not lookup */
void pic_assign_to_bit(pfile_t *pf, value_t dst, value_t src)
{
  unsigned dst_bit_ofs;

  if (pic_value_is_dynamic_bit_array_element(dst)) {
    (void) pic_assign_to_dynamic_bit(pf, dst, src);
    return;
  }
  dst_bit_ofs  = value_bit_offset_get(dst);
  if (value_sz_get(dst) == 1) {
    if (value_is_const(src)) {
      variable_const_t cn;

      cn = value_const_get(src);
      pic_instr_append_f_bn(pf,
          ((value_is_boolean(dst) && cn)
          || (cn & 1)) ? PIC_OPCODE_BSF : PIC_OPCODE_BCF,
          dst, 0, 0);
    } else {
      if (value_is_bit(src) && (value_sz_get(src) == 1)) {
        /* single bit --> single bit */
        if (pic_value_is_dynamic_bit_array_element(src)) {
          pic_dynamic_bit_load_to_w(pf, src);
          if (value_is_volatile(dst) || !pic_val_bank_same(pf, dst, src)) {
            label_t lbl_set;
            label_t lbl_done;

            lbl_set  = pfile_label_alloc(pf, 0);
            lbl_done = pfile_label_alloc(pf, 0);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
            pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
            pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
            pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
            pic_instr_append_label(pf, lbl_set);
            pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
            pic_instr_append_label(pf, lbl_done);
            label_release(lbl_done);
            label_release(lbl_set);
          } else {
            pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
            pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
          }
        } else
        if (value_is_volatile(dst) || !pic_val_bank_same(pf, dst, src)) {
          label_t lbl_set;
          label_t lbl_done;

          lbl_set  = pfile_label_alloc(pf, 0);
          lbl_done = pfile_label_alloc(pf, 0);
          pic_instr_append_f(pf, PIC_OPCODE_BTFSC, src, 0);
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
          pic_instr_append_label(pf, lbl_set);
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
          pic_instr_append_label(pf, lbl_done);
          label_release(lbl_done);
          label_release(lbl_set);
        } else {
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
          pic_instr_append_f(pf, PIC_OPCODE_BTFSC, src, 0);
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        }
      } else {
        /* determine if src is 0, set src to _status:_z, and invert test */
        if (pic_value_is_w(src)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 
            (value_is_boolean(dst) ? 0xff : 0x01));
        } else {
          if (value_is_boolean(dst)) {
            pic_value_is_zero(pf, src);
          } else {
            if (value_is_indirect(src)) {
              variable_sz_t ipos;

              pic_indirect_setup3(pf, src, VALUE_NONE, VALUE_NONE, 0, &ipos);
            }
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, 0, PIC_OPDST_W);
            pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 1);
          }
        }
        /* set or clear dst as appropriate */
        if (value_is_volatile(dst) || !pic_val_bank_same(pf, dst, src)) {
          /* cannot change dst twice or cannot skip a single inst. :( */
          label_t done;
          label_t skip;

          skip = pfile_label_alloc(pf, 0);
          done = pfile_label_alloc(pf, 0);

          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, skip);
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);
          pic_instr_append_label(pf, skip);
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
          pic_instr_append_label(pf, done);
          label_release(done);
          label_release(skip);
        } else {
          /* simple single-bit transform */
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        }
      }
    }
  } else if (!value_is_const(src) && value_is_bit(src)) {
    value_t       tmp;
    variable_sz_t tmp_sz;

    tmp_sz = (value_sz_get(src) + 7) / 8;
    if (tmp_sz < 1) {
      tmp_sz = 1;
    }
    tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, tmp_sz);
    if (!tmp) {
      pfile_log_syserr(pf, ENOMEM);
    } else {
      pic_assign_from_bit(pf, tmp, src);
      pic_assign_to_bit(pf, dst, tmp);
      pic_var_temp_release(pf, tmp);
    }
  } else if (!pic_work_in_temp(pf, OPERATOR_ASSIGN, dst, src, VALUE_NONE,
    PIC_WORK_IN_TEMP_FLAG_VALS)) {
    if (!value_is_const(src) 
      && !value_is_volatile(dst)
      && (value_sz_get(dst) <= 4)
      && ((dst_bit_ofs & 7) >= 5)) {
      variable_sz_t dst_sz;
      variable_sz_t dst_ofs;
      variable_sz_t ii;
      value_t       tmp;

      tmp = src;
      if (pic_value_is_w(tmp) || !pic_val_bank_same(pf, src, dst)) {
        tmp = pic_var_accum_get(pf);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 0);
      }

      dst_sz  = value_sz_get(dst);
      dst_ofs = value_bit_offset_get(dst);
      dst = pic_bit_overlay_create(dst);
      for (ii = 0; ii < dst_sz; ii++) {
        pic_instr_append_f_bn(pf, PIC_OPCODE_BCF, dst,
            (dst_ofs + ii) / 8,
            (dst_ofs + ii) & 7);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, tmp, ii / 8, ii & 7);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst,
            (dst_ofs + ii) / 8,
            (dst_ofs + ii) & 7);
      }
      if (tmp != src) {
        pic_var_accum_release(pf, tmp);
      }
      value_release(dst);
    } else {
      /* optimize this a bit -- don't use the standard and/or
       * routines. instead do it directly (a combined and + or)
       */
      unsigned dst_byte_ofs;
      unsigned dst_byte_sz;  /* the number of bytes of interest */
      unsigned dst_bit_sz;
      unsigned mask;

      dst_byte_ofs = dst_bit_ofs / 8;
      dst_bit_ofs  = dst_bit_ofs & 7;
      dst_bit_sz   = value_sz_get(dst);
      dst_byte_sz  = (dst_bit_sz + dst_bit_ofs + 7)/ 8;

      /* we can ignore the first dst_byte_ofs bytes of dst as
       * they're not going to change */
      dst  = pic_bit_overlay_create(dst);
      mask = ~(((1 << dst_bit_sz) - 1) << dst_bit_ofs);

      /* tmp = src with bits masked off */
      if (value_is_const(src)) {
        unsigned sval;

        sval = (value_const_get(src) & ((1 << dst_bit_sz) - 1)) << dst_bit_ofs;

        while (dst_byte_sz) {
          if ((mask & 0xff) == 0x00) {
            /* there's nothing to mask off, simply copy this value in */
            if (sval & 0xff) {
              /* there is a value, copy it in */
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sval & 0xff);
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs);
            } else {
              /* no value, simply clear this */
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, dst_byte_ofs);
            }
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (unsigned char) (mask & 0xff));
            if (sval & 0xff) {
              /* sval is not 0, we'll need to AND and OR */
              pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, dst_byte_ofs, 
                PIC_OPDST_W);
              pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW,
                (unsigned char) (sval & 0xff));
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs);
            } else {
              /* sval is 0, we need only and */
              pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, dst_byte_ofs, 
                PIC_OPDST_F);
            }
          }
          dst_byte_ofs++;
          dst_byte_sz--;
          mask >>= 8;
          sval >>= 8;
        }
      } else {
        value_t tmp;
        value_t tmp2;
        size_t  ii;

        /* tmp = ( src << dst_bit_ofs ) & mask */
        tmp  = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, dst_byte_sz);
        if (value_sz_get(src) < value_sz_get(tmp)) {
          pic_op(pf, OPERATOR_ASSIGN, tmp, src, VALUE_NONE);
          src = tmp;
        }
        if (dst_bit_ofs) {
          tmp2 = pfile_constant_get(pf, dst_bit_ofs, VARIABLE_DEF_NONE);
          pic_shift_left(pf, OPERATOR_SHIFT_LEFT, tmp, src, tmp2,
            PIC_SHIFT_FLAG_NONE);
          value_release(tmp2);
          src = tmp;
        }
        /* if bit ofs is 4 and bit sz is 4 no further masking is necessary */
        /*if ((dst_bit_ofs != 4) || (dst_bit_sz != 4)) */{
          tmp2 = pfile_constant_get(pf, ~mask, VARIABLE_DEF_NONE);
          pic_op(pf, OPERATOR_ANDB, tmp, src, tmp2);
          value_release(tmp2);
        }
        for (ii = 0; ii < dst_byte_sz; ii++) {
          if ((mask & 0xff) == 0x00) {
            /* no mask, this is a straight copy */
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, ii, PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs + ii);
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, mask & 0xff);
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, dst_byte_ofs + ii,
              PIC_OPDST_W);
            pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, tmp, ii, PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs + ii);
          }
          mask >>= 8;
        }
        pic_var_temp_release(pf, tmp);
      }
      value_release(dst);
    }
  }
}

/* dst is not bit, src is non-const bit */
void pic_assign_from_bit(pfile_t *pf, value_t dst, value_t src)
{
  if (pic_value_is_dynamic_bit_array_element(src)) {
    if ((value_sz_get(src) > 1)
        && pic_dynamic_field_load_to_w(pf, src)) {
      /* N-bit field: the loader returns the FIELD VALUE in W (Z
       * reflects it) -- no 0/1 boolean normalization */
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
      }
    } else if (pic_dynamic_bit_load_to_w(pf, src)) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          value_is_signed(src) ? 0xff : 1);
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
      }
    }
  } else if ((value_sz_get(src) == 1)
      && (value_is_indirect(src) || (value_bit_offset_get(src) != 0))) {
    variable_sz_t ipos;

    if (value_is_indirect(src)) {
      /*
       * A bit read through a pointer (open `bit in bits[]` formal with a
       * packed actual) carries the bit subscript in baseofs. It is a BIT
       * index into the packed pointed-to bytes: the FSR byte offset is
       * baseofs / 8 and the tested bit is baseofs & 7 (plus any value bit
       * offset). The old code passed the subscript to setup as a plain
       * BYTE offset and emitted a byte-oriented BTFSC whose bit field then
       * leaked the INDF file address at encode time (PIC18 INDF0 0xFEF ->
       * bit 7); classic cores passed only via the _ind-at-address-0 bit-0
       * coincidence. fix_pic18_open_bit_formal_indirect.md
       */
      unsigned long bit_ix;
      value_t dbo;
      value_t src_use;

      /*
       * Contract: an open `bit in bits[]` formal reads buffer-relative bits
       * (the normalize buffer re-bases the actual's packed bits at 0). The
       * subscript rides in baseofs; any value bitofs here is the actual's
       * intra-byte offset leaking through the formal binding (classic
       * reserved bitbucket bit 0) and must be ignored.
       */
      dbo = value_baseofs_get(src);
      if (dbo && !value_is_const(dbo)) {
        /*
         * Runtime subscript: compute byte = i/8 and bit = i&7 at runtime
         * via the dynamic loader (same shift/mask emission the first
         * branch uses for array elements); the leaked bitofs is dropped
         * inside pic_dynamic_bit_ref_prepare for pointer formals.
         */
        /*
         * The array dynamic loader cannot be reused here: its master chain
         * resolves the pointer SLOT variable as a direct array base (wrong
         * address in fuller programs). Emit directly: byte = i >> 3 and
         * mask = 1 << (i & 7) as runtime temps, point the FSR at
         * slot + byte via a cloned baseofs, read INDF and AND the mask.
         */
        {
          value_t t_idx;
          value_t t_byt;
          value_t t_bit;
          value_t t_msk;
          value_t c3;
          value_t c7;
          value_t c1;
          value_t ind;
          boolean_t ok;

          t_idx = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          t_byt = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          t_bit = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          t_msk = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          c3 = pfile_constant_get(pf, 3, VARIABLE_DEF_NONE);
          c7 = pfile_constant_get(pf, 7, VARIABLE_DEF_NONE);
          c1 = pfile_constant_get(pf, 1, VARIABLE_DEF_NONE);
          ok = t_idx && t_byt && t_bit && t_msk && c3 && c7 && c1;
          if (ok) {
            src_use = value_clone(src);
            value_baseofs_set(src_use, t_byt);

            pic_op(pf, OPERATOR_ASSIGN, t_idx, dbo, VALUE_NONE);
            pic_op(pf, OPERATOR_SHIFT_RIGHT, t_byt, t_idx, c3);
            pic_op(pf, OPERATOR_ANDB, t_bit, t_idx, c7);
            pic_op(pf, OPERATOR_ASSIGN, t_msk, c1, VALUE_NONE);
            pic_op(pf, OPERATOR_SHIFT_LEFT, t_msk, t_msk, t_bit);

            pic_indirect_setup3(pf, src_use, VALUE_NONE, dst, 0, &ipos);
            ind = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, t_msk, 0, PIC_OPDST_W);
            value_release(ind);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
                value_is_signed(src) ? 0xff : 1);
            if (!pic_value_is_w(dst)) {
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
              pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
            }
            value_release(src_use);
          }
          value_release(c1);
          value_release(c7);
          value_release(c3);
          pic_var_temp_release(pf, t_msk);
          pic_var_temp_release(pf, t_bit);
          pic_var_temp_release(pf, t_byt);
          pic_var_temp_release(pf, t_idx);
          if (ok) {
            return;
          }
        }
        /* OOM: fall through to the original emission */
      }
      bit_ix = 0;
      if (getenv("JALV2_DBG_OBF")) {
        fprintf(stderr, "OBF pass=%u bitofs=%d baseofs=%d\n",
          (int) pic_code_gen_pass_get(pf),
          (int) value_bit_offset_get(src),
          (int) ((dbo && value_is_const(dbo))
            ? (long) (value_const_get(dbo) & 0xffff) : -1L));
      }
      if (dbo && value_is_const(dbo)) {
        bit_ix += (unsigned long) (value_const_get(dbo) & 0xffff);
        src_use = value_clone(src);
        value_baseofs_set(src_use, VALUE_NONE);
      } else {
        src_use = src;
      }
      /* the result is either all 0 or all 0xff */
      pic_indirect_setup3(pf, src_use, VALUE_NONE, dst,
        (variable_sz_t) (bit_ix / 8), &ipos);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src_use,
        (size_t) (bit_ix / 8), (variable_const_t) (bit_ix & 7));
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          value_is_signed(src) ? 0xff : 1);
      if (src_use != src) {
        value_release(src_use);
      }
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
      }
      return;
    }
    /* non-indirect: original emission (unchanged) */
    /* the result is either all 0 or all 0xff */
    pic_indirect_setup3(pf, src, VALUE_NONE, dst, 0, &ipos);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_BTFSC, src, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 
        value_is_signed(src) ? 0xff : 1);
    if (!pic_value_is_w(dst)) {
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
    }
  } else if (!value_is_volatile(src)
      && (value_sz_get(src) <= 8)
      && ((value_bit_offset_get(src) & 7) != 0)) {
    variable_sz_t ipos;
    variable_sz_t ii;
    variable_sz_t src_sz;
    variable_sz_t src_bit_ofs;

    src_sz      = value_sz_get(src);
    src_bit_ofs = value_bit_offset_get(src);
    src = pic_bit_overlay_create(src);
    pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
    if (value_is_volatile(dst)
        || pic_value_is_w(dst)
        || !pic_val_bank_same(pf, dst, src)) {
      /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      for (ii = 0; ii < src_sz; ii++) {
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src, 
            (src_bit_ofs + ii) / 8, 
            (src_bit_ofs + ii) & 7);
        pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW, 1 << ii);
      }
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      }
    } else {
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, 0);
      for (ii = 0; ii < src_sz; ii++) {
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src, 
            (src_bit_ofs + ii) / 8, 
            (src_bit_ofs + ii) & 7);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst, 0, ii);
      }
    }
    value_release(src);
    if (!pic_value_is_w(dst)) {
      pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
    }
  } else {
    /* dst = (src >> src_bit_ofs) & mask */
    value_t       tdst;
    variable_sz_t src_bit_sz;
    variable_sz_t src_bit_ofs;
    variable_sz_t src_byte_sz;
    variable_sz_t src_byte_ofs;
    unsigned      mask;
    value_t       tmp;

    src_bit_ofs  = value_bit_offset_get(src);
    src_bit_sz   = value_sz_get(src);
    src_byte_ofs = src_bit_ofs / 8;
    src_bit_ofs  = src_bit_ofs & 7;
    src_byte_sz  = (src_bit_sz + 7) / 8;

    src = pic_bit_overlay_create(src);

    if (src_bit_ofs) {
      tdst = (pic_value_is_w(dst) || value_is_volatile(dst)) 
        ? pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE,
            pic_value_is_w(dst) ? 1 : pic_result_sz_get(src, VALUE_NONE, dst))
        : dst;
    } else {
      tdst = src;
    }
    /* tdst = src >> src_bit_ofs */
    if (src_bit_ofs) {
      tmp = pfile_constant_get(pf, src_bit_ofs, VARIABLE_DEF_NONE);
      pic_shift_right(pf, OPERATOR_SHIFT_RIGHT, tdst, src, tmp,
        PIC_SHIFT_FLAG_MASK_BITS);
      value_release(tmp);
    }

    mask = ((1 << src_bit_sz) - 1);

    if (pic_value_is_w(dst)) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tdst,
        (tdst == src) ? src_byte_ofs : 0, PIC_OPDST_W);
      if ((mask & 0xff) != 0xff) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, mask & 0xff);
      }
    } else {
      variable_sz_t ii;
      variable_sz_t sz;

      if (src_byte_sz < value_sz_get(dst)) {
        sz = src_byte_sz;
      } else {
        sz = value_sz_get(dst);
      }

      for (ii = 0; ii < sz; ii++) {
        if ((mask & 0xff) == 0xff) {
          if (dst != tdst) {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tdst, 
              ii + ((tdst == src) ? src_byte_ofs : 0), PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
          }
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, mask & 0xff);
          if (dst == tdst) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, ii, PIC_OPDST_F);
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, tdst, 
              ii + ((tdst == src) ? src_byte_ofs : 0), PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
          } 
        }
      }
      while (ii < value_sz_get(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
        ii++;
      }
    }
    if ((src != tdst) && (dst != tdst)) {
      pic_var_temp_release(pf, tdst);
    }
    value_release(src);
  }
}

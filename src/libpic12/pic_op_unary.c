/************************************************************
 **
 ** pic_op_unary.c : PIC operator generation -- unary operators
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

void pic_op_neg(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(src));

  if (value_is_float(src)) {
    pic_var_float_t flt;

    pic_var_float_get(pf, &flt);
    if (!value_is_same(dst, src)) {
      if (value_is_float(dst)) {
        pic_op(pf, OPERATOR_ASSIGN, dst, src, VALUE_NONE);
        src = dst;
      } else {
        pic_op(pf, OPERATOR_ASSIGN, flt.fval1, src, VALUE_NONE);
        src = flt.fval1;
      }
    }
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, 3, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0x80);
    pic_instr_append_f_bn(pf, PIC_OPCODE_BCF, src, 3, 7);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, src, 3, 7);
    if (!value_is_same(dst, src)) {
      pic_op(pf, OPERATOR_ASSIGN, dst, src, VALUE_NONE);
    }
    pic_var_float_release(pf, &flt);
  } else if (!pic_work_in_temp(pf, op, dst, src, val2,
        PIC_WORK_IN_TEMP_FLAG_VAL1
        | PIC_WORK_IN_TEMP_FLAG_DST)) {
    variable_sz_t sz;

    sz = pic_result_sz_get(src, val2, dst);
    if ((1 == sz) 
      && !pic_is_12bit(pf)  
      && (value_is_volatile(dst) || !value_is_same(dst, src))) {
      variable_sz_t ipos;

      pic_indirect_setup3(pf, dst, src, val2, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_COMF, src, 0, PIC_OPDST_W);
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      
      pic_value_sign_extend(pf, dst, 0, 
          pic_result_is_signed(pf, src, val2));
    } else if (value_is_volatile(dst)) {
        value_t tmp;

        tmp = pic_var_temp_get(pf, pic_result_flag_get(pf, src, val2),
            pic_result_sz_get(src, val2, dst));
        pic_op(pf, OPERATOR_CMPB, tmp, src, val2);
        pic_op(pf, OPERATOR_INCR, dst, tmp, VALUE_NONE);
        pic_var_temp_release(pf, tmp);
    } else if (value_sz_get(dst) > sz) {
      value_t        tmp;
      variable_def_t def;

      tmp = value_clone(dst);
      def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
          value_dflag_test(src, VARIABLE_DEF_FLAG_SIGNED)
            ? VARIABLE_DEF_FLAG_SIGNED
            : VARIABLE_DEF_FLAG_NONE,
          sz);
      value_def_set(tmp, def);
      pic_op(pf, OPERATOR_CMPB, tmp, src, val2);
      pic_op(pf, OPERATOR_INCR, tmp, tmp, VALUE_NONE);
      pic_value_sign_extend(pf, dst, sz - 1, 
          pic_result_is_signed(pf, src, val2));
      value_release(tmp);
    } else {
      pic_op(pf, OPERATOR_CMPB, dst, src, val2);
      pic_op(pf, OPERATOR_INCR, dst, dst, VALUE_NONE);
    }
  }
}

void pic_op_incr(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(src));

  if (!pic_work_in_temp(pf, op, dst, src, val2,
    PIC_WORK_IN_TEMP_FLAG_VAL1
    | PIC_WORK_IN_TEMP_FLAG_DST)) {
    variable_sz_t sz;
    variable_sz_t ii;

    ii = 0;
    sz = pic_result_sz_get(src, val2, dst);
    if (1 == sz) {
      /* trivial case */
      pic_opdst_t   opdst;
      variable_sz_t ipos;

      opdst = (value_is_same(src, dst)) ? PIC_OPDST_F : PIC_OPDST_W;

      ii = 1; /* for sign extend */
      pic_indirect_setup3(pf, src, val2, dst, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, src, 0, opdst);
      if (PIC_OPDST_W == opdst) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      }
    } else if (value_is_same(src, dst) && !value_is_indirect(src)) {
      for (ii = 0; ii < sz; ii++) {
        if (ii) {
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        }
        pic_instr_append_f_d(pf, PIC_OPCODE_INCF, dst, ii, PIC_OPDST_F);
      }
    } else {
      variable_sz_t ipos;
      pic_opdst_t   opdst;
      value_t       src_sign;

      opdst = (value_is_same(dst, src)) ? PIC_OPDST_F : PIC_OPDST_W;
      pic_indirect_setup3(pf, dst, src, VALUE_NONE, 0, &ipos);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
      src_sign = VALUE_NONE;
      for (ii = 0; ii < sz; ii++) {
        pic_indirect_bump3(pf, dst, src, val2, ii, &ipos);
        if (ii) {
          if (ii == value_sz_get(src)) {
            src_sign = pic_value_sign_get(pf, src, VALUE_NONE);
            src      = src_sign;
            if (VALUE_NONE == src) {
              opdst = PIC_OPDST_W;
            }
          }
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
        }
        if (VALUE_NONE == src) {
          if (ii + 1 < sz) {
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
          }
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, src, 
            (src == src_sign) ? 0 : ii, opdst);
        }
        if (PIC_OPDST_W == opdst) {
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
      }
      if (VALUE_NONE != src_sign) {
        pic_var_accum_release(pf, src_sign);
      }
    }
    pic_value_sign_extend(pf, dst, ii - 1,
      pic_result_is_signed(pf, src, val2));
  }
}

void pic_op_decr(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(src));

  if (!pic_work_in_temp(pf, op, dst, src, val2,
    PIC_WORK_IN_TEMP_FLAG_VAL1
    | PIC_WORK_IN_TEMP_FLAG_DST)) {
    variable_sz_t sz;
    variable_sz_t ii;

    sz = pic_result_sz_get(src, val2, dst);
    if (1 == sz) {
      /* trivial case */
      pic_opdst_t   opdst;
      variable_sz_t ipos;

      opdst = (value_is_same(src, dst)) ? PIC_OPDST_F : PIC_OPDST_W;

      ii = 1;
      pic_indirect_setup3(pf, src, VALUE_NONE, dst, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_DECF, src, 0, opdst);
      if (PIC_OPDST_W == opdst) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      }
    } else {
      /* the work must be done in dst, 
       * so first copy src(n..1) --> dst(n..1) */
      variable_sz_t ipos;

      if (!value_is_same(dst, src)) {
        pic_indirect_setup3(pf, dst, src, VALUE_NONE, sz - 1, &ipos);
        for (ii = sz; ii > 1; ii--) {
          if (ii > value_sz_get(src)) {
            if (value_is_signed(src)) {
              if (ii == sz) {
                pic_value_sign_get_in_w(pf, src);
              }
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii - 1);
            } else {
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii - 1);
            }
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, ii - 1, 
              PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii - 1);
          }
          pic_indirect_bump3(pf, dst, src, VALUE_NONE, ii - 2, &ipos);
        }
        if (value_is_indirect(dst) || (sz > 2)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
          pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, src, 0, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_DECF, src, 0, PIC_OPDST_W);
        }
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        if (value_is_indirect(dst) || (sz > 2)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
        }
      } else {
        pic_indirect_setup3(pf, dst, src, VALUE_NONE, 0, &ipos);
        if (value_is_indirect(dst) || (sz > 2)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
          pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, dst, 0, PIC_OPDST_F);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_DECF, dst, 0, PIC_OPDST_F);
        }
      }
      if ((2 == sz) && !value_is_indirect(dst)) {
        pic_instr_append_f_d(pf, PIC_OPCODE_INCF, dst, 0, PIC_OPDST_W);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        pic_instr_append_f_d(pf, PIC_OPCODE_DECF, dst, 1, PIC_OPDST_F);
        ii = sz;
      } else {
        for (ii = 1; ii < sz; ii++) {
          pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
          pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, dst, ii, PIC_OPDST_F);
        }
      }
    }
    pic_value_sign_extend(pf, dst, ii - 1,
      pic_result_is_signed(pf, src, val2));
  }
}

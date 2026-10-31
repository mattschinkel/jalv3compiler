/************************************************************
 **
 ** pic_op_compare.c : PIC operator generation -- comparison and logic
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
static void pic_equality(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
static operator_t pic_relational_float(pfile_t *pf, operator_t op, 
  value_t dst, value_t val1, value_t val2);
static void pic_binary_and_or_xor(pfile_t *pf, operator_t op,
    value_t dst, value_t val1, value_t val2, pic_opcode_t pop);

/* much simpler than relational -- need only compare bytes;
 * nb : this is called for both OPERATOR_EQ & OPERATOR_NE
 * if dst is VALUE_NONE, the caller must check _Z and act
 * appropriately.
 */
static void pic_equality(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if (value_is_same(val1, val2)) {
    value_t          c;

    c = pfile_constant_get(pf, 
        (OPERATOR_EQ == op) ? 1 : 0, VARIABLE_DEF_NONE);
    pic_op(pf, OPERATOR_ASSIGN, dst, c, VALUE_NONE);
    value_release(c);
  } else if (value_is_float(val1) || value_is_float(val2)) {
    pic_relational_float(pf, op, dst, val1, val2);
  } else if (value_is_single_bit(val1) 
    && value_is_single_bit(val2)
    && !value_is_const(val1)
    && !value_is_const(val2)) {
    /* possible cases:
     *   dst == VALUE_NONE : need only set or clear status<z>
     *   dst is single bit
     *     volatile or different bank than val1/val2
     *   otherwise, work in W and assign
     */
    label_t      lbl_val1_clear;
    label_t      lbl_false;
    label_t      lbl_true;
    label_t      lbl_done;
    pic_opcode_t op_true;
    pic_opcode_t op_false;
    uchar        val_true;
    uchar        val_false;

    if (dst && (OPERATOR_NE == op)) {
      op_true   = PIC_OPCODE_BCF;
      op_false  = PIC_OPCODE_BSF;
      val_true  = 0;
      val_false = 1;
    } else {
      op_true   = PIC_OPCODE_BSF;
      op_false  = PIC_OPCODE_BCF;
      val_true  = 1;
      val_false = 0;
    }

    lbl_val1_clear = pfile_label_alloc(pf, 0);
    lbl_false      = pfile_label_alloc(pf, 0);
    lbl_true       = pfile_label_alloc(pf, 0);
    lbl_done       = pfile_label_alloc(pf, 0);

    /* for single bits, dst and val2 must be in the same bank */
    if (value_is_single_bit(dst)
      && !pic_val_bank_same(pf, dst, val2)
      && pic_val_bank_same(pf, dst, val1)) {
      value_t tmp;

      tmp = val1;
      val1 = val2;
      val2 = tmp;
    }

    if (value_is_single_bit(dst) 
      && (value_is_volatile(dst) 
        || !pic_val_bank_same(pf, dst, val2))) {
      /* this needs a bit different strategy */
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_val1_clear);
      /* val1 is TRUE */
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_false);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_true);
      /* val1 is FALSE */
      pic_instr_append_label(pf, lbl_val1_clear);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_true);
      /* the result is FALSE */
      pic_instr_append_label(pf, lbl_false);
      pic_instr_append_f(pf, op_false, dst, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      /* the result is TRUE */
      pic_instr_append_label(pf, lbl_true);
      pic_instr_append_f(pf, op_true, dst, 0);
    } else {
      /* initialize dst to TRUE */
      if (!dst) {
        pic_instr_append_reg_flag(pf, op_true, "_status", "_z");
      } else if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, op_true, dst, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, val_true);
      }
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_val1_clear);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_false);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_val1_clear);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
      pic_instr_append_label(pf, lbl_false);
      if (!dst) {
        pic_instr_append_reg_flag(pf, op_false, "_status", "_z");
      } else if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, op_false, dst, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, val_false);
      }
    }
    pic_instr_append_label(pf, lbl_done);
    if (dst && !value_is_single_bit(dst)) {
      pic_op(pf, OPERATOR_ASSIGN, dst, VALUE_NONE, VALUE_NONE);
    }
    label_release(lbl_done);
    label_release(lbl_true);
    label_release(lbl_false);
    label_release(lbl_val1_clear);
  } else if (!pic_work_in_temp(pf, op, dst, val1, val2,
        PIC_WORK_IN_TEMP_FLAG_VALS
        | (value_is_single_bit(dst)
          ? 0
          : PIC_WORK_IN_TEMP_FLAG_DST))) {
    value_t       tmp; /* for multi-byte operations */
    value_t       val1_sign;
    value_t       val2_sign;
    variable_sz_t sz;
    variable_sz_t ii;
    variable_sz_t ipos;
    variable_const_t cn;

    sz = pic_result_sz_get(val1, val2, VALUE_NONE);
    if (value_is_const(val1) && value_is_const(val2)) {
      value_t       cfold;
      variable_sz_t jj;
      boolean_t     eq_cf;

      eq_cf = BOOLEAN_TRUE;
      for (jj = 0; (jj < sz) && eq_cf; jj++) {
        if (pic_value_const_byte_get(val1, jj)
          != pic_value_const_byte_get(val2, jj)) {
          eq_cf = BOOLEAN_FALSE;
        }
      }
      cfold = pfile_constant_get(pf,
          (OPERATOR_EQ == op) ? (eq_cf ? 1 : 0) : (eq_cf ? 0 : 1),
          VARIABLE_DEF_NONE);
      pic_op(pf, OPERATOR_ASSIGN, dst, cfold, VALUE_NONE);
      value_release(cfold);
    } else {
    val1_sign = VALUE_NONE;
    val2_sign = VALUE_NONE;
    tmp       = VALUE_NONE;
    if (sz > 1) {
      tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
    }
    pic_indirect_setup3(pf, val1, val2, VALUE_NONE, 0, &ipos);
    /* if this is const v. variable, put the const on the left */
    if (value_is_const(val2)) {
      assert(!value_is_const(val1));
      SWAP(value_t, val1, val2);
    }
    cn = 0;
    if (value_is_const(val1)) {
      cn = value_const_get(val1);
    }
    for (ii = 0; ii < sz; ii++) {
      pic_opcode_t     pop;
      variable_const_t cn_pv;

      cn_pv = cn;
      if (!value_is_const(val1) && (ii == value_sz_get(val1))) {
        val1_sign = pic_value_sign_get(pf, val1, VALUE_NONE);
        val1      = val1_sign;
      } else if (!value_is_const(val2) && (ii == value_sz_get(val2))) {
        val2_sign = pic_value_sign_get(pf, val2, VALUE_NONE);
        val2      = val2_sign;
      }
      pic_indirect_bump3(pf, val1, val2, VALUE_NONE, ii, &ipos);

      pop = PIC_OPCODE_NONE;
      if (value_is_const(val1)) {
        uchar ch;

        ch = pic_value_const_byte_get(val1, ii);
        if (0 == ch) {
          pop = (0 == cn) ? PIC_OPCODE_IORWF : PIC_OPCODE_MOVF;
        } else if (1 == ch) {
          pop = PIC_OPCODE_DECF;
        } else if (255 == ch) {
          pop = PIC_OPCODE_INCF;
        } else {
          pop = PIC_OPCODE_SUBWF;
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, ch);
        }
        cn = cn >> 8;
      } else if (VALUE_NONE != val1) {
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val1, 
            (val1 == val1_sign) ? 0 : ii, PIC_OPDST_W);
        pop = PIC_OPCODE_SUBWF;
      }
      if (value_is_const(val2)) {
        assert(0);
        pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW,
            pic_value_const_byte_get(val2, ii));
      } else if (VALUE_NONE != val2) {
        pic_instr_append_f_d(pf, 
            (VALUE_NONE == val1) ? PIC_OPCODE_MOVF : pop, 
            val2, (val2 == val2_sign) ? 0 : ii, PIC_OPDST_W);
      }
      if (VALUE_NONE != tmp) {
        if (ii) {
          if (!value_is_const(val1) || (cn_pv != 0)) {
            pic_instr_append_f_d(pf,
                PIC_OPCODE_IORWF, tmp, 0, 
                ((ii + 1 == sz) || (value_is_const(val1) && (0 == cn)))
                ? PIC_OPDST_W : PIC_OPDST_F);
          }
        } else {
          if (!value_is_const(val1) || (cn != 0)) {
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 0);
          }
        }
      }
    }
    /* _status:_z is set for equal (w = 0), clear for not (w != 0) */
    if ((VALUE_NONE != dst) 
        && !value_is_single_bit(dst)) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
      if (OPERATOR_EQ == op) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_XORLW, 1);
      }
    }
    if (VALUE_NONE != val2_sign) {
      pic_var_accum_release(pf, val2_sign);
    }
    if (VALUE_NONE != val1_sign) {
      pic_var_accum_release(pf, val1_sign);
    }
    if (VALUE_NONE != tmp) {
      pic_var_temp_release(pf, tmp);
    }
    if (value_is_indirect(dst)) {
      /* must sock W away */
      tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 0);
      pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 0, PIC_OPDST_W);
      pic_var_temp_release(pf, tmp);
    }
    if (value_is_single_bit(dst)) {
      pic_opcode_t skip_op;

      skip_op = (OPERATOR_EQ == op) 
        ? PIC_OPCODE_BTFSC 
        : PIC_OPCODE_BTFSS;
      if (value_is_volatile(dst)) {
        label_t lbl_set;
        label_t lbl_done;

        lbl_set = pfile_label_alloc(pf, 0);
        lbl_done = pfile_label_alloc(pf, 0);
        pic_instr_append_reg_flag(pf, skip_op, "_status", "_z");
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
        pic_instr_append_reg_flag(pf, skip_op, "_status", "_z");
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      }
    } else if (!pic_value_is_w(dst)) {
      pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
    }
    }
  }
}

/* 
 * one of val1 or val2 must be float. the result is wholey defined by the 
 * flt.fval1 exponent + sign.
 *   sign(1)  : <
 *   exp == 0 : ==
 *   sign(0)  : >
 *
 * if dst is VALUE_NONE, 'C' and 'Z' are expected to be set as appropriate
 */
static operator_t pic_relational_float(pfile_t *pf, operator_t op, 
  value_t dst, value_t val1, value_t val2)
{
  pic_var_float_t flt;

  /*
   * put the const on the right
   */
  if (value_is_const(val1)) {
    SWAP(value_t, val1, val2);
    if (OPERATOR_GE == op) {
      op = OPERATOR_LE;
    } else if (OPERATOR_GT == op) {
      op = OPERATOR_LT;
    } else if (OPERATOR_LE == op) {
      op = OPERATOR_GE;
    } else if (OPERATOR_LT == op) {
      op = OPERATOR_GT;
    }
  }
  if (value_is_const(val2)
    && ((value_is_float(val2) && (0.0 == value_const_float_get(val2)))
      || (!value_is_float(val2) && (0 == value_const_get(val2))))) {
    /* this is (val1 op 0). Start by setting 'Z' */
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, val1, 2, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, val1, 3, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0xff);
    /* 'Z' is set if val2 is 0 */
    if (VALUE_NONE == dst) {
      if ((OPERATOR_EQ != op) && (OPERATOR_NE != op)) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_c");
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSS, val1, 3, 7);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
      }
    } else {
      label_t lbl;

      pic_var_float_get(pf, &flt);
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, flt.exp1, 0);
      switch (op) {
        case OPERATOR_EQ: /*       Z */
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          break;
        case OPERATOR_NE: /*      !Z */
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          break;
        case OPERATOR_LT: /* C && !Z */
          lbl = pfile_label_alloc(pf, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl);
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val1, 3, 7);
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          pic_instr_append_label(pf, lbl);
          label_release(lbl);
          break;
        case OPERATOR_LE: /* C ||  Z */
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val1, 3, 7);
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          break;
        case OPERATOR_GE: /* !C || Z */
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSS, val1, 3, 7);
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          break;
        case OPERATOR_GT: /* !C && !Z */
          lbl = pfile_label_alloc(pf, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl);
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSS, val1, 3, 7);
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          pic_instr_append_label(pf, lbl);
          label_release(lbl);
          break;
        default:
          break;
      }
      pic_op(pf, OPERATOR_ASSIGN, dst, flt.exp1, VALUE_NONE);
      pic_var_float_release(pf, &flt);
    }
  } else {
    /* only deal with < and <= */
    if ((OPERATOR_GE == op) || (OPERATOR_GT == op)) {
      SWAP(value_t, val1, val2);
      op = (OPERATOR_GE == op) 
         ? OPERATOR_LE
         : OPERATOR_LT;
    }
    pic_add_sub_float(pf, OPERATOR_SUB, VALUE_NONE, val1, val2);
    pic_var_float_get(pf, &flt);
    /* set flt.exp1 to 0 if the result is FALSE, else 1 */
    if (VALUE_NONE != dst) {
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, flt.exp1, 0);
    }
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, flt.fval1, 2, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, flt.fval1, 3, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0xff);
      /* Z is set if the exponent is 0 */
    if (VALUE_NONE == dst) {
      /* C doesn't matter in the simple equality test */
      if ((OPERATOR_EQ != op) && (OPERATOR_NE != op)) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, flt.fval1, 3, 7);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_c");
      }
    } else {
      label_t lbl;

      lbl = pfile_label_alloc(pf, 0);
      switch (op) {
        case OPERATOR_EQ:
        case OPERATOR_NE:
          pic_instr_append_reg_flag(pf,
            (OPERATOR_EQ == op) ? PIC_OPCODE_BTFSC : PIC_OPCODE_BTFSS,
            "_status", "_z");
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          break;
        case OPERATOR_LT: /* (sign bit set) AND (z == 0) */
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl);
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, flt.fval1, 3, 7);
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          pic_instr_append_label(pf, lbl);
          break;
        case OPERATOR_LE: /* (sign bit set) OR (z == 1) */
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, flt.fval1, 3, 7);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_z");
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, flt.exp1, 0, PIC_OPDST_F);
          break;
        default:
          break;
      }
      label_release(lbl);
      pic_op(pf, OPERATOR_ASSIGN, dst, flt.exp1, VALUE_NONE);
    }
    pic_var_float_release(pf, &flt);
  }
  return op;
}

/* this only handles <, <=, >=, >; the rest are handled by pic_equality
 * on exit, if dst is VALUE_NONE, _C and/or _Z must be tested by the caller 
 * also note the operator might change.
 */
operator_t pic_relational(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if ((OPERATOR_EQ == op) || (OPERATOR_NE == op)) {
    pic_equality(pf, op, dst, val1, val2);
  } else if (value_is_same(val1, val2)) {
    value_t          c;

    c = pfile_constant_get(pf,
          ((OPERATOR_LE == op) || (OPERATOR_GE == op)) ? 1 : 0,
          VARIABLE_DEF_NONE);
    pic_op(pf, OPERATOR_ASSIGN, dst, c, VALUE_NONE);
    value_release(c);
  } else if (value_is_float(val1) || value_is_float(val2)) {
    op = pic_relational_float(pf, op, dst, val1, val2);
  } else {
    /*
     * nb: this must be done before pic_work_in_temp or we lose
     *     the operator if it changes, resulting in B00063
     */
    if (value_is_const(val2)) {
      /* val1 *must* be the const, so swap these & fixup the op */
      SWAP(value_t, val1, val2);
      switch (op) {
        case OPERATOR_LT: op = OPERATOR_GT; break;
        case OPERATOR_LE: op = OPERATOR_GE; break;
        case OPERATOR_GE: op = OPERATOR_LE; break;
        case OPERATOR_GT: op = OPERATOR_LT; break;
        default: abort();
      }
    }
    if (!pic_work_in_temp(pf, op, dst, val1, val2,
        PIC_WORK_IN_TEMP_FLAG_VALS
        | (value_is_single_bit(dst)
          ? 0
          : PIC_WORK_IN_TEMP_FLAG_DST))) {
      /* unlike equality, relationals must work from the highest
       * part of the # to the lowest */
      variable_sz_t sz;
      variable_sz_t ipos;
      variable_sz_t ii;
      value_t       val1_sign;
      value_t       val2_sign;
      label_t       lbl_done;
      boolean_t     is_first;

      val1_sign = VALUE_NONE;
      val2_sign = VALUE_NONE;
      is_first  = BOOLEAN_TRUE;

      lbl_done = pfile_label_alloc(pf, 0);
      sz = pic_result_sz_get(val1, val2, VALUE_NONE);
      pic_indirect_setup3(pf, val1, val2, VALUE_NONE, sz - 1, &ipos);
      ii = sz;
      if (!value_is_const(val1) && (ii > value_sz_get(val1))) {
        val1_sign = pic_value_sign_get(pf, val1, VALUE_NONE);
        if (VALUE_NONE == val1_sign) {
          val1_sign = pic_var_accum_get(pf);
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, val1_sign, 0);
        }
      }
      if (!value_is_const(val2) && (ii > value_sz_get(val2))) {
        val2_sign = pic_value_sign_get(pf, val2, VALUE_NONE);
        if (VALUE_NONE == val2_sign) {
          val2_sign = pic_var_accum_get(pf);
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, val2_sign, 0);
        }
      }
      if (pic_result_is_signed(pf, val1, val2)) {
        value_t tmp;

        tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
        pic_value_to_w(pf, PIC_VALUE_TO_W_MOV, val2, val2_sign, ii - 1);
        pic_instr_append_w_kn(pf, PIC_OPCODE_XORLW, 0x80);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 0);
        if (value_is_const(val1)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
            pic_value_const_byte_get(val1, ii - 1) ^ 0x80);
        } else {
          pic_value_to_w(pf, PIC_VALUE_TO_W_MOV, val1, val1_sign, ii - 1);
          pic_instr_append_w_kn(pf, PIC_OPCODE_XORLW, 0x80);
        }
        pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, tmp, 0, PIC_OPDST_W);
        pic_var_temp_release(pf, tmp);
        is_first = BOOLEAN_FALSE;
        ii--;
      }
      while (ii--) {
        if (!is_first) {
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        }
        is_first = BOOLEAN_FALSE;
        pic_indirect_bump3(pf, val1, val2, VALUE_NONE, ii, &ipos);
        pic_value_to_w(pf, PIC_VALUE_TO_W_MOV, val1, val1_sign, ii);
        pic_value_to_w(pf, 
            (value_is_const(val2) ? PIC_VALUE_TO_W_ADD : PIC_VALUE_TO_W_SUB), 
             val2, val2_sign, ii);
      }
      pic_instr_append_label(pf, lbl_done);

      /* _c & !_z : val1 < val2  */
      /* _c & _z  : val1 <= val2 */
      /* !_c & _z : val1 >= val2 */
      /* !_c & !_z: val1 >  val2 */
      if (VALUE_NONE != dst) {
        label_t lbl_skip;

        lbl_skip = LABEL_NONE;
        if ((OPERATOR_LT == op) || (OPERATOR_GT == op)) {
          lbl_skip = pfile_label_alloc(pf, 0);
        }

        if (value_is_single_bit(dst)) {
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        }
        switch (op) {
          case OPERATOR_LT:
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
            pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_skip);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
            break;
          case OPERATOR_LE:
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
            break;
          case OPERATOR_GE:
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
            break;
          case OPERATOR_GT:
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
            pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_skip);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
            break;
          default:
            abort();
        }
        if (value_is_single_bit(dst)) {
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
        }
        if (LABEL_NONE != lbl_skip) {
          pic_instr_append_label(pf, lbl_skip);
        }
        label_release(lbl_skip);
      }
      if (VALUE_NONE != val2_sign) {
        pic_var_accum_release(pf, val2_sign);
      }
      if (VALUE_NONE != val1_sign) {
        pic_var_accum_release(pf, val1_sign);
      }
      label_release(lbl_done);
      if ((VALUE_NONE != dst) && !value_is_single_bit(dst)) {
        pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
      }
    }
  }
  return op;
}

void pic_op_relational(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  pic_relational(pf, op, dst, val1, val2);
}

/* if dst is VALUE_NONE, the caller is responsible for checking _Z
 * _Z is set if val1 && val2 == 0, clear if not
 */
void pic_op_logical_and(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  UNUSED(op);

  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  /* make sure dst/val1 are the same */
  if (value_is_same(dst, val2)) {
    SWAP(value_t, val1, val2);
  }

  if (value_is_same(val1, val2)) {
    pic_op(pf, OPERATOR_LOGICAL, dst, val1, VALUE_NONE);
  } else if (value_is_const(val1) || value_is_const(val2)) {
    if (value_is_const(val2)) {
      SWAP(value_t, val1, val2);
    }
    /* val1 is const */
    if (value_const_get(val1)) {
      pic_op(pf, OPERATOR_LOGICAL, dst, val2, VALUE_NONE);
    } else {
      pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_OP);
    }
  } else if (value_is_single_bit(val1) && value_is_single_bit(val2)) {
    if (pic_val_bank_same(pf, val1, val2) && !value_is_same(dst, val1)) {
      if (!value_is_single_bit(dst)
          || (value_is_single_bit(dst)
            && !value_is_volatile(dst)
            && pic_val_bank_same(pf, dst, val1))) {
        pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
        pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_OP);
      } else {
        label_t lbl_done;
        label_t lbl_clr;

        lbl_done = pfile_label_alloc(pf, 0);
        lbl_clr  = pfile_label_alloc(pf, 0);

        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_label(pf, lbl_clr);
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        pic_instr_append_label(pf, lbl_done);

        label_release(lbl_clr);
        label_release(lbl_done);
      }
    } else {
      label_t lbl_done;
      label_t lbl_clr;

      lbl_done = pfile_label_alloc(pf, 0);
      lbl_clr  = pfile_label_alloc(pf, 0);

      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
      pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_clr);
      pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      pic_instr_append_label(pf, lbl_done);
      if (!value_is_single_bit(dst)) {
        pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
      }
      label_release(lbl_clr);
      label_release(lbl_done);
    }
  } else if (value_is_single_bit(val1) || value_is_single_bit(val2)) {
    /* do some work */
    if (value_is_single_bit(val2)) {
      SWAP(value_t, val1, val2);
    }
    pic_value_is_zero(pf, val2);
    /* W is 0 or 1 */
    if (value_is_single_bit(dst)
        && (value_is_volatile(dst) 
          || !pic_val_bank_same(pf, dst, val1)
          || value_is_same(dst, val1)
          || value_is_same(dst, val2))) {
      label_t lbl_clr;
      label_t lbl_done;

      lbl_done = pfile_label_alloc(pf, 0);
      lbl_clr  = pfile_label_alloc(pf, 0);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      pic_instr_append_label(pf, lbl_done);
      label_release(lbl_clr);
      label_release(lbl_done);
    } else {
      pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      pic_instr_append_daop(pf, val1, PIC_OPCODE_BTFSS, BOOLEAN_FALSE, 0);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
      pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      if (!value_is_single_bit(dst)) {
        pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
      }
    }
  } else {
    label_t lbl_done;
    label_t lbl_clr;

    lbl_done = pfile_label_alloc(pf, 0);
    lbl_clr  = pfile_label_alloc(pf, 0);

    pic_value_is_zero(pf, val1);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
    pic_value_is_zero(pf, val2);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
    pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
    pic_instr_append_label(pf, lbl_clr);
    pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
    pic_instr_append_label(pf, lbl_done);
    if (!value_is_single_bit(dst)) {
      pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
    }
    label_release(lbl_clr);
    label_release(lbl_done);
  }
}

/* if dst is VALUE_NONE, _Z is set if A || B is true, clear if false */
void pic_op_logical_or(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  UNUSED(op);

  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if (value_is_same(val1, val2)) {
    pic_op(pf, OPERATOR_LOGICAL, dst, val1, VALUE_NONE);
  } else if (value_is_const(val1) || value_is_const(val2)) {
    if (value_is_const(val2)) {
      SWAP(value_t, val1, val2);
    }
    /* val1 is const */
    if (value_const_get(val1)) {
      pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_OP);
    } else {
      pic_op(pf, OPERATOR_LOGICAL, dst, val2, VALUE_NONE);
    }
  } else if (value_is_single_bit(val1) && value_is_single_bit(val2)) {
    if (pic_val_bank_same(pf, val1, val2)) {
      if (!value_is_single_bit(dst)
          || (value_is_single_bit(dst)
            && !value_is_volatile(dst)
            && !value_is_same(dst, val1)
            && !value_is_same(dst, val2)
            && pic_val_bank_same(pf, dst, val1))) {
        pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
        pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_OP);
      } else {
        label_t lbl_done;
        label_t lbl_set;

        lbl_done = pfile_label_alloc(pf, 0);
        lbl_set  = pfile_label_alloc(pf, 0);

        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_label(pf, lbl_set);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        pic_instr_append_label(pf, lbl_done);

        label_release(lbl_set);
        label_release(lbl_done);
      }
    } else {
      label_t lbl_done;
      label_t lbl_set;

      lbl_done = pfile_label_alloc(pf, 0);
      lbl_set  = pfile_label_alloc(pf, 0);

      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_set);
      pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      pic_instr_append_label(pf, lbl_done);
      if (!value_is_single_bit(dst)) {
        pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
      }
      label_release(lbl_set);
      label_release(lbl_done);
    }
  } else if (value_is_single_bit(val1) || value_is_single_bit(val2)) {
    /* do some work */
    if (value_is_single_bit(val2)) {
      SWAP(value_t, val1, val2);
    }
    pic_value_is_zero(pf, val2);
    /* W is 0 or 1 */
    if (value_is_single_bit(dst)
        && (value_is_volatile(dst) 
          || !pic_val_bank_same(pf, dst, val1)
          || value_is_same(dst, val1)
          || value_is_same(dst, val2))) {
      label_t lbl_set;
      label_t lbl_done;

      lbl_done = pfile_label_alloc(pf, 0);
      lbl_set  = pfile_label_alloc(pf, 0);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      pic_instr_append_label(pf, lbl_done);
      label_release(lbl_set);
      label_release(lbl_done);
    } else {
      pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      pic_instr_append_daop(pf, val1, PIC_OPCODE_BTFSS, BOOLEAN_FALSE, 0);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
      pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
      if (!value_is_single_bit(dst)) {
        pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
      }
    }
  } else {
    label_t lbl_done;
    label_t lbl_set;

    lbl_done = pfile_label_alloc(pf, 0);
    lbl_set  = pfile_label_alloc(pf, 0);

    pic_value_is_zero(pf, val1);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
    pic_value_is_zero(pf, val2);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
    pic_assign_logical_to_dst(pf, dst, 0, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
    pic_instr_append_label(pf, lbl_set);
    pic_assign_logical_to_dst(pf, dst, 1, PIC_ASSIGN_LOGICAL_TO_DST_NONE);
    pic_instr_append_label(pf, lbl_done);
    if (!value_is_single_bit(dst)) {
      pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
    }
    label_release(lbl_set);
    label_release(lbl_done);
  }
}

/* perform an AND, OR, or XOR */
static void pic_binary_and_or_xor(pfile_t *pf, operator_t op,
    value_t dst, value_t val1, value_t val2, pic_opcode_t pop)
{
  if (value_is_const(val1) || value_is_const(val2)) {
    variable_const_t cval;

    if (value_is_const(val1)) {
      SWAP(value_t, val1, val2);
    }
    cval = value_const_get(val2);
    if (!cval) {
      if (OPERATOR_ANDB == op) {
        /* anything AND 0 is 0, so set dst to 0 */
        /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        pic_op(pf, OPERATOR_ASSIGN, dst, VALUE_NONE, VALUE_NONE);
      } else {
        /* anything XOR or OR 0 is unchanged */
        pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
      }
    } else if ((cval == (variable_const_t) -1) && (OPERATOR_ANDB == op)) {
      /* anything AND all bits is unchanged */
      pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
    } else if (value_is_same(dst, val1)
      && (((OPERATOR_ANDB == op) && ((~cval & (~cval - 1)) == 0))
        || ((OPERATOR_ANDB != op) && ((cval & (cval - 1)) == 0)))) {
      /* single bit set, clear, or flip */
      unsigned bit;

      if (OPERATOR_ANDB == op) {
        cval = ~cval;
      }

      for (bit = -1; cval; cval >>= 1, bit++)
        ;
      if (value_is_bit(dst)) {
        if (bit < value_sz_get(dst)) {
          bit += value_bit_offset_get(dst);
        } else {
          dst = VALUE_NONE;
        }
      } else if (bit >= 8U * value_sz_get(dst)) {
        dst = VALUE_NONE;
      }
      if (VALUE_NONE != dst) {
        variable_sz_t ipos;

        pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE,
            bit / 8, &ipos);
        if (OPERATOR_ANDB == op) {
          pic_instr_append_f_bn(pf, PIC_OPCODE_BCF, dst, bit / 8, bit & 7);
        } else if (OPERATOR_ORB == op) {
          pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst, bit / 8, bit & 7);
        } else if (OPERATOR_XORB == op) {
          if (value_is_volatile(dst) || value_is_same(dst, val1)) {
            label_t lbl_done;
            label_t lbl_set;

            lbl_done = pfile_label_alloc(pf, 0);
            lbl_set  = pfile_label_alloc(pf, 0);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSS, val1, bit / 8, bit & 7);
            pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BCF, dst, bit / 8, bit & 7);
            pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
            pic_instr_append_label(pf, lbl_set);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst, bit / 8, bit & 7);
            pic_instr_append_label(pf, lbl_done);
            label_release(lbl_set);
            label_release(lbl_done);
          } else {
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, bit / 8, bit & 7);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BCF,   dst, bit / 8, bit & 7);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSS, dst, bit / 8, bit & 7);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BSF,   dst, bit / 8, bit & 7);
          }
        }
      }
    } else if (!pic_work_in_temp(pf, op, dst, val1, val2,
          PIC_WORK_IN_TEMP_FLAG_VALS_DST)) {
      /* dst = val1 op c */
      variable_sz_t sz;
      variable_sz_t ii;
      variable_sz_t ipos;
      value_t       sign;     /* for sign extension */
      variable_sz_t val1_ofs; /* index into val1    */
      uchar         ch_last;

      sign    = VALUE_NONE;
      ch_last = 0; /* prevent a compiler warning */

      sz = pic_result_sz_get(val1, val2, dst);
      pic_indirect_setup3(pf, dst, val1, VALUE_NONE, 0, &ipos);
      for (ii = 0, val1_ofs = 0; ii < sz; ii++) {
        uchar ch;

        if (ii) {
          pic_indirect_bump3(pf, dst, val1, val2, ii, &ipos);
          if (!sign && (++val1_ofs == value_sz_get(val1))) {
            sign     = pic_value_sign_get(pf, val1, VALUE_NONE);
            val1     = sign;
            val1_ofs = 0;
          }
          cval >>= 8;
        }
        ch = cval & 0xff;
        /* 0 && x --> 0; just clear dst */
        if (((0 == ch) || (VALUE_NONE == val1)) && (OPERATOR_ANDB == op)) {
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
        } else {
          pic_opcode_t opcode;

          if (((0xff == ch) && (OPERATOR_ANDB == op))
            || (((0x00 == ch) || (VALUE_NONE == val1))
              && ((OPERATOR_ORB == op) || (OPERATOR_XORB == op)))) {
            /* no action: x & 0xff == x */
            /*            x | 0x00 == x */
            /*            x ^ 0x00 == x */
            opcode = PIC_OPCODE_MOVF;
            if ((VALUE_NONE == val1) && ch) {
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, ch);
            }
          } else {
            /* an in-place mask changing a single bit of this byte is one
             * BCF/BSF (e.g. lookup pointers mask their top byte with
             * 0x7f); same math as the whole-value single-bit fold above,
             * applied per byte */
            boolean_t folded;

            folded = BOOLEAN_FALSE;
            if (((OPERATOR_ANDB == op) || (OPERATOR_ORB == op))
                && value_is_same(dst, val1)) {
              unsigned bmask;
              unsigned bbit;

              bmask = (OPERATOR_ANDB == op)
                ? ((unsigned)(~ch) & 0xffU) : (unsigned) ch;
              for (bbit = 0; bbit < 8; bbit++) {
                if (bmask == (1U << bbit)) {
                  break;
                }
              }
              if (bbit < 8) {
                pic_instr_append_f_bn(pf,
                  (OPERATOR_ANDB == op) ? PIC_OPCODE_BCF : PIC_OPCODE_BSF,
                  val1, val1_ofs, bbit);
                folded = BOOLEAN_TRUE;
              }
            }
            if (folded) {
              opcode = PIC_OPCODE_MOVF;
            } else {
              /* if val1 & dst are different, W was destroyed */
              if (!value_is_same(dst, val1) || (ch != ch_last) || (0 == ii)) {
                pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, ch);
                ch_last = ch;
              }
              opcode = pop;
            }
          }
          if (value_is_same(dst, val1)) {
            if (opcode != PIC_OPCODE_MOVF) {
              pic_instr_append_f_d(pf, opcode, val1, val1_ofs, PIC_OPDST_F);
            }
          } else {
            if (val1 != VALUE_NONE) {
              pic_instr_append_f_d(pf, opcode, val1, val1_ofs, PIC_OPDST_W);
            }
            if ((VALUE_NONE == val1) && !ch) {
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
            } else {
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
            }
          }
        }
      }
      pic_value_sign_extend(pf, dst, ii - 1,
          pic_result_is_signed(pf, val1, val2));
      if (sign) {
        pic_var_accum_release(pf, sign);
      }
    }
  } else if (!pic_work_in_temp(pf, op, dst, val1, val2,
        PIC_WORK_IN_TEMP_FLAG_VALS_DST)) {
    value_t       val1_sign;
    value_t       val2_sign;
    variable_sz_t sz;
    variable_sz_t ii;
    variable_sz_t ipos;
    variable_sz_t val1_ofs;
    variable_sz_t val2_ofs;
    pic_opdst_t   opdst;

    /* if dst is the same as an argument, make sure that argument is first */
    if (value_is_same(dst, val2)) {
      SWAP(value_t, val1, val2);
    }

    opdst = (value_is_same(dst, val1))
      ? PIC_OPDST_F
      : PIC_OPDST_W;

    val1_sign = VALUE_NONE;
    val2_sign = VALUE_NONE;
    sz = pic_result_sz_get(val1, val2, dst);
    pic_indirect_setup3(pf, val1, val2, dst, 0, &ipos);
    for (ii = 0, val1_ofs = 0, val2_ofs = 0; ii < sz; ii++) {
      if (ii) {
        if ((VALUE_NONE == val1_sign) && (++val1_ofs == value_sz_get(val1))) {
          val1_sign = pic_value_sign_get(pf, val1, VALUE_NONE);
          val1      = val1_sign;
          val1_ofs  = 0;
        }
        if ((VALUE_NONE == val2_sign) && (++val2_ofs == value_sz_get(val2))) {
          val2_sign = pic_value_sign_get(pf, val2, VALUE_NONE);
          val2      = val2_sign;
          val2_ofs  = 0;
        }
        pic_indirect_bump3(pf, dst, val1, val2, ii, &ipos);
      }
      /* if we've run out of val1 or val2 and it's not sign extended
       * it will always be `and 0' which is effectively clear dst
       */
      if ((VALUE_NONE == val1) || (VALUE_NONE == val2)) {
        if (OPERATOR_ANDB == op) {
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
        } else if (PIC_OPDST_W == opdst) {
          if (VALUE_NONE == val1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2, val2_ofs, opdst);
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val1, val1_ofs, opdst);
          }
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
      } else {
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2, val2_ofs, PIC_OPDST_W);
        pic_instr_append_f_d(pf, pop, val1, val1_ofs, opdst);
        if (PIC_OPDST_W == opdst) {
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
      }
    }
    if (VALUE_NONE != val1_sign) {
      pic_var_accum_release(pf, val1_sign);
    }
    if (VALUE_NONE != val2_sign) {
      pic_var_accum_release(pf, val2_sign);
    }
    pic_value_sign_extend(pf, dst, ii - 1, 
        pic_result_is_signed(pf, val1, val2));
  }
}

void pic_op_binary_and(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if (value_is_same(val1, val2)) {
    pic_op(pf, OPERATOR_ASSIGN, dst, val1, VALUE_NONE);
  } else if (!value_is_const(val1)
      && !value_is_const(val2)
      && value_is_single_bit(val1) 
      && value_is_single_bit(val2)) {
    /* the quick case only works if:
     *   val1 & val2 are in the same bank
     *   dst is not bit
     *     or dst is not volatile & dst is in the same bank as val1
     */
    label_t lbl_clr;
    label_t lbl_done;
    uchar   true_value;

    true_value = 1;
    if (pic_result_is_signed(pf, val1, val2)) {
      true_value = 255;
    }

    if (value_is_same(dst, val2)) {
      SWAP(value_t, val1, val2);
    }

    lbl_done = pfile_label_alloc(pf, 0);
    lbl_clr  = pfile_label_alloc(pf, 0);
    if (pic_val_bank_same(pf, val1, val2)) {
      if (!value_is_same(dst, val1)
          && (!value_is_single_bit(dst)
            || (!value_is_volatile(dst) 
              && pic_val_bank_same(pf, dst, val1)
              && pic_val_bank_same(pf, dst, val2)))) {
        /* 4 instructions (+ assignment if dst is not a bit) */
        if (value_is_single_bit(dst)) {
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, true_value);
        }
        if (value_is_shared(val1)) {
          pic_instr_append_daop(pf, val2, PIC_OPCODE_NONE, BOOLEAN_FALSE, 0);
        }
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
        if (value_is_single_bit(dst)) {
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        } else {
          /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        }
      } else if (!value_is_same(dst, val1)
          && !value_is_volatile(dst) 
          && !pic_val_bank_same(pf, dst, val1)) {
        /* 5 instructions */
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        if (value_is_shared(val2)) {
          pic_instr_append_daop(pf, val2, PIC_OPCODE_NONE, BOOLEAN_FALSE, 0);
        }
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      } else if (value_is_same(dst, val1)) {
        /* dst = dst & val2, both are single bit; only relies
         * on val2! */
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      } else {
        /* 6 instructions */
        if (value_is_shared(val1)) {
          pic_instr_append_daop(pf, val2, PIC_OPCODE_NONE, BOOLEAN_FALSE, 0);
        }
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_label(pf, lbl_clr);
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      }
    } else if (!value_is_same(dst, val1)
        && (!value_is_single_bit(dst) || !value_is_volatile(dst))) {
      /* 6 instructions */
      if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      } else {
        /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      }
      if (value_is_shared(val1)) {
        pic_instr_append_daop(pf, val2, PIC_OPCODE_NONE, BOOLEAN_FALSE, 0);
      }
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, true_value);
      }
    } else {
      /* 7 instructions */
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
    }
    pic_instr_append_label(pf, lbl_done);
    if (!value_is_single_bit(dst)) {
      pic_assign_from_w(pf, dst, 255 == true_value);
    }
    label_release(lbl_clr);
    label_release(lbl_done);
  } else {
    pic_binary_and_or_xor(pf, op, dst, val1, val2, PIC_OPCODE_ANDWF);
  } 
}

void pic_op_binary_or(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if (value_is_same(val1, val2)) {
    pic_op(pf, OPERATOR_ASSIGN, dst, val1, VALUE_NONE);
  } else if (!value_is_const(val1)
    && !value_is_const(val2)
    && value_is_single_bit(val1) 
    && value_is_single_bit(val2)
    && !(value_is_signed(val1) ^ value_is_signed(val2))) {
    label_t lbl_done;
    label_t lbl_set;
    uchar   true_value;

    true_value = 1;

    if (value_is_signed(val1) || value_is_signed(val2)) {
      true_value = 255;
    }

    lbl_done = pfile_label_alloc(pf, 0);
    lbl_set  = pfile_label_alloc(pf, 0);

    /* make sure (dst, val1) are same if any */
    if (value_is_same(dst, val2)) {
      SWAP(value_t, val1, val2);
    }
    if (pic_val_bank_same(pf, val1, val2)) {
      if (!value_is_same(dst, val1)
        && (!value_is_single_bit(dst)
            || (!value_is_volatile(dst) 
              && pic_val_bank_same(pf, dst, val1)
              && pic_val_bank_same(pf, dst, val2)))) {
        /* 4 instructions (+ assignment if dst is not a bit) */
        if (value_is_single_bit(dst)) {
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        } else {
          /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        }
        if (value_is_shared(val1)) {
          pic_instr_append_daop(pf, val2, PIC_OPCODE_NONE, BOOLEAN_FALSE, 0);
        }
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
        if (value_is_single_bit(dst)) {
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, true_value);
        }
      } else if (!value_is_same(dst, val1)
        && !value_is_volatile(dst) 
        && !pic_val_bank_same(pf, dst, val1)) {
        /* 5 instructions */
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        if (value_is_shared(val1)) {
          pic_instr_append_daop(pf, val2, PIC_OPCODE_NONE, BOOLEAN_FALSE, 0);
        }
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      } else if (value_is_same(dst, val1)) {
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      } else {
        /* 6 instructions */
        if (value_is_shared(val1)) {
          pic_instr_append_daop(pf, val2, PIC_OPCODE_NONE, BOOLEAN_FALSE, 0);
        }
        pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val1, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_label(pf, lbl_set);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      }
    } else if (!value_is_same(dst, val1)
      && (!value_is_single_bit(dst) || !value_is_volatile(dst))) {
      /* 6 instructions */
      if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, true_value);
      }
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      } else {
        /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      }
    } else {
      /* 7 instructions */
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
    }
    pic_instr_append_label(pf, lbl_done);
    if (!value_is_single_bit(dst)) {
      pic_assign_from_w(pf, dst, 255 == true_value);
    }
    label_release(lbl_set);
    label_release(lbl_done);
  } else {
    pic_binary_and_or_xor(pf, op, dst, val1, val2, PIC_OPCODE_IORWF);
  }
}

void pic_op_binary_xor(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if (value_is_same(val1, val2)) {
    /* a ^ a = 0 */
    variable_sz_t ii;
    variable_sz_t ipos;

    pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
    for (ii = 0; ii < value_sz_get(dst); ii++) {
      pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
    }
  } else if (!value_is_const(val1)
      && !value_is_const(val2)
      && value_is_single_bit(val1) 
      && value_is_single_bit(val2)
      && !(value_is_signed(val1) ^ value_is_signed(val2))) {
    label_t lbl_done;
    label_t lbl_skip;
    label_t lbl_set;
    label_t lbl_clr;
    uchar   true_value; /* value for TRUE */

    true_value = 1;
    if (value_is_signed(val1) || value_is_signed(val2)) {
      true_value = 255;
    }

    lbl_done = pfile_label_alloc(pf, 0);
    lbl_skip = pfile_label_alloc(pf, 0);
    lbl_set  = pfile_label_alloc(pf, 0);
    lbl_clr  = pfile_label_alloc(pf, 0);

    if (value_is_same(dst, val2)) {
      SWAP(value_t, val1, val2);
    }

    if (!value_is_single_bit(dst)
        || (!value_is_volatile(dst)
          && !value_is_same(dst, val1)
          && pic_val_bank_same(pf, dst, val2))) {
      if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      } else {
        /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      }
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_skip);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
      if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, true_value);
      }
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_skip);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      if (value_is_single_bit(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, true_value);
      }
    } else {
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val1, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_skip);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
      pic_instr_append_label(pf, lbl_skip);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, val2, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_label(pf, lbl_clr);
      pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
    }
    pic_instr_append_label(pf, lbl_done);
    if (!value_is_single_bit(dst)) {
      pic_assign_from_w(pf, dst, 255 == true_value);
    }
    label_release(lbl_clr);
    label_release(lbl_set);
    label_release(lbl_skip);
    label_release(lbl_done);
  } else {
    pic_binary_and_or_xor(pf, op, dst, val1, val2, PIC_OPCODE_XORWF);
  }
}

void pic_op_cmp(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(src));

  if (value_is_single_bit(dst) && value_is_single_bit(src)) {
    if (value_is_boolean(dst) 
        && !value_is_signed(src)
        && !value_is_boolean(src)) {
      /* if src is 0, dst is clearly 1; if src is 1,
       * the inverse is -2 */
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
    } else if (value_is_volatile(src)
        || value_is_same(dst, src)
        || !pic_val_bank_same(pf, dst, src)) {
      label_t lbl_done;
      label_t lbl_set;

      lbl_done = pfile_label_alloc(pf, 0);
      lbl_set  = pfile_label_alloc(pf, 0);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, src, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
      pic_instr_append_label(pf, lbl_set);
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      pic_instr_append_label(pf, lbl_done);
      label_release(lbl_set);
      label_release(lbl_done);
    } else {
      pic_instr_append_f(pf, PIC_OPCODE_BTFSC, src, 0);
      pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      pic_instr_append_f(pf, PIC_OPCODE_BTFSS, src, 0);
      pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
    }
  } else if (!pic_work_in_temp(pf, op, dst, src, val2,
        PIC_WORK_IN_TEMP_FLAG_VAL1
        | PIC_WORK_IN_TEMP_FLAG_DST)) {
    variable_sz_t sz;
    variable_sz_t sz_hi;
    variable_sz_t ii;
    variable_sz_t ipos;
    pic_opdst_t   opdst;

    sz    = pic_result_sz_get(src, val2, dst);
    sz_hi = value_sz_get(src);
    if (sz < sz_hi) {
      sz_hi = sz;
    }
    pic_indirect_setup3(pf, dst, src, val2, 0, &ipos);
    opdst = (value_is_same(dst, src))
      ? PIC_OPDST_F
      : PIC_OPDST_W;
    for (ii = 0; ii < sz_hi; ii++) {
      pic_indirect_bump3(pf, dst, src, val2, ii, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_COMF, src, ii, opdst);
      if (PIC_OPDST_W == opdst) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
      }
    }
    if (ii < sz) {
      src = pic_value_sign_get(pf, src, VALUE_NONE);
      if (VALUE_NONE == src) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0xff);
      } else {
        pic_instr_append_f_d(pf, PIC_OPCODE_COMF, src, 0, PIC_OPDST_W);
      }
      while (ii < sz) {
        pic_indirect_bump3(pf, dst, src, val2, ii, &ipos);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        ii++;
      }
      if (VALUE_NONE != src) {
        pic_var_accum_release(pf, src);
      }
    }
    pic_value_sign_extend(pf, dst, ii - 1, 
        pic_result_is_signed(pf, src, val2));
  }
}

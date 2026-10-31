/************************************************************
 **
 ** pic_op_arith.c : PIC operator generation -- arithmetic
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
static void pic_add_sub_16bit(pfile_t* pf, operator_t op, value_t dst,
    value_t val1, value_t val2, variable_sz_t sz);
static void pic_add_sub_1byte(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, variable_sz_t sz);
static void pic_add_sub_2byte(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, variable_sz_t sz);
static void pic_add_sub_nbyte(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, variable_sz_t sz);
static value_t pic_shift_get(pfile_t *pf, value_t val, value_t val2);
static boolean_t pic_multiply_inline_16bit(pfile_t *pf, value_t dst, 
  value_t val1, value_t val2, pic_var_mul_t *mvars);
static boolean_t pic_multiply_inline(pfile_t *pf, value_t dst, value_t val1,
  value_t val2, pic_var_mul_t *mvars);
static boolean_t pic_float_adj_exp(pfile_t *pf, value_t dst, value_t val1,
  value_t val2, pic_opcode_t op);
static void pic_float_mul(pfile_t *pf, value_t dst, value_t val1, 
  value_t val2);
static void pic_float_div(pfile_t *pf, value_t dst, value_t val1,
  value_t val2);

/* operator on val1 & dst (assign, neg, etc.). if val1 is signed,
 * and its size is smaller than dst, and the result *is not*
 * signed, shrink dst to val1 to eliminate sign extension,
 * perform the operation, then zero out the upper bytes in dst
 */
void pic_unary_fixup(pfile_t *pf, operator_t op,
    value_t dst, value_t val1, value_t val2)
{
  if (value_is_signed(val1)
      && !pic_result_is_signed(pf, val1, val2)
      && (pic_result_sz_get(val1, val2, dst) < value_byte_sz_get(dst))) {
    variable_def_t def;
    variable_def_t def_fixed;

    def = value_def_get(dst);
    def_fixed = variable_def_alloc(0, 
        variable_def_type_get(def),
        variable_def_flags_get_all(def),
        pic_result_sz_get(val1, val2, dst));
    value_def_set(dst, def_fixed);
    pic_op(pf, op, dst, val1, val2);
    value_def_set(dst, def);
    pic_value_sign_extend(pf, dst, pic_result_sz_get(val1, val2, dst) - 1,
        BOOLEAN_FALSE);
  } else {
    pic_op(pf, op, dst, val1, val2);
  }
}

/*
 * ADD: a = a + b
 *   movf   b_n, w
 *   if (n >= 1)
 *     btfsc _c
 *     incfsz b_n, w
 *   fi  
 *   addwf  a_n, f
 * 
 * SUB: a = a - b
 *   movf   b_n, w
 *   if (n >= 1)
 *     btfss  _c
 *     incfsz b_n
 *   fi  
 *   subwf  a_n
 *
 * nb : in the case where size <= 2, a and b can be in different
 *      banks. in the case where size > 2, a and b *must* be in the
 *      same bank. For that case a special register is defined,
 *      pic_accum. This variable *must* exist in all banks either in 
 *      a shared space (preferable), or at the same offset in each bank.
 */

/* 
 * the 16 bit cores change the status<c> on incf/decf operations
 * so the 12 & 14 bit add/sub routines cannot be used. Use the new
 *    addwfc -- w + f + c --> dst
 *    subfwb -- w - f - !c --> dst
 *    subwfb -- f - w - !c --> dst
 * these routines are much simpler:
 *    movf val1,w
 *    addwf val2,w
 *    movwf dst
 *    repeat
 *      movf   val1+n, w
 *      addwfc val2+n, w
 *      movwf  dst
 */
static void pic_add_sub_16bit(pfile_t* pf, operator_t op, value_t dst,
    value_t val1, value_t val2, variable_sz_t sz)
{
    pic_opdst_t   pop_dst;
    pic_opcode_t  pop;      /* opcode for first byte      */
    pic_opcode_t  pop_cont; /* opcode for remaining bytes */
    variable_sz_t ii;
    value_t       val1_sign;
    value_t       val2_sign;
    boolean_t     release_val;
    boolean_t     result_is_signed;
    value_t       tmp;
    variable_sz_t ipos;

    /*
     * This is called by both 16 bit and 14 bit hybrid. The
     * former changes status<c> on increment decrement, so
     * cannot work with indirect variables. The later doesn't
     * have this restriction
     */
    if (pic_is_16bit(pf)) {
        assert(!value_is_indirect(dst));
        assert(!value_is_indirect(val1));
        assert(!value_is_indirect(val2));
    }

    result_is_signed = pic_result_is_signed(pf, val1, val2);
    /* nb: we always want the const to be in val1! */
    release_val = BOOLEAN_FALSE;
    if ((OPERATOR_SUB == op) && value_is_const(val2)) {
        /* var - const = (-const) + var */
        val2 = pfile_constant_get(pf, -value_const_get(val2), VARIABLE_DEF_NONE);
        release_val = BOOLEAN_TRUE;
        op = OPERATOR_ADD;
    }
    if (value_is_const(val2)
        || ((OPERATOR_ADD == op) && value_is_same(val1, dst))) {
        tmp = val1;
        val1 = val2;
        val2 = tmp;
    }
    assert(!value_is_const(val2));
    pop_dst = (value_is_same(val2, dst)) ? PIC_OPDST_F : PIC_OPDST_W;
    if (OPERATOR_ADD == op) {
        pop = PIC_OPCODE_ADDWF;
        pop_cont = PIC_OPCODE_ADDWFc;
    }
    else {
        pop = PIC_OPCODE_SUBFWB;
        pop_cont = PIC_OPCODE_SUBFWB;
    }
    val1_sign = val1;
    val2_sign = val2;
    if (OPERATOR_SUB == op) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_c");
    }
    pic_indirect_setup3(pf, dst, val1, val2, 0, &ipos);
    for (ii = 0; ii < sz; ii++) {
        pic_indirect_bump3(pf, dst, val1, val2, ii, &ipos);
        if (ii == value_sz_get(val1) && !value_is_const(val1)) {
            tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
            val1_sign = pic_value_sign_get(pf, val1, tmp);
            if (VALUE_NONE == val1_sign) {
                /* val1_sign = pic_var_accum_get(pf); */
                val1_sign = tmp;
                pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
                pic_instr_append_f(pf, PIC_OPCODE_MOVWF, val1_sign, 0);
            }
        }
        else if (ii == value_sz_get(val2)) {
            tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
            val2_sign = pic_value_sign_get(pf, val2, tmp);
            if (VALUE_NONE == val2_sign) {
                /*val2_sign = pic_var_accum_get(pf);*/
                val2_sign = tmp;
                pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
                pic_instr_append_f(pf, PIC_OPCODE_MOVWF, val2_sign, 0);
            }
        }
        if (value_is_const(val1)) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
                pic_value_const_byte_get(val1, ii));
        }
        else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val1_sign,
                (val1_sign == val1) ? ii : 0, PIC_OPDST_W);
        }
        pic_instr_append_f_d(pf, pop, val2_sign,
            (val2_sign == val2) ? ii : 0, pop_dst);
        if (PIC_OPDST_W == pop_dst) {
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
        pop = pop_cont;
    }
    if (val1_sign && (val1_sign != val1)) {
        /* pic_var_accum_release(pf, val1_sign); */
        pic_var_temp_release(pf, val1_sign);
    }
    if (val2_sign && (val2_sign != val2)) {
        /*pic_var_accum_release(pf, val2_sign);*/
        pic_var_temp_release(pf, val2_sign);
    }
    if (release_val) {
        value_release(val1);
    }
    pic_value_sign_extend(pf, dst, ii - 1, result_is_signed);
}

/* the simplest of all cases */
static void pic_add_sub_1byte(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, variable_sz_t sz)
{
  UNUSED(sz);

  if (value_is_const(val1)) {
    variable_sz_t ipos;

    pic_indirect_setup3(pf, dst, val1, val2, 0, &ipos);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2, 0, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW,
      value_const_get(val1));
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
  } else {
    pic_opcode_t   pop;
    pic_opdst_t    opdst;
    variable_sz_t  ipos;
    pop   = (OPERATOR_ADD == op) ? PIC_OPCODE_ADDWF : PIC_OPCODE_SUBWF;
    opdst = (value_is_same(dst, val1)) ? PIC_OPDST_F : PIC_OPDST_W;

    pic_indirect_setup3(pf, dst, val1, val2, 0, &ipos);
    if (value_is_const(val2)) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 
        pic_value_const_byte_get(val2, 0));
    } else {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2, 0, PIC_OPDST_W);
    }
    pic_instr_append_f_d(pf, pop, val1, 0, opdst);
    if (PIC_OPDST_W == opdst) {
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
    }
  }
}

/* this one isn't so bad either */
static void pic_add_sub_2byte(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, variable_sz_t sz)
{
  pic_opcode_t  pop;
  pic_opcode_t  pop_post;
  pic_opcode_t  pop_skip;
  pic_opdst_t   opdst;
  variable_sz_t ipos;
  variable_sz_t ii;
  value_t       val1_sign;
  value_t       val2_sign;

  pic_indirect_setup3(pf, dst, val1, val2, sz - 1, &ipos);
  if (OPERATOR_ADD == op) {
    pop      = PIC_OPCODE_ADDWF;
    pop_post = PIC_OPCODE_INCF;
    pop_skip = PIC_OPCODE_BTFSC;
  } else {
    pop      = PIC_OPCODE_SUBWF;
    pop_post = PIC_OPCODE_DECF;
    pop_skip = PIC_OPCODE_BTFSS;
  }
  opdst = (value_is_same(dst, val1)) ? PIC_OPDST_F : PIC_OPDST_W;
  assert(!value_is_const(val1));

  val1_sign = VALUE_NONE;
  val2_sign = VALUE_NONE;
  if (!value_is_const(val1) && (value_sz_get(val1) == 1)) {
    val1_sign = pic_value_sign_get(pf, val1, VALUE_NONE);
    if ((VALUE_NONE == val1_sign) && (OPERATOR_SUB == op)) {
      val1_sign = pic_var_accum_get(pf);
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, val1_sign, 0);
    }
  } else if (!value_is_const(val2) && (value_sz_get(val2) == 1)) {
    val2_sign = pic_value_sign_get(pf, val2, VALUE_NONE);
  }

  for (ii = 2; ii > 0; ii--) {
    pic_indirect_bump3(pf, dst, val1, val2, ii - 1, &ipos);
    if ((value_is_const(val2) && (0 == pic_value_const_byte_get(val2, ii - 1)))
      || (!value_is_const(val2) 
        && (VALUE_NONE == val2_sign) 
        && (ii - 1 >= value_sz_get(val2)))) {
      if (PIC_OPDST_W == opdst) {
        if (ii - 1 >= value_sz_get(val1)) {
          if (VALUE_NONE == val1_sign) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val1_sign, 0, 
              PIC_OPDST_W);
          }
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val1, ii - 1, 
            PIC_OPDST_W);
        }
      }
      if (1 == ii) {
        /* an add 0 on the high byte clearly will have no effect,
         * so don't bother with the pop-skip/increment */
        op = OPERATOR_NULL;
      }
    } else {
      if (value_is_const(val2)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 
          pic_value_const_byte_get(val2, ii - 1));
      } else if (ii - 1 >= value_sz_get(val2)) {
        if (VALUE_NONE == val2_sign) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2_sign, 0, 
            PIC_OPDST_W);
        }
      } else {
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2, ii - 1, PIC_OPDST_W);
      }
      if (ii - 1 >= value_sz_get(val1)) {
        assert(PIC_OPDST_W == opdst);
        if (VALUE_NONE == val1_sign) {
          /* no need to do anything here */
        } else {
          pic_instr_append_f_d(pf, pop, val1_sign, 0, PIC_OPDST_W);
        }
      } else {
        pic_instr_append_f_d(pf, pop, val1, ii - 1, opdst);
      }
    }
    if (PIC_OPDST_W == opdst) {
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii - 1);
    }
  }
  if (value_is_indirect(dst)) {
    pic_indirect_bump3(pf, dst, val1, val2, 1, &ipos);
  }
  if (OPERATOR_NULL != op) {
    pic_instr_append_reg_flag(pf, pop_skip, "_status", "_c");
    pic_instr_append_f_d(pf, pop_post, dst, 1, PIC_OPDST_F);
  }
  if (VALUE_NONE != val1_sign) {
    pic_var_accum_release(pf, val1_sign);
  } else if (VALUE_NONE != val2_sign) {
    pic_var_accum_release(pf, val2_sign);
  }
}

static void pic_add_sub_nbyte(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, variable_sz_t sz)
{
  pic_opcode_t  pop;
  pic_opcode_t  pop_skip;
  pic_opcode_t  pop_post;
  variable_sz_t ipos;
  variable_sz_t ii;
  value_t       accum;
  value_t       use;
  value_t       tdst;

  tdst = VALUE_NONE;
  if (!value_is_same(dst, val1)) {
    if (value_is_same(dst, val2)) {
      tdst = dst;
      dst = pic_var_temp_get_assign(pf, val1, val2, dst);
    } else {
      pic_op(pf, OPERATOR_ASSIGN, dst, val1, VALUE_NONE);
    }
  }
  pic_indirect_setup3(pf, dst, VALUE_NONE, val2, 0, &ipos);
  if (OPERATOR_ADD == op) {
    pop      = PIC_OPCODE_ADDWF;
    pop_skip = PIC_OPCODE_INCFSZ;
    pop_post = PIC_OPCODE_BTFSC;
  } else {
    pop      = PIC_OPCODE_SUBWF;
    pop_skip = PIC_OPCODE_INCFSZ;
    pop_post = PIC_OPCODE_BTFSS;
  }
  assert(!value_is_const(val1));

  accum = pic_var_accum_get(pf);
  for (ii = 0; ii < sz; ii++) {
    pic_indirect_bump3(pf, dst, VALUE_NONE, val2, ii, &ipos);
    if (value_is_const(val2)) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
        pic_value_const_byte_get(val2, ii));
      if (ii) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
      }
      use = accum;
    } else if (ii >= value_sz_get(val2)) {
      if (value_is_signed(val2)) {
        if (ii == value_sz_get(val2)) {
          pic_value_sign_get(pf, val2, accum);
          if (!pic_val_movl_bank_equal(pf, accum, 0, dst, ii)) {
            pic_instr_append_daop(pf, dst, PIC_OPCODE_NONE, BOOLEAN_FALSE, ii);
          }
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, accum, 0, PIC_OPDST_W);
        }
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        if (ii) {
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
        }
      }
      use = accum;
    } else {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2, ii, PIC_OPDST_W);
      if (ii && !pic_val_bank_same(pf, dst, val2)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
        if (!pic_val_movl_bank_equal(pf, accum, 0, dst, ii)) {
          pic_instr_append_daop(pf, dst, PIC_OPCODE_NONE, BOOLEAN_FALSE, ii);
        }
        use = accum;
      } else {
        use = val2;
      }
    }
    if (ii) {
      pic_instr_append_reg_flag(pf, pop_post, "_status", "_c");
      pic_instr_append_f_d(pf, pop_skip, use,
        (use == val2) ? ii : 0, PIC_OPDST_W);
    }
    pic_instr_append_f_d(pf, pop, dst, ii, PIC_OPDST_F);
  }
  pic_var_accum_release(pf, accum);
  if (tdst) {
    pic_op(pf, OPERATOR_ASSIGN, tdst, dst, VALUE_NONE);
    pic_var_temp_release(pf, dst);
  }
}

void pic_add_sub_float(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  pic_var_float_t flt;
  label_t         lbl;
  const char     *which;

  pic_var_float_get(pf, &flt);
  pic_op(pf, OPERATOR_ASSIGN, flt.fval2, val2, VALUE_NONE);
  pic_op(pf, OPERATOR_ASSIGN, flt.fval1, val1, VALUE_NONE);
  which = (OPERATOR_ADD == op) ? PIC_LABEL_FLOAT_ADD : PIC_LABEL_FLOAT_SUB;
  lbl = pic_label_find(pf, which, BOOLEAN_TRUE);
  pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
  label_release(lbl);
  if ((VALUE_NONE != dst) && (flt.fval1 != dst)) {
    pic_op(pf, OPERATOR_ASSIGN, dst, flt.fval1, VALUE_NONE);
  }
}

void pic_op_add_sub(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  variable_sz_t    sz;
  variable_const_t cn;

  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  sz = pic_result_sz_get(val1, val2, dst);
  /* one constant for addition : put into val2
   * if (dst,val2) are the same swap
   */
  if ((OPERATOR_ADD == op) && 
      (value_is_const(val1) || value_is_same(dst, val2))) {
    SWAP(value_t, val1, val2);
  }

  cn = 0;
  if (value_is_const(val2)) {
    cn = value_const_get(val2);
  }
    
  if (value_is_float(val1) || value_is_float(val2) || value_is_float(dst)) {
    pic_add_sub_float(pf, op, dst, val1, val2);
  } else if (value_is_const(val2) && (0 == cn)) {
    /* if val1 is signed, but the result is unsigned, change val1 */
    pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
  } else if (value_is_const(val2) && (1 == cn)) {
    pic_unary_fixup(pf, (OPERATOR_ADD == op) ? OPERATOR_INCR : OPERATOR_DECR, 
        dst, val1, val2);
  } else if (value_is_const(val2) && ((variable_const_t) -1 == cn)) {
    pic_unary_fixup(pf, (OPERATOR_ADD == op) ? OPERATOR_DECR : OPERATOR_INCR, 
        dst, val1, val2);
  } else if (value_is_const(val1) && (0 == value_const_get(val1))) {
    pic_unary_fixup(pf, (OPERATOR_ADD == op) ? OPERATOR_ASSIGN : OPERATOR_NEG,
        dst, val2, val1);
  } else if ((OPERATOR_ADD == op) && value_is_same(val1, val2)) {
    value_t tmp;

    tmp = pfile_constant_get(pf, 1, value_def_get(val1));
    pic_op(pf, OPERATOR_SHIFT_LEFT, dst, val1, tmp);
    value_release(tmp);
  } else if (pic_work_in_temp(pf, op, dst, val1, val2,
        PIC_WORK_IN_TEMP_FLAG_VALS
        | PIC_WORK_IN_TEMP_FLAG_DST
        | (((sz > 1) && !pic_is_16bit(pf)) 
            ? PIC_WORK_IN_TEMP_FLAG_DST_VOLATILE : 0))) {
  } else if (pic_is_16bit(pf) 
      || ((OPERATOR_ADD == op) && pic_is_14bit_hybrid(pf))) {
    /*
     * the 14 bit hybrid controllers do not have
     * the subfwb operator (they do have subwfb however).
     * The 16 bit subtract uses subfwb, so it cannot
     * be used on the 14 bit hybrids
     */
    pic_add_sub_16bit(pf, op, dst, val1, val2, sz);
  } else {
    boolean_t result_is_signed;

    result_is_signed = pic_result_is_signed(pf, val1, val2);
    if ((OPERATOR_SUB == op) && value_is_const(val1)) {
      variable_def_t   vdef;
      value_t          tmp;
      variable_sz_t    ii;
      variable_sz_t    ipos;

      /* dst = ~val2 + (val1 + 1) */
      pic_indirect_setup3(pf, dst, val1, val2, 0, &ipos);
      for (ii = 0; ii < sz; ii++) {
        pic_indirect_bump3(pf, dst, val1, val2, ii, &ipos);
        if (value_is_same(dst, val2)) {
          pic_instr_append_f_d(pf, PIC_OPCODE_COMF, dst, ii, PIC_OPDST_F);
        } else {
          if (ii < value_sz_get(val2)) {
            pic_instr_append_f_d(pf, PIC_OPCODE_COMF, val2, ii, PIC_OPDST_W);
          } else if (ii == value_sz_get(val2)) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0xff);
            if (value_is_signed(val2)) {
              pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val2, 
                value_sz_get(val2) - 1, 7);
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
            }
          }
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
      }

      /* pic_op(pf, OPERATOR_CMPB, dst, val2, VALUE_NONE); */
      cn = 1 + value_const_get(val1);
      if (sz < 4) {
        cn &= (1UL << 8 * sz) - 1;
      }
      if (0 == cn) {
        op = OPERATOR_NULL;
      } else {
        vdef = value_def_get(val1);
        tmp = pfile_constant_get(pf, cn,
          variable_def_alloc(0, variable_def_type_get(vdef),
            VARIABLE_DEF_FLAG_CONST
            | (variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_SIGNED)
              ? VARIABLE_DEF_FLAG_SIGNED : VARIABLE_DEF_FLAG_NONE),
            sz));
        val1 = dst;
        val2 = tmp;
        op   = OPERATOR_ADD;
      }
    }
    if (OPERATOR_NULL != op) {
      if (value_is_const(val1)) {
        SWAP(value_t, val1, val2);
      }
      if (1 == sz) {
        pic_add_sub_1byte(pf, op, dst, val1, val2, sz);
      } else if (2 == sz) {
        pic_add_sub_2byte(pf, op, dst, val1, val2, sz);
      } else {
        pic_add_sub_nbyte(pf, op, dst, val1, val2, sz);
      }
    }
    pic_value_sign_extend(pf, dst, sz - 1, result_is_signed);
  }
}

/* 
 * return the shift value for a multiply or divide,
 * or 0 if the value is not const or not shiftable
 * (assumes shift of 0 is optimized out earlier)
 */
static value_t pic_shift_get(pfile_t *pf, value_t val, value_t val2)
{
  value_t shift_val;

  shift_val = VALUE_NONE;
  if (value_is_const(val)) {
    variable_const_t n;

    n = value_const_get(val);
    if (!(n & (n - 1))) {
      /* make this a shift */
      variable_const_t shift;

      for (shift = -1; n; shift++, n >>= 1)
        ;
      shift_val = pfile_constant_get(pf, shift, 
          pfile_variable_def_promotion_get(pf, OPERATOR_NULL, val, val2));
    }
  }
  return shift_val;
}

/*
 * 16 bit pics have a built in 8x8=16 multiplier, so use that
 * the constant is always on the right
 *   It appears 1 & 2 byte multiplies should be inlined
 * whereas anything larger should be a built-in
 */
static boolean_t pic_multiply_inline_16bit(pfile_t *pf, value_t dst, 
  value_t val1, value_t val2, pic_var_mul_t *mvars)
{
  value_t   prod;
  boolean_t was_inlined;

  prod = pfile_value_find(pf, PFILE_LOG_NONE, "prod");
  if (!prod) {
    was_inlined = BOOLEAN_FALSE;
  } else {
    variable_sz_t dst_sz;
    value_t       mresult;
    value_t       multiplicand;
    boolean_t     need_assign;

    was_inlined = BOOLEAN_TRUE;
    dst_sz = pic_result_sz_get(val1, val2, dst);
    if (value_is_indirect(dst)
      || value_is_volatile(dst) 
      || value_is_same(dst, val2)) {
      mresult = pic_cast(mvars->mresult, dst_sz);
      need_assign = BOOLEAN_TRUE;
    } else {
      mresult = pic_cast(dst, dst_sz);
      need_assign = BOOLEAN_FALSE;
    }
    if (value_is_indirect(val2)) {
      multiplicand = pic_cast(mvars->multiplicand, value_sz_get(val2));
      pic_op(pf, OPERATOR_ASSIGN, multiplicand, val2, VALUE_NONE);
    } else {
      multiplicand = val2;
      value_lock(val2);
    }
    if (value_is_indirect(val1)) {
      pic_indirect_setup(pf, val1, 0);
    }
    pic_multiply_create_fn(pf, LABEL_NONE, mresult, val1, multiplicand);
    if (need_assign) {
      pic_op(pf, OPERATOR_ASSIGN, dst, mresult, VALUE_NONE);
    } else {
      pic_value_sign_extend(pf, dst, dst_sz - 1,
        pic_result_is_signed(pf, val1, val2));
    }
    value_release(multiplicand);
    value_release(mresult);
    value_release(prod);
  }
  return was_inlined;
}

/*
 * if this particular multiplication is only used once, inline
 * it. On entry we know if there's a constant, it's in val2
 * return BOOLEAN_TRUE if this was inlined, BOOLEAN_FALSE otherwise
 */
static boolean_t pic_multiply_inline(pfile_t *pf, value_t dst, value_t val1,
  value_t val2, pic_var_mul_t *mvars)
{
  boolean_t     was_inlined;
  variable_sz_t w1;
  variable_sz_t w2;
  const pfile_multiply_width_table_entry_t *mw;
  size_t        ii;

  w1 = value_byte_sz_get(val1);
  w2 = value_is_universal(val2) ? w1 : value_byte_sz_get(val2);

  if (w1 > w2) {
    SWAP(variable_sz_t, w1, w2);
    SWAP(value_t, val1, val2);
  }
  was_inlined = BOOLEAN_FALSE;

  /* 16 bit, any multiply of 2x2 or less is always inlined */
  /* also inline a 3x1 as it is the same size (17 instructions) */
  if (pic_is_16bit(pf)
      && (((w1 <= 2) && (w2 <= 2))
          || ((w1 == 1) && (w2 == 3)))
      && pic_multiply_inline_16bit(pf, dst, val1, val2, mvars)) {
    was_inlined = BOOLEAN_TRUE;
  } else if (pfile_flag_test(pf, PFILE_FLAG_MISC_FASTMATH)) {
    for (ii = 0;
         (0 != (mw = pfile_multiply_width_table_entry_get(pf, ii)))
         && ((mw->multiplier != w1) || (mw->multiplicand != w2));
         ii++)
      ; /* null body */
    /* on everything, if a multiply is only used once, inline it */  
    if (mw && (1 == mw->use_ct)) {
      /* this can be inlined! */
      if (pic_is_16bit(pf)
          && pic_multiply_inline_16bit(pf, dst, val1, val2, mvars)) {
        was_inlined = BOOLEAN_TRUE;
      } else {
        value_t multiplier;
        value_t multiplicand;
        value_t mresult;

        multiplier = pic_cast(mvars->multiplier, w1);
        pic_op(pf, OPERATOR_ASSIGN, multiplier, val1, VALUE_NONE);
        if (value_is_indirect(val2)
          || value_is_volatile(val2) 
          || value_is_same(val2, dst)) {
          multiplicand = pic_cast(mvars->multiplicand, w2);
          pic_op(pf, OPERATOR_ASSIGN, multiplicand, val2, VALUE_NONE);
        } else {
          multiplicand = val2;
          value_lock(multiplicand);
        }
        if (value_is_volatile(dst)
          || value_is_indirect(dst)) {
          mresult = pic_cast(mvars->mresult, w2);
        } else {
          mresult = pic_cast(dst, w2);
        }
        pic_multiply_create_fn(pf, LABEL_NONE, mresult, multiplier, 
            multiplicand);
        if (value_variable_get(mresult) != value_variable_get(dst)) {
          pic_op(pf, OPERATOR_ASSIGN, dst, mresult, VALUE_NONE);
        } else {
          pic_value_sign_extend(pf, dst, value_byte_sz_get(mresult) - 1,
            pic_result_is_signed(pf, val1, val2));
        }
        value_release(mresult);
        value_release(multiplicand);
        value_release(multiplier);
      }
    }
  }
  return was_inlined;
}

/*
 * adjust a float by a power of two (add or subtract from the exponent)
 * return BOOLEAN_TRUE if exponent adjusted, otherwise BOOLEAN_FALSE
 */
static boolean_t pic_float_adj_exp(pfile_t *pf, value_t dst, value_t val1,
  value_t val2, pic_opcode_t op)
{
  boolean_t        ret;

  ret = BOOLEAN_FALSE;
  if (value_is_const(val2)) {
    variable_const_t shift; /* shift distance */
    boolean_t        neg;   /* negate result  */

    if (value_is_float(val2)) {
      float n;
      float mantissa;
      int   exp;

      n = value_const_float_get(val2);
      neg = n < 0.0;
      if (neg) {
        n = -n;
      }

      mantissa = frexp(value_const_float_get(val2), &exp);
      if ((mantissa - 0.5) < 0.0000001) {
        /* This is a power of 2. */
        ret = BOOLEAN_TRUE;
        exp--;
        if (exp < 0) {
          op = (PIC_OPCODE_ADDLW == op)
            ? PIC_OPCODE_SUBLW
            : PIC_OPCODE_ADDLW;
          exp = -exp;
        }
        shift = exp;
      }
    } else {
      variable_const_t n;

      n = value_const_get(val2);
      neg = (value_is_signed(val2) && (n & 0x80000000));
      if (neg) {
        n = -n;
      } 
      if ((n & (n - 1)) == 0) {

        ret = BOOLEAN_TRUE;
        for (shift = -1; n; shift++, n = n >> 1)
          ; /* null body */
      }
    }
    if (BOOLEAN_TRUE == ret) {
      pic_op(pf, (neg) ? OPERATOR_NEG : OPERATOR_ASSIGN, dst, val1,
        VALUE_NONE);
      if (shift) {
        value_t accum;
        label_t lbl_skip;
        label_t lbl_zero;

        accum = pic_var_accum_get(pf);
        lbl_skip = pfile_label_alloc(pf, 0);
        lbl_zero = pfile_label_alloc(pf, 0);
        pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, 2, PIC_OPDST_W);
        pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, 3, PIC_OPDST_W);
        /* If dst is 0, no action so skip. */
        pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0xff);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_skip);
        if (PIC_OPCODE_ADDLW == op) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, shift);
          /*
           * Check for overflow. If 'C' is set, we've overflow. Also
           * if the result is 0xff, that's also overflow.
           */
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_zero);
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
          pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW, 0xff);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_zero);
        } else {
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, shift);
          pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, accum, 0, PIC_OPDST_F);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_zero);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_zero);
        }
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0x80);
        pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, 3, PIC_OPDST_F);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
        pic_instr_append_f_d(pf, PIC_OPCODE_RRF, accum, 0, PIC_OPDST_W);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BCF, dst, 2, 7);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
        pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst, 2, 7);
        pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, dst, 3, PIC_OPDST_F);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_skip);
        pic_instr_append_label(pf, lbl_zero);
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, 0);
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, 1);
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, 2);
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, 3);
        pic_instr_append_label(pf, lbl_skip);
        label_release(lbl_skip);
        label_release(lbl_zero);
        pic_var_accum_release(pf, accum);
      }
    }
  }
  return ret;
}


static void pic_float_mul(pfile_t *pf, value_t dst, value_t val1, 
  value_t val2) 
{
  if (value_is_const(val1)) {
    SWAP(value_t, val1, val2);
  }
  if (!pic_float_adj_exp(pf, dst, val1, val2, PIC_OPCODE_ADDLW)) {
    pic_var_float_t flt;
    label_t         lbl;

    lbl = pic_label_find(pf, PIC_LABEL_FLOAT_MULTIPLY, BOOLEAN_TRUE);
    pic_var_float_get(pf, &flt);
    pic_op(pf, OPERATOR_ASSIGN, flt.fval2, val2, VALUE_NONE);
    pic_op(pf, OPERATOR_ASSIGN, flt.fval1, val1, VALUE_NONE);
    pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
    pic_op(pf, OPERATOR_ASSIGN, dst, flt.fval1, VALUE_NONE);
    pic_var_float_release(pf, &flt);
    label_release(lbl);
  }
}

void pic_op_mul(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  UNUSED(op);

  /* make sure the constant is on the right */
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if (value_is_const(val1)) {
    SWAP(value_t, val1, val2);
  }
  if (value_is_const(val2) && (0 == value_const_get(val2))) {
    pic_assign_const(pf, dst, val1, val2, 0);
  } else if (value_is_const(val2) && (1 == value_const_get(val2))) {
    pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
  } else if (value_is_const(val2) 
      && ((variable_const_t) -1 == value_const_get(val2))) {
    pic_unary_fixup(pf, OPERATOR_NEG, dst, val1, val2);
  } else if (value_is_float(val1) || value_is_float(val2)) {
    pic_float_mul(pf, dst, val1, val2);
  } else {
    value_t shift_val;

    shift_val = pic_shift_get(pf, val2, val1);
    if (shift_val) {
      pic_op(pf, OPERATOR_SHIFT_LEFT, dst, val1, shift_val);
      value_release(shift_val);
    } else {
      pic_var_mul_t  mvars;
      variable_def_t def;
      value_t        tmp;
      boolean_t      skip_assign;
      variable_sz_t  w1;
      variable_sz_t  w2;

      skip_assign = BOOLEAN_FALSE;
      w1 = value_byte_sz_get(val1);
      w2 = value_is_universal(val2) ? w1 : value_byte_sz_get(val2);

      /* multiplier is always the narrower of the two */
      if (w1 > w2) {
        SWAP(value_t, val1, val2);
        SWAP(variable_sz_t, w1, w2);
      }

      pic_var_mul_get(pf, w1, w2, &mvars);
      if ((value_is_same(val1, pic_last_values[PIC_LAST_VALUE_MULTIPLIER])
          && value_is_same(val2, pic_last_values[PIC_LAST_VALUE_MULTIPLICAND]))
          || (value_is_same(val1, pic_last_values[PIC_LAST_VALUE_MULTIPLICAND])
            && value_is_same(val2, pic_last_values[PIC_LAST_VALUE_MULTIPLIER]))
          ) {
        /* we needn't do anything here */
      } else {
        pic_last_values_reset();
        if (pic_multiply_inline(pf, dst, val1, val2, &mvars)) {
          skip_assign = BOOLEAN_TRUE;
        } else {
          label_t fn;
          char    label[32];

          if (pfile_flag_test(pf, PFILE_FLAG_MISC_FASTMATH)) {
            tmp = pic_cast(mvars.multiplier, w1);
            pic_op(pf, OPERATOR_ASSIGN, tmp, val1, VALUE_NONE);
            value_release(tmp);

            tmp = pic_cast(mvars.multiplicand, w2);
            pic_op(pf, OPERATOR_ASSIGN, tmp, val2, VALUE_NONE);
            value_release(tmp);

            sprintf(label, "%s_%u_%u", PIC_LABEL_MULTIPLY, w1, w2);
          } else {
            strcpy(label, PIC_LABEL_MULTIPLY);
            pic_op(pf, OPERATOR_ASSIGN, mvars.multiplier, val1, VALUE_NONE);
            pic_op(pf, OPERATOR_ASSIGN, mvars.multiplicand, val2, VALUE_NONE);
          }
          fn = pic_label_find(pf, label, BOOLEAN_TRUE);
          pic_instr_append_n(pf, PIC_OPCODE_CALL, fn);
          label_release(fn);
          pic_last_value_set(PIC_LAST_VALUE_MULTIPLIER,   val1);
          pic_last_value_set(PIC_LAST_VALUE_MULTIPLICAND, val2);
        }
      }
      if (!skip_assign) {
        tmp = value_clone(mvars.mresult);
        def = pfile_variable_def_promotion_get(pf, OPERATOR_NULL,
            val1, val2);
        value_def_set(tmp, def);
        pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
        value_release(tmp);
      }
      pic_var_mul_release(pf, &mvars);
    }
  }
}

static void pic_float_div(pfile_t *pf, value_t dst, value_t val1,
  value_t val2)
{
  if (!pic_float_adj_exp(pf, dst, val1, val2, PIC_OPCODE_SUBLW)) {
    label_t         lbl;
    pic_var_float_t flt;

    pic_var_float_get(pf, &flt);
    lbl = pic_label_find(pf, PIC_LABEL_FLOAT_DIVIDE, BOOLEAN_TRUE);
    pic_op(pf, OPERATOR_ASSIGN, flt.fval2, val2, VALUE_NONE);
    pic_op(pf, OPERATOR_ASSIGN, flt.fval1, val1, VALUE_NONE);
    pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
    pic_op(pf, OPERATOR_ASSIGN, dst, flt.fval1, VALUE_NONE);
    label_release(lbl);
    pic_var_float_release(pf, &flt);
  }
}

void pic_op_div_mod(pfile_t *pf, operator_t op, value_t dst, 
    value_t val1, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  if (value_is_const(val2) && (0 == value_const_get(val2))) {
    pfile_log(pf, PFILE_LOG_ERR, "division by zero");
  } else if (value_is_same(val1, val2)) {
    pic_assign_const(pf, dst, val1, val2, (OPERATOR_DIV == op) ? 1 : 0);
  } else if (value_is_const(val2) && (1 == value_const_get(val2))) {
    if (OPERATOR_DIV == op) {
      pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
    } else {
      pic_assign_const(pf, dst, val1, val2, 0);
    }
  } else if (value_is_const(val2) 
      && pic_result_is_signed(pf, val1, val2)
      && ((variable_const_t) -1 == value_const_get(val2))) {
    if (OPERATOR_DIV == op) {
      pic_unary_fixup(pf, OPERATOR_NEG, dst, val1, val2);
    } else {
      pic_assign_const(pf, dst, val1, val2,  0);
    }
  } else if (value_is_float(val1) || value_is_float(val2)) {
    pic_float_div(pf, dst, val1, val2);
  } else {
    value_t shift_val;

    shift_val = pic_shift_get(pf, val2, val1);
    /* cannot shift right if the result is signed, for example
     * -1 / 8 should be zero, but would shift into -1 */
    if (shift_val && !pic_result_is_signed(pf, val1, val2)) {
      if (OPERATOR_DIV == op) {
        /* right shift */
        pic_op(pf, OPERATOR_SHIFT_RIGHT, dst, val1, shift_val);
      } else if (OPERATOR_MOD == op) {
        /* binary and */
        value_t tmp;

        tmp = pfile_constant_get(pf, value_const_get(val2) - 1, 
            pfile_variable_def_promotion_get(pf, op, val1, val2));
        pic_op(pf, OPERATOR_ANDB, dst, val1, tmp);
        value_release(tmp);
      }
      value_release(shift_val);
    } else {
      variable_sz_t sz;
      pic_var_div_t dvars;
      value_t       result;

      sz = pic_result_sz_get(val1, val2, VALUE_NONE);
      pic_var_div_get(pf, sz, &dvars);
      if (value_is_same(pic_last_values[PIC_LAST_VALUE_DIVIDEND], val1)
          && value_is_same(pic_last_values[PIC_LAST_VALUE_DIVISOR], val2)) {
      } else {
        label_t fn;

        pic_op(pf, OPERATOR_ASSIGN, dvars.divisor,  val2, VALUE_NONE);
        pic_op(pf, OPERATOR_ASSIGN, dvars.dividend, val1, VALUE_NONE);
        fn = pic_label_find(pf,
            (VARIABLE_DEF_FLAG_SIGNED == pic_result_flag_get(pf, val1, val2))
            ? PIC_LABEL_SDIVIDE
            : PIC_LABEL_DIVIDE,
            BOOLEAN_TRUE);
        pic_instr_append_n(pf, PIC_OPCODE_CALL, fn);
        label_release(fn);
        pic_last_values_reset();
        pic_last_value_set(PIC_LAST_VALUE_DIVIDEND, val1);
        pic_last_value_set(PIC_LAST_VALUE_DIVISOR, val2);
      }
      result = (OPERATOR_DIV == op)
        ? dvars.quotient : dvars.remainder;
      if (pic_result_is_signed(pf, val1, val2)
          ^ value_is_signed(result)) {
        variable_def_t def;
        value_t        tmp;

        tmp = value_clone(result);
        def = pfile_variable_def_promotion_get(pf, OPERATOR_NULL,
            val1, val2);
        value_def_set(tmp, def);
        pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
        value_release(tmp);
      } else {
        pic_op(pf, OPERATOR_ASSIGN, dst, result, VALUE_NONE);
      }
      pic_var_div_release(pf, &dvars);
    }
  }
}

/* if dst is a single, non-volatile bit, clear it for 0, set for 1
 */
void pic_assign_logical_to_dst(pfile_t *pf, value_t dst, uchar n,
    unsigned flags)
{
  assert((0 == n) || (1 == n));
  if (value_is_single_bit(dst)) {
    pic_instr_append_f(pf,
        (n) ? PIC_OPCODE_BSF : PIC_OPCODE_BCF,
        dst, 0);
  } else {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, n);
    if (flags & PIC_ASSIGN_LOGICAL_TO_DST_OP) {
      /* assign the result to dst */
      pic_op(pf, OPERATOR_ASSIGN, dst, VALUE_NONE, VALUE_NONE);
    }
  }
}

void pic_value_to_w(pfile_t *pf, pic_value_to_w_t op,
    value_t val, value_t val_sign, variable_sz_t pos)
{
  if (value_is_const(val) 
      || (VALUE_NONE == val)
      || ((VALUE_NONE == val_sign) && (pos >= value_sz_get(val)))) {
    pic_opcode_t pop;

    pop = PIC_OPCODE_NONE;
    switch (op) {
      case PIC_VALUE_TO_W_NONE: break;
      case PIC_VALUE_TO_W_MOV:  pop = PIC_OPCODE_MOVLW; break;
      case PIC_VALUE_TO_W_SUB:  pop = PIC_OPCODE_SUBLW; break;
      case PIC_VALUE_TO_W_ADD:  pop = PIC_OPCODE_ADDLW; break;
    }
    if ((VALUE_NONE == val)/* || (pos >= value_sz_get(val))*/) {
      pic_instr_append_w_kn(pf, pop, 0);
    } else {
      pic_instr_append_w_kn(pf, pop, pic_value_const_byte_get(val, pos));
    }
  } else {
    pic_opcode_t pop;

    pop = PIC_OPCODE_NONE;
    switch (op) {
      case PIC_VALUE_TO_W_NONE: break;
      case PIC_VALUE_TO_W_MOV:  pop = PIC_OPCODE_MOVF;  break;
      case PIC_VALUE_TO_W_SUB:  pop = PIC_OPCODE_SUBWF; break;
      case PIC_VALUE_TO_W_ADD:  pop = PIC_OPCODE_ADDWF; break;
    }
    if (pos >= value_sz_get(val)) {
      pic_instr_append_f_d(pf, pop, val_sign, 0, PIC_OPDST_W);
    } else {
      pic_instr_append_f_d(pf, pop, val, pos, PIC_OPDST_W);
    }
  }
}

/* when this is called, W is either 0, 1, or 255
 * in the later case, if w is 255 we'll want to replicate it
 */
void pic_assign_from_w(pfile_t *pf, value_t dst, boolean_t sign_extend)
{
  if (!pic_value_is_w(dst)) {
    variable_sz_t ipos;
    variable_sz_t ii;

    ipos = 0;
    if (value_is_indirect(dst)) {
      value_t accum;

      accum = pic_var_accum_get(pf);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
      pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, accum, 0, PIC_OPDST_W);
      pic_var_accum_release(pf, accum);
    }
    for (ii = 0; ii < value_sz_get(dst); ii++) {
      pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
      if ((0 == ii) || sign_extend) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
      } else {
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
      }
    }
  }
}

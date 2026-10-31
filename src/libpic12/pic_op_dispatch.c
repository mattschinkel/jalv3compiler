/************************************************************
 **
 ** pic_op_dispatch.c : PIC operator generation -- operator dispatch table
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

/*
 * NAME
 *   pic_op
 *
 * DESCRIPTION
 *   create code for an operator
 *
 * PARAMETERS
 *   pf  : file
 *   op  :
 *   dst :
 *   val1:
 *   val2:
 *
 * RETURN
 *   none
 *
 * NOTES
 */
extern variable_sz_t pic_temp_sz_in_use;

typedef void (*operator_fn_t)(pfile_t *pf, operator_t op, value_t dst, 
    value_t val1, value_t val2);

static const struct operator_fn_tbl_ {
  operator_t    op;
  operator_fn_t fn;
  unsigned      flags;
} operator_fn_tbl[] = {
  { OPERATOR_NULL,        0,                     0},
  { OPERATOR_ADD,         pic_op_add_sub,        0},
  { OPERATOR_SUB,         pic_op_add_sub,        0},
  { OPERATOR_MUL,         pic_op_mul,            0},
  { OPERATOR_DIV,         pic_op_div_mod,        0},
  { OPERATOR_MOD,         pic_op_div_mod,        0},
  { OPERATOR_LT,          pic_op_relational,     0},
  { OPERATOR_LE,          pic_op_relational,     0},
  { OPERATOR_EQ,          pic_op_relational,     0},
  { OPERATOR_NE,          pic_op_relational,     0},
  { OPERATOR_GE,          pic_op_relational,     0},
  { OPERATOR_GT,          pic_op_relational,     0},
  { OPERATOR_ANDL,        pic_op_logical_and,    0},
  { OPERATOR_ORL,         pic_op_logical_or,     0},
  { OPERATOR_NOTL,        pic_op_logical_or_not, 0},
  { OPERATOR_ANDB,        pic_op_binary_and,     0},
  { OPERATOR_ORB,         pic_op_binary_or,      0},
  { OPERATOR_XORB,        pic_op_binary_xor,     0},
  { OPERATOR_CMPB,        pic_op_cmp,            0},
  { OPERATOR_ASSIGN,      pic_op_assign,         0},
  { OPERATOR_POINTER_ASSIGN, pic_op_assign,      0},
  { OPERATOR_NEG,         pic_op_neg,            0},
  { OPERATOR_INCR,        pic_op_incr,           0},
  { OPERATOR_DECR,        pic_op_decr,           0},
  { OPERATOR_SHIFT_LEFT,  pic_op_shift_left,     0},
  { OPERATOR_SHIFT_RIGHT, pic_op_shift_right,    0},
  { OPERATOR_SHIFT_RIGHT_ARITHMETIC, pic_op_shift_right,    0},
  { OPERATOR_LOGICAL,     pic_op_logical_or_not, 0}
};

void pic_op(pfile_t *pf, operator_t op, value_t dst, value_t val1,
    value_t val2)
{
  variable_sz_t    init;


  init = pic_temp_sz_in_use;
  if ((OPERATOR_ASSIGN != op) && (OPERATOR_POINTER_ASSIGN != op)
      && (value_is_label(val1) || value_is_label(val2))) {
    if (value_is_label(val1)) {
      value_t tmp;

      tmp = pic_var_temp_get_assign(pf, val1, VALUE_NONE, dst);
      pic_op(pf, op, dst, tmp, val2);
      pic_var_temp_release(pf, tmp);
    } else if (value_is_label(val2)) {
      value_t tmp;

      tmp = pic_var_temp_get_assign(pf, val2, VALUE_NONE, dst);
      pic_op(pf, op, dst, val1, tmp);
      pic_var_temp_release(pf, tmp);
    }
  } else {
    if (operator_is_binary(op)) {
      if (!val2) {
        val2 = val1;
        val1 = dst;
      }
    } else if (operator_is_unary(op)) {
      if (!val1) {
        val1 = dst;
      }
    }
    if ((OPERATOR_ASSIGN != op) && (OPERATOR_POINTER_ASSIGN != op)
        && value_is_const(val1)
        && (operator_is_unary(op) || value_is_const(val2))) {
      value_t    tmp;
      pf_const_t c;

      c = pfile_op_const_exec(pf, op, val1, val2);
      tmp = pf_const_to_const(pf, c, VARIABLE_DEF_NONE);
      pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
      value_release(tmp);
    } else {
      size_t           ii;
      pic_last_value_t which;

      for (ii = 0;
           (ii < COUNT(operator_fn_tbl))
           && (operator_fn_tbl[ii].op != op);
           ii++)
        ;
      if (ii == COUNT(operator_fn_tbl)) {
        assert(0);
      } else if (!operator_fn_tbl[ii].fn) {
        assert(0);
      } else {
        operator_fn_tbl[ii].fn(pf, op, dst, val1, val2);
      }
      /* if one of the last values has changed, invalid these */
      for (which = PIC_LAST_VALUE_FIRST;
           (which < PIC_LAST_VALUE_CT)
           && !value_is_same(pic_last_values[which], dst);
           which++)
        ;
      if (which < PIC_LAST_VALUE_CT) {
        pic_last_values_reset();
      }
    }
  }
  assert(init == pic_temp_sz_in_use);
}

/************************************************************
 **
 ** pf_expr.c : p-code expression & operator functions
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <assert.h>
#include <string.h>
#include "pfiled.h"
#include "pf_cmd.h"
#include "pf_msg.h"
#include "pf_op.h"
#include "pf_expr.h"

variable_def_t pfile_variable_def_promotion_get_default(pfile_t *pf,
    operator_t op, value_t val1, value_t val2)
{
  variable_def_t      rdef; /* result */
  variable_sz_t       sz1;
  variable_sz_t       sz2;
  variable_def_t      def1;
  variable_def_t      def2;

  UNUSED(pf);
  UNUSED(op);

  def1 = value_def_get(val1);
  def2 = value_def_get(val2);

  sz1 = variable_def_sz_get(def1);
  if (!value_is_bit(val1)) {
    sz1 *= 8;
  }
  sz2 = variable_def_sz_get(def2);
  if (!value_is_bit(val2)) {
    sz2 *= 8;
  }

  if (!def2 || variable_def_is_same(def1, def2)) {
    /* (1) definition is the same */
    rdef = def1;
  } else if ((OPERATOR_ASSIGN == op) && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def1))) {
    /* Handle pointer dereference assignment (pointer = value) */
    /* For pointer assignments, use the type of the value being assigned */
    rdef = def2;
  } else if (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(def1)) {
    rdef = def1;
  } else if (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(def2)) {
    rdef = def2;
  } else if (variable_def_flag_test(def1, VARIABLE_DEF_FLAG_SIGNED)
    && variable_def_flag_test(def2, VARIABLE_DEF_FLAG_SIGNED)) {
    /* (2) both operands are signed, the result is the one with the
     * greatest size */
    rdef = (sz1 > sz2) ? def1 : def2;
  } else if (!variable_def_flag_test(def1, VARIABLE_DEF_FLAG_SIGNED)
      && (sz1 >= sz2)) {
    /* (3a) unsigned type sz >= signed type size; result is unsigned */
    rdef = def1;
  } else if (!variable_def_flag_test(def2, VARIABLE_DEF_FLAG_SIGNED)
      && (sz2 >= sz1)) {
    /* (3b) unsigned type sz >= signed type size; result is unsigned */
    rdef = def2;
  } else if (variable_def_flag_test(def1, VARIABLE_DEF_FLAG_SIGNED)
      && (sz1 > sz2)) {
    /* (4a) */
    rdef = def1;
  } else if (variable_def_flag_test(def2, VARIABLE_DEF_FLAG_SIGNED)
      && (sz2 > sz1)) {
    /* (4b) */
    rdef = def2;
  } else {
    /* (5) */
    rdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_NONE, (sz1 > sz2) ? sz1 : sz2);
  }
  if (variable_def_flag_test(rdef, VARIABLE_DEF_FLAG_CONST)
      || variable_def_flag_test(rdef, VARIABLE_DEF_FLAG_BIT)) {
    /* flip this bit */
    rdef = variable_def_flags_change(rdef,
        variable_def_flags_get_all(rdef)
        & ~(VARIABLE_DEF_FLAG_CONST | VARIABLE_DEF_FLAG_BIT));
  }
  return rdef;
}

variable_def_t pfile_variable_def_promotion_get(pfile_t *pf,
    operator_t op, value_t val1, value_t val2)
{
  variable_def_t rdef;

  rdef = (pf->vectors->pf_expr_result_def_get_fn)
    ? pf->vectors->pf_expr_result_def_get_fn(pf, op, val1, val2)
    : pfile_variable_def_promotion_get_default(pf, op, val1, val2);
  if (!rdef) {
    pfile_log(pf, PFILE_LOG_ERR, "invalid operation (op=%d, val1_type=%d, val2_type=%d)", 
              (int)op, 
              val1 ? (int)value_type_get(val1) : -1, 
              val2 ? (int)value_type_get(val2) : -1);
  }
  return rdef;
}

/*
 * NAME
 *   pfile_expr_push
 *
 * DESCRIPTION
 *   push a subexpression onto the expression stack
 *
 * PARAMETERS
 *   pf   : returned by pfile_open()
 *   stk  : top of the expression stack
 *   expr : expression to append
 *
 * RETURN
 *   none
 *
 * NOTES
 *   this creates commands as necessary
 */
result_t pfile_expr_push(pfile_t *pf, expr_t **stk, value_t val,
    operator_t op)
{
  result_t rc;
  expr_t  *stkptr;
  expr_t  *expr;

  rc     =  expr_alloc(&expr, op, val);
  stkptr = *stk;
  if (RESULT_OK == rc) {
    while (stkptr 
      && (operator_priority_get(expr_operator_get(stkptr))
        >= operator_priority_get(expr_operator_get(expr)))) {
      expr_t  *stkpv;
      value_t  dst;

      stkpv = expr_link_get(stkptr);
      dst   = 0;

      if (operator_is_assign(expr_operator_get(stkptr))) {
        /* stkptr->val stkptr->op expr->val */
        dst = expr_val_get(stkptr);
        pfile_cmd_op_add(pf, expr_operator_get(stkptr),
          &dst, expr_val_get(expr), 0);
        value_lock(dst);
      } else {
        /* dst = stkptr->val stkptr->op expr->val */
        operator_t s_op;

        s_op = expr_operator_get(stkptr);
        if (!expr_val_get(stkptr) || !expr_val_get(expr)) {
          /* an operand never parsed (error already logged, e.g. an
           * invalid operation on a name-collided symbol): skip the op
           * instead of asserting on VALUE_NONE during recovery */
          expr_val_set(expr, VALUE_NONE);
          expr_free(stkptr);
          continue;
        }
        if ((OPERATOR_SHIFT_RIGHT == s_op)
          && value_is_signed(expr_val_get(stkptr))) {
          s_op = OPERATOR_SHIFT_RIGHT_ARITHMETIC;
        }
        pfile_cmd_op_add(pf, s_op, &dst, expr_val_get(stkptr), 
            expr_val_get(expr));
      }
      expr_val_set(expr, dst);
      expr_free(stkptr);
      value_release(dst);
      stkptr = stkpv;
    }
    expr_link_set(expr, stkptr);
    *stk = expr;
  }
  if (RESULT_OK != rc) {
    pfile_log_syserr(pf, rc);
  }
  return rc;
}

/* does the relation make sense? */
typedef enum relation_result_ {
  RELATION_RESULT_UNKNOWN, /* no problems */
  RELATION_RESULT_TRUE,    /* result is always TRUE */
  RELATION_RESULT_FALSE    /* result is always FALSE */
} relation_result_t;

static relation_result_t pfile_expr_relation_check(pfile_t *pf, operator_t op,
  value_t val1, value_t val2)
{
  variable_const_t  cn;
  size_t            val1_sz;
  const char       *wstr;
  relation_result_t result;
  boolean_t         both_const;

  result = RELATION_RESULT_UNKNOWN;
  both_const = value_is_const(val1) && value_is_const(val2);
  if (value_is_const(val1) || value_is_const(val2)) {
    /* first, force the constant to the right side */
    if (value_is_const(val1)) {
      value_t tmp;

      switch (op) {
        case OPERATOR_LT: op = OPERATOR_GT; break;
        case OPERATOR_LE: op = OPERATOR_GE; break;
        case OPERATOR_GE: op = OPERATOR_LE; break;
        case OPERATOR_GT: op = OPERATOR_LT; break;
        default:
          /*
           * The only way to get here is '=' or '!=' and these are
           * cummutative so it doesn't matter. No other operators
           * should have come this far!
           */
           break;
      }
      tmp = val1;
      val1 = val2;
      val2 = tmp;
    }
    cn = value_const_get(val2);
    val1_sz = value_sz_get(val1);
    if (!value_is_bit(val1)) {
      val1_sz *= 8;
    }
    if (value_is_signed(val1)) {
      variable_const_t cn_low;
      variable_const_t cn_high;
      boolean_t        below;
      boolean_t        above;

      cn_high = (val1_sz >= VARIABLE_CONST_BIT_CT)
        ? (variable_const_mask_get(VARIABLE_CONST_BIT_CT - 1))
        : (variable_const_mask_get((unsigned) val1_sz - 1));
      cn_low = variable_const_sign_extend(
        ((variable_const_t) 1) << ((val1_sz >= VARIABLE_CONST_BIT_CT)
        ? (VARIABLE_CONST_BIT_CT - 1) : ((unsigned) val1_sz - 1)),
        (unsigned) val1_sz);
      below = (variable_const_cmp(cn, cn_low, VARIABLE_CONST_BIT_CT,
          BOOLEAN_TRUE) < 0);
      above = (variable_const_cmp(cn, cn_high, VARIABLE_CONST_BIT_CT,
          BOOLEAN_TRUE) > 0);
      switch (op) {
        case OPERATOR_LT:
          if (below || (0 == variable_const_cmp(cn, cn_low,
              VARIABLE_CONST_BIT_CT, BOOLEAN_TRUE))) {
            result = RELATION_RESULT_FALSE;
          } else if (above) {
            result = RELATION_RESULT_TRUE;
          }
          break;
        case OPERATOR_LE:
          if (below) {
            result = RELATION_RESULT_FALSE;
          } else if (above || (0 == variable_const_cmp(cn, cn_high,
              VARIABLE_CONST_BIT_CT, BOOLEAN_TRUE))) {
            result = RELATION_RESULT_TRUE;
          }
          break;
        case OPERATOR_EQ:
          if (below || above) {
            result = RELATION_RESULT_FALSE;
          }
          break;
        case OPERATOR_NE:
          if (below || above) {
            result = RELATION_RESULT_TRUE;
          }
          break;
        case OPERATOR_GE:
          if (below || (0 == variable_const_cmp(cn, cn_low,
              VARIABLE_CONST_BIT_CT, BOOLEAN_TRUE))) {
            result = RELATION_RESULT_TRUE;
          } else if (above) {
            result = RELATION_RESULT_FALSE;
          }
          break;
        case OPERATOR_GT:
          if (below) {
            result = RELATION_RESULT_TRUE;
          } else if (above || (0 == variable_const_cmp(cn, cn_high,
              VARIABLE_CONST_BIT_CT, BOOLEAN_TRUE))) {
            result = RELATION_RESULT_FALSE;
          }
          break;
        default:
          break;
      }
    } else {
      if (value_is_signed(val2)
          && variable_const_is_negative(cn, VARIABLE_CONST_BIT_CT)) {
        switch (op) {
          case OPERATOR_LT: /* an unsigned value can never be <= a negative #*/
          case OPERATOR_LE:
          case OPERATOR_EQ: result = RELATION_RESULT_FALSE; break;
          case OPERATOR_NE:
          case OPERATOR_GE:
          case OPERATOR_GT: result = RELATION_RESULT_TRUE;  break;
          default: break;
        }
      } else {
        variable_const_t cn_high;

        /* the highest possible number that can fit into val1 */
        cn_high = variable_const_mask_get((unsigned) val1_sz);
        switch (op) {
          case OPERATOR_LT:
            if (cn == 0) {
              result = RELATION_RESULT_FALSE;
            } else if (cn > cn_high) {
              result = RELATION_RESULT_TRUE;
            }
            break;
          case OPERATOR_LE:
            if (cn >= cn_high) {
              result = RELATION_RESULT_TRUE;
            }
            break;
          case OPERATOR_EQ:
            if (cn > cn_high) {
              result = RELATION_RESULT_FALSE;
            }
            break;
          case OPERATOR_NE:
            if (cn > cn_high) {
              result = RELATION_RESULT_TRUE;
            }
            break;
          case OPERATOR_GE:
            if (cn == 0) {
              result = RELATION_RESULT_TRUE;
            } else if (cn > cn_high) {
              result = RELATION_RESULT_FALSE;
            }
            break;
          case OPERATOR_GT:
            if (cn >= cn_high) {
              result = RELATION_RESULT_FALSE;
            }
            break;
          default: break;
        }
      }
    }
    wstr = 0;
    switch (result) {
      case RELATION_RESULT_UNKNOWN: break;
      case RELATION_RESULT_TRUE:    wstr = "operation is always TRUE";  break;
      case RELATION_RESULT_FALSE:   wstr = "operation is always FALSE"; break;
    }
    if (wstr
      && !both_const
      && !pf->relwarn_hold
      && pfile_flag_test(pf, PFILE_FLAG_WARN_MISC)) {
      pfile_log(pf, PFILE_LOG_WARN, "%s", wstr);
    }
  }
  return result;
}

/*
 * Check to see if the universal in val1 is compatible with val2
 * (it's compatible if its value will fit within the definition
 * if val2).
 * nb: the operator is needed here for the logicals and for
 *     binary and -- as long as the high order bytes of binary
 *     and are 0xff they have no bearing on the result, therefore
 *     should not affect overflow
 */
boolean_t pfile_value_def_overflow_check(pfile_t *pf, 
  operator_t op, const variable_def_t vdef, value_t val)
{
  boolean_t        overflow;

  if (operator_is_logical(op)) {
    /* there is no overflow with logical operators */
    overflow = BOOLEAN_FALSE;
  } else {
    variable_const_t cn;
    variable_sz_t    vdef_sz;

    cn      = value_const_get(val);
    vdef_sz = variable_def_sz_get(vdef);
    if (!variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_BIT)) {
      vdef_sz *= 8;
    }
    if ((OPERATOR_ANDB == op) && (vdef_sz < VARIABLE_CONST_BIT_CT)) {
      /*
       * in this case, as long as all of the high order bits are set
       * there is no overflow since x & 0xff == x
       */
      variable_const_t hbits; /* the high order bits */
      variable_const_t lbit;  /* low order bit       */
      variable_const_t mask;
      
      hbits = cn;
      lbit  = ((variable_const_t) 1) << vdef_sz;
      mask  = lbit - 1;
      hbits = hbits & ~mask;
      if ((0 == hbits) || (0 == hbits + lbit)) {
        cn = cn & mask;
      }
    }
    if (variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_SIGNED)) {
      variable_const_t hi;
      variable_const_t lo;
      char             cn_buf[50];
      char             lo_buf[50];
      char             hi_buf[50];
      
      hi = variable_const_mask_get((unsigned) vdef_sz - 1);
      lo = variable_const_sign_extend(
        ((variable_const_t) 1) << ((unsigned) vdef_sz - 1),
        (unsigned) vdef_sz);
      if (value_is_signed(val)) {
        overflow = (variable_const_cmp(cn, lo, VARIABLE_CONST_BIT_CT,
            BOOLEAN_TRUE) < 0)
          || (variable_const_cmp(cn, hi, VARIABLE_CONST_BIT_CT,
            BOOLEAN_TRUE) > 0);
      } else {
        overflow = (cn > hi);
      }
      if (overflow) {
        variable_const_str_get(cn, value_is_signed(val), cn_buf,
            sizeof(cn_buf));
        variable_const_str_get(lo, BOOLEAN_TRUE, lo_buf, sizeof(lo_buf));
        variable_const_str_get(hi, BOOLEAN_TRUE, hi_buf, sizeof(hi_buf));
        pfile_log(pf, PFILE_LOG_WARN, "value out of range (%s:%s...%s)",
          cn_buf, lo_buf, hi_buf);
      }
    } else if (vdef_sz < VARIABLE_CONST_BIT_CT) { /* vdef is unsigned */
      variable_const_t hi;

      hi = variable_const_mask_get((unsigned) vdef_sz);
      if (((OPERATOR_ADD == op) || (OPERATOR_SUB == op))
        && value_is_signed(val)
        && variable_const_is_negative(cn, VARIABLE_CONST_BIT_CT)) {
        overflow = BOOLEAN_FALSE;
      } else {
        overflow = cn > hi;
      }
      if (overflow) {
        char cn_buf[50];
        char hi_buf[50];

        variable_const_str_get(cn, value_is_signed(val), cn_buf,
            sizeof(cn_buf));
        variable_const_str_get(hi, BOOLEAN_FALSE, hi_buf, sizeof(hi_buf));
        if (value_is_signed(val)) {
          pfile_log(pf, PFILE_LOG_WARN, "value out of range (%s:0...%s)",
            cn_buf, hi_buf);
        } else {
          pfile_log(pf, PFILE_LOG_WARN, "value out of range (%s:0...%s)",
            cn_buf, hi_buf);
        }
      }
    } else {
      overflow = BOOLEAN_FALSE;
    }
  }
  return overflow;
}

static boolean_t pfile_value_overflow_check(pfile_t *pf, 
  operator_t op, value_t val1, value_t val2)
{
  boolean_t overflow;

  overflow = BOOLEAN_FALSE;
  if (value_is_number(val1) 
    && value_is_number(val2)
    && (value_is_universal(val1) || value_is_universal(val2))) {
    if (value_is_universal(val2)) {
      value_t tmp;

      tmp  = val2;
      val2 = val1;
      val1 = tmp;
    }
    (void) pfile_value_def_overflow_check(pf, op, value_def_get(val2), val1);
  }
  return overflow;
}

static boolean_t pfile_const_value_fits_def(
    variable_const_t      cn,
    boolean_t             cn_signed,
    const variable_def_t  vdef)
{
  variable_sz_t vdef_sz;
  boolean_t     fits;

  vdef_sz = variable_def_sz_get(vdef);
  if (!variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_BIT)) {
    vdef_sz *= 8;
  }
  if (variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_SIGNED)) {
    variable_const_t hi;
    variable_const_t lo;

    hi = variable_const_mask_get((unsigned) vdef_sz - 1);
    lo = variable_const_sign_extend(
      ((variable_const_t) 1) << ((unsigned) vdef_sz - 1),
      (unsigned) vdef_sz);
    if (cn_signed) {
      fits = (variable_const_cmp(cn, lo, VARIABLE_CONST_BIT_CT,
          BOOLEAN_TRUE) >= 0)
        && (variable_const_cmp(cn, hi, VARIABLE_CONST_BIT_CT,
          BOOLEAN_TRUE) <= 0);
    } else {
      fits = cn <= hi;
    }
  } else if (vdef_sz < VARIABLE_CONST_BIT_CT) {
    variable_const_t hi;

    hi = variable_const_mask_get((unsigned) vdef_sz);
    fits = cn <= hi;
  } else {
    fits = BOOLEAN_TRUE;
  }
  return fits;
}

/* do two things
 *   1. if val2 exists, make sure val1 & val2 are compatible
 *   2. make sure the result of (val1/val2) is compatible with dst
 */
void pfile_value_def_type_check(pfile_t *pf, operator_t op, 
    variable_def_t dst_def, value_t val1, value_t val2)
{
  if (variable_def_type_is_number(variable_def_type_get(dst_def)) 
      && value_is_number(val1)
      && ((VALUE_NONE == val2) || value_is_number(val2))
      && (pfile_flag_test(pf, PFILE_FLAG_WARN_TRUNCATE)
          || pfile_flag_test(pf, PFILE_FLAG_WARN_CONVERSION))) {
    variable_def_t rdef;
    flag_t         warnings;

    warnings = 0; /* assume no warnings */
    rdef = pfile_variable_def_promotion_get(pf, op, val1, val2);
    if (value_is_universal(val2)) {
      value_t tmp;

      tmp  = val1;
      val1 = val2;
      val2 = tmp;
    }
    if (!value_is_universal(val1)) {
      if ((VALUE_NONE == val2) && value_is_const(val1)) {
        if (!pfile_const_value_fits_def(value_const_get(val1),
            value_is_signed(val1), dst_def)) {
          warnings |= PFILE_FLAG_WARN_TRUNCATE;
        }
      } else {
        if (((VALUE_NONE != val2) 
              && (value_is_signed(val1) ^ value_is_signed(val2))) 
            || ((VALUE_NONE == val2)
              && (variable_def_flag_test(dst_def, VARIABLE_DEF_FLAG_SIGNED) 
                ^ variable_def_flag_test(rdef, VARIABLE_DEF_FLAG_SIGNED)))) {
          warnings |= PFILE_FLAG_WARN_CONVERSION;
        }
        if (variable_def_sz_get(dst_def) < variable_def_sz_get(rdef)) {
          warnings |= PFILE_FLAG_WARN_TRUNCATE;
        }
      }
    } else {
      /* determine which warnings are appropriate; these are done
       * using a flag to make sure warnings are only reported once
       */
      variable_sz_t val1_bitsz;
      flag_t        val1_flags;
      variable_sz_t dst_bitsz;

      variable_calc_sz_min(value_const_get(val1), &val1_bitsz, 0, 
          &val1_flags);

      val1_bitsz *= 8;  
      /*
       * nb: only warn if the universal is signed, and the other not.
       *     otherwise ``var sbyte xx = 0'' will throw a warning
       */
      if (VALUE_NONE == val2) {
        if (!variable_def_flag_test(dst_def, VARIABLE_DEF_FLAG_SIGNED)
          && value_is_signed(val1)) {
          warnings |= PFILE_FLAG_WARN_CONVERSION;
        }
      } else {
        if (!value_is_signed(val2) && value_is_signed(val1)) {
          warnings |= PFILE_FLAG_WARN_CONVERSION;
        }
      }

      dst_bitsz = variable_def_sz_get(dst_def);
      if (variable_def_flag_test(dst_def, VARIABLE_DEF_FLAG_BIT)) {
        /* 
         * determine how many *bits* are required to hold val1
         * if the topmost bit is 1, subtract the number of 1s on
         *   the left side
         * if the topmost bit is 0, subtract the number of 0s on
         *   the left side
         */
        variable_const_t n;
        ulong            top_bit;

        top_bit = 1UL << (val1_bitsz - 1);

        n = value_const_get(val1);
        if (n & top_bit) {
          while (top_bit && (n & top_bit)) {
            top_bit >>= 1;
            val1_bitsz--;
          }
        } else {
          while (top_bit && !(n & top_bit)) {
            top_bit >>= 1;
            val1_bitsz--;
          }
        }
      } else {
        dst_bitsz *= 8;
      }

      if (dst_bitsz < val1_bitsz) {
        warnings |= PFILE_FLAG_WARN_TRUNCATE;
      }
    }
    if ((warnings & PFILE_FLAG_WARN_TRUNCATE)
      && pfile_flag_test(pf, PFILE_FLAG_WARN_TRUNCATE)) {
      pfile_log(pf, PFILE_LOG_WARN, 
          "assignment to smaller type; truncation possible");
    }
    if ((warnings & PFILE_FLAG_WARN_CONVERSION)
      && pfile_flag_test(pf, PFILE_FLAG_WARN_CONVERSION)) {
      pfile_log(pf, PFILE_LOG_WARN, "signed/unsigned mismatch");
    }
  }
  if (value_is_float(val1) || value_is_float(val2)) {
    /* make sure this is legal for a floating point value */
    switch (op) {
      case OPERATOR_EQ:
      case OPERATOR_NE:
        if (pfile_flag_test(pf, PFILE_FLAG_WARN_MISC)) {
          pfile_log(pf, PFILE_LOG_WARN,
            "You should not compare floating values for equality");
        }
        break;
      case OPERATOR_ADD:
      case OPERATOR_SUB:
      case OPERATOR_MUL:
      case OPERATOR_DIV:
      case OPERATOR_MOD:
      case OPERATOR_LT:
      case OPERATOR_LE:
      case OPERATOR_GE:
      case OPERATOR_GT:
      case OPERATOR_ANDL:
      case OPERATOR_ORL:
      case OPERATOR_NOTL:
      case OPERATOR_ASSIGN:
      case OPERATOR_POINTER_ASSIGN:
      case OPERATOR_NEG:
      case OPERATOR_INCR:
      case OPERATOR_DECR:
      case OPERATOR_LOGICAL:
        break;
      case OPERATOR_ANDB:
      case OPERATOR_ORB:
      case OPERATOR_XORB:
      case OPERATOR_CMPB:
      case OPERATOR_SHIFT_LEFT:
      case OPERATOR_SHIFT_RIGHT:
      case OPERATOR_SHIFT_RIGHT_ARITHMETIC:
      case OPERATOR_NULL:
      case OPERATOR_REFERENCE:
      case OPERATOR_DOT:
      case OPERATOR_SUBSCRIPT:
      case OPERATOR_CAST:
      case OPERATOR_CT:
        pfile_log(pf, PFILE_LOG_ERR,
          "Invalid operation");
    }
  }
}

static void pfile_value_type_check(pfile_t *pf, operator_t op, value_t dst,
    value_t val1, value_t val2)
{
  pfile_value_def_type_check(pf, op, value_def_get(dst), val1, val2);
}

/*
 * NAME
 *   pfile_cmd_op_add
 *
 * DESCRIPTION
 *   add an operation
 *
 * PARAMETERS
 *   pf:
 *   label : line label
 *   op    : operation
 *   dst   : destination, can be 0 to allocate a temporary
 *   val1  : value 1 (can be 0 for unary operations, cannot be 0 for binary)
 *   val2  : value 2 (must be 0 for unary, can be 0 for binary)
 *
 * RETURN
 *
 * NOTES
 *   if dst is NULL on entry, it will hold a value on successful exit
 *   if dst is a function
 *     *error*
 *   if dst is a reference
 *     if *dst is a function
 *       src must be a function with a matching signature
 */
void pfile_cmd_op_add_assign(pfile_t *pf, operator_t op,
  value_t *pdst, value_t src)
{
  if (!*pdst 
    && (OPERATOR_ASSIGN == op) 
    && value_is_const(src)
    && value_is_number(src)) {
    *pdst = pfile_constant_get(pf, value_const_get(src),
      value_def_get(src));
  } else {
    value_t dst;

    dst = *pdst;
    if (!dst) {
      assert(VARIABLE_DEF_TYPE_FUNCTION != value_type_get(src));
      dst = pfile_value_temp_get_from_def(pf, value_def_get(src));
      *pdst = dst;
    }
    /* `p is x` must bind an address before the same-def aggregate fast path:
     * pointer-to-const-byte[N] and const byte x[N] have matching array shapes
     * and would otherwise emit a bulk copy through the pointer slot. */
    if ((OPERATOR_POINTER_ASSIGN == op) && value_is_pointer(dst)
        && (value_is_array(src) || value_is_function(src) || !value_is_const(src)
            || (value_variable_get(src) != NULL))) {
      cmd_t cmd;

      cmd = cmd_op_alloc(op, dst, src, VALUE_NONE);
      pfile_cmd_add(pf, cmd);
    /* assignment is allowed between variables that are logically
       the same (the definitions match), and between any two
       numbers */
    } else if ((variable_def_is_same(value_def_get(dst), value_def_get(src)))
      || (value_is_number(dst) && value_is_number(src))) {
      cmd_t cmd;

      cmd = cmd_op_alloc(op, dst, src, VALUE_NONE);
      pfile_cmd_add(pf, cmd);
    } else if (value_is_pointer(dst) 
      && value_is_function(src)
      && variable_def_is_same(variable_def_member_def_get(
        variable_def_member_get(value_def_get(dst))),
          value_def_get(src))) {
      /* assign function to pointer */
      cmd_t cmd;

      cmd = cmd_op_alloc(op, dst, src, VALUE_NONE);
      pfile_cmd_add(pf, cmd);
    } else if ((OPERATOR_ASSIGN == op)
        && value_is_array(dst) && value_is_array(src)
        && (value_ct_get(dst) == value_ct_get(src))
        && variable_def_is_same(
            variable_def_member_def_get(
              variable_def_member_get(value_def_get(dst))),
            variable_def_member_def_get(
              variable_def_member_get(value_def_get(src))))) {
      /* Aggregate init uses a temp array whose outer def may differ from the
       * destination (e.g. const/volatile flags) even when element type and
       * length match; allow the bulk copy. */
      cmd_t cmd;

      cmd = cmd_op_alloc(op, dst, src, VALUE_NONE);
      pfile_cmd_add(pf, cmd);
    } else if ((OPERATOR_ASSIGN == op) && value_is_pointer(dst)) {
      /* Handle pointer dereference assignment (pointer = value) */
      cmd_t cmd;
      cmd = cmd_op_alloc(op, dst, src, VALUE_NONE);
      pfile_cmd_add(pf, cmd);
    } else if ((OPERATOR_POINTER_ASSIGN == op) && value_is_pointer(dst)) {
      /* `p is rhs` missed the branches above (e.g. pseudo-variable RHS that is
       * const with no variable_t, or other bindable shapes). Still emit the
       * bind so pointer assign_ct is bumped and slot_bind_check passes. */
      cmd_t cmd;

      cmd = cmd_op_alloc(op, dst, src, VALUE_NONE);
      pfile_cmd_add(pf, cmd);
    } else {
      /* Provide more helpful error messages for common assignment errors */
      if (dst && src) {
        variable_def_type_t dst_type = value_type_get(dst);
        variable_def_type_t src_type = value_type_get(src);
        
        if (dst_type == VARIABLE_DEF_TYPE_ARRAY && src_type == VARIABLE_DEF_TYPE_VALUE) {
          pfile_log(pf, PFILE_LOG_ERR, "Cannot assign single value to array. Use array[index] = value instead");
        } else if (dst_type == VARIABLE_DEF_TYPE_ARRAY && value_is_number(src)) {
          pfile_log(pf, PFILE_LOG_ERR, "Cannot assign number to array. Use array[index] = value instead");
        } else if (dst_type == VARIABLE_DEF_TYPE_FUNCTION && src_type == VARIABLE_DEF_TYPE_VALUE) {
          pfile_log(pf, PFILE_LOG_ERR, "Cannot assign value to function. Functions cannot be assigned values");
        } else if (dst_type == VARIABLE_DEF_TYPE_POINTER && !value_is_pointer(src) && !value_is_number(src)) {
          pfile_log(pf, PFILE_LOG_ERR, "Cannot assign incompatible type to pointer. Use pointer is variable_name instead");
        } else {
          pfile_log(pf, PFILE_LOG_ERR, "Invalid assignment: incompatible types (destination type %d, source type %d)", 
                    (int)dst_type, (int)src_type);
        }
      } else {
        pfile_log(pf, PFILE_LOG_ERR, "invalid operation (assign: op=%d, dst_type=%d, src_type=%d)", 
                  (int)op, 
                  dst ? (int)value_type_get(dst) : -1, 
                  src ? (int)value_type_get(src) : -1);
      }
    }
  }
}

static void pfile_cmd_op_add_unary(pfile_t *pf, operator_t op,
    value_t *pdst, value_t src)
{
  value_t       dst;
  variable_def_t def;

  if (!value_is_number(src)) {
    pfile_log(pf, PFILE_LOG_ERR, "invalid operation");
  } else if (!*pdst && value_is_const(src)) {
    pf_const_t n;

    n = pfile_op_const_exec(pf, op, src, VALUE_NONE);
    if (value_is_bit(src)) {
      n.u.n &= (1 << value_sz_get(src)) - 1;
    }
    def = value_def_get(src);
    if ((PF_CONST_TYPE_INTEGER == n.type)
        && variable_const_is_negative(n.u.n, VARIABLE_CONST_BIT_CT)
        && !variable_def_flag_test(def, VARIABLE_DEF_FLAG_SIGNED)) {
      def = variable_def_flags_change(def,
          variable_def_flags_get_all(def) | VARIABLE_DEF_FLAG_SIGNED);
    }
    *pdst = pf_const_to_const(pf, n, def);
  } else {
    cmd_t cmd;

    dst = *pdst;
    if (!dst) {
      dst = pfile_value_temp_get_from_def(pf, 
          pfile_variable_def_promotion_get(pf, op, src, VALUE_NONE));
      *pdst = dst;
    }
    /* see nb about volatile in add_binary */
    cmd = cmd_op_alloc(op, dst, src, VALUE_NONE);
    pfile_cmd_add(pf, cmd);
  }
}
/*
  (1) if both operands are the same type
        no conversion is done
  (2) if both operands are signed
        the operand of lesser width is promoted to the operand of greater width
  (3) if the operand that is unsigned has a greater or equal width than the 
        signed, the signed operand is converted to the unsigned type
  (4) if the operand that is signed can represent all values of the operand 
        that is unsigned, the unsigned operand is converted to the signed
  (5) failing all else, both operands are converted to the unsigned integer 
        type with the same width as the signed operand
 */
static void pfile_cmd_op_add_binary(pfile_t *pf, operator_t op,
    value_t *pdst, value_t val1, value_t val2)
{
  if (!*pdst 
    && (value_is_const(val1) && !value_is_lookup(val1))
    && value_is_number(val1) 
    && (value_is_const(val2) && !value_is_lookup(val2))
    && value_is_number(val2)) {
    variable_def_t   def;
    pf_const_t       n;

    n.type = PF_CONST_TYPE_NONE;
    def = pfile_variable_def_promotion_get(pf, op, val1, val2);
    n = pfile_op_const_exec(pf, op, val1, val2);
    if ((PF_CONST_TYPE_INTEGER == n.type)
        && variable_const_is_negative(n.u.n, VARIABLE_CONST_BIT_CT)
        && !variable_def_flag_test(def, VARIABLE_DEF_FLAG_SIGNED)) {
      /* a negative fold result must carry a signed def, otherwise range
       * checks and later folds read the two's complement as huge unsigned */
      def = variable_def_flags_change(def,
          variable_def_flags_get_all(def) | VARIABLE_DEF_FLAG_SIGNED);
    }
    *pdst = pf_const_to_const(pf, n, def);
  } else {
    value_t dst;

    dst = *pdst;
    if ((!value_is_number(val1) || !value_is_number(val2))
        && ((OPERATOR_EQ != op) && (OPERATOR_NE != op))) {
        pfile_log(pf, PFILE_LOG_ERR, "invalid operation");
    } else {
      variable_def_t rdef; /* result */
      cmd_t          cmd;

      if (!value_is_number(val1)) {
        /* the result is a byte */

        rdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER, 
            VARIABLE_DEF_FLAG_NONE, 1);
      } else {
        rdef = pfile_variable_def_promotion_get(pf, op, val1, val2);
      }
      if (!dst) {
        dst = pfile_value_temp_get_from_def(pf, rdef);
        *pdst = dst;
      }
      /* nb: if dst is volatile, the work must be done in a temporary
             *but* this needs to be left to the backend because some
             operations can be optimized at a lower level */
      cmd = cmd_op_alloc(op, dst, val1, val2);
      pfile_cmd_add(pf, cmd);
    }
  }
}

/*
 * True if dst is a store through a data pointer whose pointee type is const
 * (e.g. var pointer const byte p then p = x, p[i] = x, or p[i]++).
 * POINTER_ASSIGN (p is x) binds an address; it is not a store through the pointee.
 */
static boolean_t pfile_value_writes_through_const_pointee(const value_t val)
{
  variable_def_t         def;
  variable_def_member_t  mbr;
  variable_def_t         pointee;

  if (!val) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(val);
  if (!def) {
    return BOOLEAN_FALSE;
  }
  /* After subscript: type is the element; flags include POINTER + INDIRECT. */
  if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_CONST)
      && value_flag_test(val, VALUE_FLAG_POINTER)
      && value_flag_test(val, VALUE_FLAG_INDIRECT)) {
    return BOOLEAN_TRUE;
  }
  /* Scalar write-through: destination value is still typed as pointer. */
  if (value_is_pointer(val)) {
    mbr = variable_def_member_get(def);
    if (mbr) {
      pointee = variable_def_member_def_get(mbr);
      if (pointee && variable_def_flag_test(pointee, VARIABLE_DEF_FLAG_CONST)) {
        if (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(pointee)) {
          return BOOLEAN_FALSE;
        }
        return BOOLEAN_TRUE;
      }
    }
  }
  return BOOLEAN_FALSE;
}

void pfile_cmd_op_add(pfile_t *pf,
  operator_t op, value_t *pdst, value_t val1, value_t val2)
{
  value_t  dst;
  result_t rc;

  rc = RESULT_OK;

  dst = (pdst) ? *pdst : VALUE_NONE;

  /* check val1 and/or val2 for strings. currently
     strings cannot be used as operators for any expression

     *except* assignment!
   */
  if ((OPERATOR_ASSIGN != op) || (!pdst)
    || !value_def_is_same(*pdst, val1)) {
    if (!pfile_expr_string_fix(pf, val1)) {
      rc = RESULT_INVALID;
    } else if (! pfile_expr_string_fix(pf, val2)) {
      rc = RESULT_INVALID;
    }
  }
  if (RESULT_OK == rc) {
    /* 1st, make sure val1 and/or val2 exit.
     *
     * Malformed input can reach this point with an operand missing: the
     * front end logs its own diagnostic and then keeps parsing for error
     * recovery, so a half-built expression can be submitted here. That used
     * to be an assertion, which turned a recoverable syntax error into
     * SIGABRT (fuzzer: `pragma target OSC case hs` aborts the process).
     * Report the missing operand and substitute a well-formed result; the
     * compile still fails, it just fails with diagnostics, not a core dump.
     */
    boolean_t operands_ok;

    operands_ok = BOOLEAN_TRUE;
    if (operator_is_binary(op)) {
      if ((VALUE_NONE == val1)
        || ((VALUE_NONE == val2) && (VALUE_NONE == dst))) {
        operands_ok = BOOLEAN_FALSE;
      } else if (VALUE_NONE == val2) {
        val2 = val1;
        val1 = dst;
      }
    } else if (operator_is_unary(op)) {
      if ((VALUE_NONE == val1) && (VALUE_NONE == dst)) {
        operands_ok = BOOLEAN_FALSE;
      } else if (VALUE_NONE == val1) {
        val1 = dst;
      }
    }
    if (!operands_ok) {
      /* Nothing can be emitted without an operand. Report it and leave a
       * well-formed result behind: callers build branches/assigns on *pdst
       * and a NULL result is dereferenced further down (that path is how a
       * syntax error became SIGABRT-then-SIGSEGV instead of a diagnostic).
       * val1/val2 are not locked yet here, so they must not be released. */
      pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
      if (pdst) {
        if (*pdst) {
          value_release(*pdst);
        }
        *pdst = pfile_constant_get(pf, 0, 0);
      }
      return;
    }
    value_lock(val1);
    value_lock(val2);

    /* non-numbers only allow:
     *   assign to 0
     *   assign to like type
     *   equal (of like type)
     *   not equal (of like type)
     */   
    pfile_value_type_check(pf, op, dst, val1, val2);
    if (dst && value_is_const(dst) && !value_name_get(dst)) {
      dst = VALUE_NONE;
    }
    /* check for an invalid read or write.
     * VARIABLE_FLAG_[READ|WRITE] are not fully implemented yet
     * so this is kind of a hack. For dst, if READ is set and
     * not WRITE issue an error. For val[1|2], if WRITE is set
     * and not READ issue an error. */
    if (value_vflag_test(dst, VARIABLE_FLAG_READ)
      && !value_vflag_test(dst, VARIABLE_FLAG_WRITE)) {
      pfile_log(pf, PFILE_LOG_ERR, "cannot write to %s",
        value_name_get(dst));
    } else if (value_vflag_test(val1, VARIABLE_FLAG_WRITE)
      && !value_vflag_test(val1, VARIABLE_FLAG_READ)) {
      pfile_log(pf, PFILE_LOG_ERR, "cannot read from %s",
        value_name_get(val1));
    } else if (value_vflag_test(val2, VARIABLE_FLAG_WRITE)
      && !value_vflag_test(val2, VARIABLE_FLAG_READ)) {
      pfile_log(pf, PFILE_LOG_ERR, "cannot read from %s",
        value_name_get(val2));
    } else if (dst && (value_is_const(dst) || value_is_lookup(dst))) {
      pfile_log(pf, PFILE_LOG_ERR, "assign to constant");
    } else if (dst
        && (OPERATOR_ASSIGN == op || OPERATOR_INCR == op || OPERATOR_DECR == op)
        && pfile_value_writes_through_const_pointee(dst)) {
      pfile_log(pf, PFILE_LOG_ERR,
          "cannot assign or modify through pointer to const data");
    } else if (OPERATOR_NULL == op) {
    } else if (operator_is_assign(op)) {
      (void) pfile_value_overflow_check(pf, op, dst, val1);
      if (!(val1 || val2)) {
        pfile_log(pf, PFILE_LOG_ERR, "value missing");
      } else {
        pfile_cmd_op_add_assign(pf, op, &dst, val1);
      }
    } else if (operator_is_unary(op)) {
      if (val2 || !(val1 || dst)) {
        pfile_log(pf, PFILE_LOG_ERR, "value missing");
      } else {
        assert(val2 == 0);
        assert(val1 || dst);
        if (!val1) {
          val1 = dst;
        }
        pfile_cmd_op_add_unary(pf, op, &dst, val1);
      }
    } else { /* op_is_binary! */
      if (!val1 || !(val2 || dst)) {
      } else {
        relation_result_t result;

        assert(val1 != 0);
        assert(val2 || dst);
        if (!val2) {
          val2 = val1;
          val1 = dst;
        }
        result = RELATION_RESULT_UNKNOWN;
        if (operator_is_relation(op)) {
          result = pfile_expr_relation_check(pf, op, val1, val2);
        } else {
          (void) pfile_value_overflow_check(pf, op, val1, val2);
        }
        switch (result) {
          case RELATION_RESULT_UNKNOWN:
            pfile_cmd_op_add_binary(pf, op, &dst, val1, val2);
            break;
          case RELATION_RESULT_TRUE:
          case RELATION_RESULT_FALSE:
            value_release(val1);
            val1 = pfile_constant_get(pf, (result == RELATION_RESULT_TRUE),
              pfile_variable_def_promotion_get(pf, op, val1, val2));
            pfile_cmd_op_add_assign(pf, OPERATOR_ASSIGN, &dst, val1);
            break;
        }
      }
    }

    value_release(val1);
    value_release(val2);
    if (pdst) {
      if (dst != *pdst) {
        if (*pdst) {
          value_release(*pdst);
        }
        *pdst = dst;
      }
    } else if (dst) {
      value_release(dst);
    }
  }
}

/*
 * I messed up awhile ago and need this hack. '"'...'"' can create
 * either a single character or a string. Unfortunately, there's no
 * way to determine it is supposed to be until much later. So, if it
 * is determined that a character was required, this will change the
 * value as appropriate
 *   return BOOLEAN_TRUE on success, or BOOLEAN_FALSE on failure
 */
boolean_t pfile_expr_string_fix(pfile_t *pf, value_t val)
{
  boolean_t rc;

  rc = BOOLEAN_TRUE;
  if (value_is_array(val)
    && value_is_string(val)) {
    /* Heap `var string`: one array member with ct = 3×index width (header +
     * payload). Not the lowered literal / fixed `string[N]` shape where this
     * hack resolves char vs string; dereferencing would strip the string def
     * and `value_ct_get` != 1 was wrongly rejected (e.g. `p is dyn_string`). */
    if (value_dflag_test(val,
        VARIABLE_DEF_FLAG_STRING | VARIABLE_DEF_FLAG_STRING_DYNAMIC)) {
      return BOOLEAN_TRUE;
    }
    /* Fixed `string[N]` (STRING only): multi-element array is never the
     * char-vs-string literal ambiguity this routine resolves; ct>1 must not
     * error (e.g. RHS of `pointer is fixed_string` before assign lowering). */
    if (value_dflag_test(val, VARIABLE_DEF_FLAG_STRING)
        && (1 != value_ct_get(val))) {
      return BOOLEAN_TRUE;
    }
    if (1 != value_ct_get(val)) {
      pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
      rc = BOOLEAN_FALSE;
    }
    value_dereference(val);
  }
  return rc;
}



/************************************************************
 **
 ** pf_op.c : perform operations on two constant values
 **
 ** Copyright (c) 2007, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <assert.h>
#include "pf_expr.h"
#include "pf_op.h"

/* return n sign extended as necessary */
static unsigned pfile_op_def_bit_ct_get(
    const variable_def_t def)
{
  variable_sz_t sz;

  sz = variable_def_sz_get(def);
  if (!variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)) {
    sz *= 8;
  }
  if (sz > VARIABLE_CONST_BIT_CT) {
    sz = VARIABLE_CONST_BIT_CT;
  }
  return sz;
}

static variable_const_t pfile_op_val_sign_extend(
    variable_const_t     n,
    const variable_def_t def)
{
  unsigned sz; /* *bit* size */

  assert((VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(def))
      || (VARIABLE_DEF_TYPE_BOOLEAN == variable_def_type_get(def))
      || (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(def))
      || (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))
      || (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(def)));

  sz = pfile_op_def_bit_ct_get(def);
  if (VARIABLE_DEF_TYPE_BOOLEAN == variable_def_type_get(def)) {
    n = !!n;
  } else if (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(def)) {
    /* Function types don't need sign extension */
    /* They will be handled by the calling code */
  } else if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(def)) {
    /* Array types don't need sign extension */
    /* They will be handled by the calling code */
  } else {
    n &= variable_const_mask_get(sz);
    if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_SIGNED)) {
      n = variable_const_sign_extend(n, sz);
    }
  }
  return n;
}

/* return the result if n is assigned */
static variable_const_t pfile_op_val_get(
    variable_const_t     n,
    const variable_def_t def)
{
  /* Debug: check what type we're getting */
  variable_def_type_t def_type = variable_def_type_get(def);
  /* Support for extended types in pointer operations */
  assert((VARIABLE_DEF_TYPE_INTEGER == def_type)
      || (VARIABLE_DEF_TYPE_BOOLEAN == def_type)
      || (VARIABLE_DEF_TYPE_POINTER == def_type)
      || (VARIABLE_DEF_TYPE_FUNCTION == def_type)
      || (VARIABLE_DEF_TYPE_ARRAY == def_type));

  return pfile_op_val_sign_extend(variable_const_normalize(n, def), def);
}

pf_const_t pfile_op_const_exec(pfile_t *pf,
    operator_t op,
    value_t    val1,
    value_t    val2)
{
  variable_def_t   def1;
  variable_def_t   def2;
  variable_def_t   rdef;
  pf_const_t       n;
  pf_const_t       n1;
  pf_const_t       n2;
  unsigned         sz;
  variable_const_t mask;
  boolean_t        result_is_signed;
  boolean_t        is_float;

  assert(value_is_const(val1) || value_is_pseudo_const(val1));
  assert(operator_is_unary(op) 
      || value_is_const(val2) 
      || value_is_pseudo_const(val2));

  def1 = value_def_get(val1);
  def2 = value_def_get(val2);
  rdef = pfile_variable_def_promotion_get(pf, op, val1, val2);

  if (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(def1)) {
    n1.type = PF_CONST_TYPE_FLOAT;
    n1.u.f  = value_const_float_get(val1);
  } else {
    n1.type = PF_CONST_TYPE_INTEGER;
    n1.u.n  = pfile_op_val_get(value_const_get(val1), def1);
  }

  if (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(def2)) {
    n2.type = PF_CONST_TYPE_FLOAT;
    n2.u.f  = value_const_float_get(val2);
  } else {
    n2.type = PF_CONST_TYPE_INTEGER;
    n2.u.n  = (def2) 
              ? pfile_op_val_get(value_const_get(val2), def2)
              : 0;
  }

  is_float = (n1.type == PF_CONST_TYPE_FLOAT)
    || (n2.type == PF_CONST_TYPE_FLOAT);
  /* If either are float, the result is float */
  if (is_float) {
    if (n2.type != PF_CONST_TYPE_FLOAT) {
      n2.type  = PF_CONST_TYPE_FLOAT;
      if (value_is_signed(val2)) {
        n2.u.f = (long) n2.u.n;
      } else {
        n2.u.f = n2.u.n;
      }
    } else if (n1.type != PF_CONST_TYPE_FLOAT) {
      n1.type  = PF_CONST_TYPE_FLOAT;
      if (value_is_signed(val1)) {
        n1.u.f = (long) n1.u.n;
      } else {
        n1.u.f = n1.u.n;
      }
    }
  }
  sz               = pfile_op_def_bit_ct_get(rdef);
  mask             = variable_const_mask_get(sz);
  result_is_signed = variable_def_flag_test(
                        pfile_variable_def_promotion_get(pf, OPERATOR_NULL,
                        val1, val2), VARIABLE_DEF_FLAG_SIGNED);

  n.type = PF_CONST_TYPE_NONE;
  n.u.n  = 0;
  switch (op) {
    case OPERATOR_NULL:
    case OPERATOR_REFERENCE:
    case OPERATOR_DOT:
    case OPERATOR_CAST:
    case OPERATOR_SUBSCRIPT:
    case OPERATOR_CT:
      assert(0);
      break;
    case OPERATOR_POINTER_ASSIGN:
      /* Pointer assignments are not constant expressions */
      n.type = PF_CONST_TYPE_NONE;
      n.u.n = 0;
      return n;
    case OPERATOR_ADD:
      if (is_float) {
        n.type = PF_CONST_TYPE_FLOAT;
        n.u.f = n1.u.f + n2.u.f;
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n + n2.u.n;
      }
      break;
    case OPERATOR_SUB:
      if (is_float) {
        n.type = PF_CONST_TYPE_FLOAT;
        n.u.f = n1.u.f - n2.u.f;
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n - n2.u.n;
      }
      break;
    case OPERATOR_MUL:
      if (is_float) {
        n.type = PF_CONST_TYPE_FLOAT;
        n.u.f = n1.u.f * n2.u.f;
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n * n2.u.n;
      }
      break;
    case OPERATOR_DIV:
    case OPERATOR_MOD:
      if (is_float) {
        n.type = PF_CONST_TYPE_FLOAT;
        if (n2.u.f == 0) {
          pfile_log(pf, PFILE_LOG_ERR, "divide by zero");
        } else {
          if (OPERATOR_DIV == op) {
            n.u.f = n1.u.f / n2.u.f;
          }
        }
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        if (n2.u.n == 0) {
          pfile_log(pf, PFILE_LOG_ERR, "divide by zero");
        } else {
          boolean_t sign;
          boolean_t sign1;

          /* Sign fix-up is driven by the constants themselves: a folded
           * negative constant (e.g. -7, or 10 / -2) may carry an unsigned
           * literal def, so value_is_signed()/result_is_signed() under-
           * report and the divide ran unsigned over the full constant.
           * A positive literal never has the const sign bit set, so this
           * cannot misfire on genuinely unsigned operands.
           * Quotient sign = dividend XOR divisor; remainder sign follows
           * the dividend alone (C truncation semantics). */
          sign  = BOOLEAN_FALSE;
          sign1 = BOOLEAN_FALSE;
          if (variable_const_is_negative(n1.u.n, VARIABLE_CONST_BIT_CT)) {
            sign   = BOOLEAN_TRUE;
            sign1  = BOOLEAN_TRUE;
            n1.u.n = -n1.u.n;
          }
          if (variable_const_is_negative(n2.u.n, VARIABLE_CONST_BIT_CT)) {
            sign  ^= BOOLEAN_TRUE;
            n2.u.n = -n2.u.n;
          }
          n1.u.n = n1.u.n & mask;
          n2.u.n = n2.u.n & mask;
          n.u.n = (OPERATOR_DIV == op) ? n1.u.n / n2.u.n : n1.u.n % n2.u.n;
          if (OPERATOR_DIV == op) {
            if (sign) {
              n.u.n = -n.u.n;
            }
          } else if (sign1) {
            n.u.n = -n.u.n;
          }
        }
      }
      break;
    case OPERATOR_LT:
    case OPERATOR_LE:
    case OPERATOR_EQ:
    case OPERATOR_NE:
    case OPERATOR_GE:
    case OPERATOR_GT:
      if (is_float) {
        n.type = PF_CONST_TYPE_INTEGER;
        switch (op) {
          case OPERATOR_LT: n.u.n = n1.u.f <  n2.u.f; break;
          case OPERATOR_LE: n.u.n = n1.u.f <= n2.u.f; break;
          case OPERATOR_EQ: n.u.n = n1.u.f == n2.u.f; break;
          case OPERATOR_NE: n.u.n = n1.u.f != n2.u.f; break;
          case OPERATOR_GE: n.u.n = n1.u.f >= n2.u.f; break;
          case OPERATOR_GT: n.u.n = n1.u.f >  n2.u.f; break;
          default:
            break;
        }
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        if (BOOLEAN_FALSE == result_is_signed) {
          unsigned sz1;
          unsigned sz2;

          sz1 = value_sz_get(val1);
          if (!value_is_bit(val1)) {
            sz1 *= 8;
          }
          sz2 = value_sz_get(val2);
          if (!value_is_bit(val2)) {
            sz2 *= 8;
          }
          sz = (variable_sz_t) ((sz1 < sz2) ? sz2 : sz1);
          if (sz < 8) {
            sz = 8;
          }
          mask = variable_const_mask_get(sz);
          n1.u.n &= mask;
          n2.u.n &= mask;
        }
        switch (op) {
          case OPERATOR_LT:
            n.u.n = variable_const_cmp(n1.u.n, n2.u.n, sz,
                result_is_signed) < 0;
            break;
          case OPERATOR_LE:
            n.u.n = variable_const_cmp(n1.u.n, n2.u.n, sz,
                result_is_signed) <= 0;
            break;
          case OPERATOR_EQ:
            n.u.n = (n1.u.n == n2.u.n);
            break;
          case OPERATOR_NE:
            n.u.n = (n1.u.n != n2.u.n);
            break;
          case OPERATOR_GE:
            n.u.n = variable_const_cmp(n1.u.n, n2.u.n, sz,
                result_is_signed) >= 0;
            break;
          case OPERATOR_GT:
            n.u.n = variable_const_cmp(n1.u.n, n2.u.n, sz,
                result_is_signed) > 0;
            break;
          default:
            break; /* cannot get here */
        }
      }
      break;
    case OPERATOR_ANDL:
      n.type = PF_CONST_TYPE_INTEGER;
      if (is_float) {
        n.u.n = n1.u.f && n2.u.f;
      } else {
        n.u.n = n1.u.n && n2.u.n;
      }
      break;
    case OPERATOR_ORL:
      n.type = PF_CONST_TYPE_INTEGER;
      if (is_float) {
        n.u.n = n1.u.f || n2.u.f;
      } else {
        n.u.n = n1.u.n || n2.u.n;
      }
      break;
    case OPERATOR_NOTL:
      n.type = PF_CONST_TYPE_INTEGER;
      if (is_float) {
        n.u.n = !n1.u.f;
      } else {
        n.u.n = !n1.u.n;
      }
      break;
    case OPERATOR_ANDB:
      if (!is_float) {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n & n2.u.n;
      }
      break;
    case OPERATOR_ORB:
      if (!is_float) {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n | n2.u.n;
      }
      break;
    case OPERATOR_XORB:
      if (!is_float) {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n ^ n2.u.n;
      }
      break;
    case OPERATOR_CMPB:
      if (!is_float) {
        n.type = PF_CONST_TYPE_INTEGER;
        if (value_is_boolean(val1)) {
          n.u.n = (variable_const_t) ((n1.u.n) ? 0 : -1);
        } else {
          n.u.n = ~n1.u.n;
        }
      }
      break;
    case OPERATOR_ASSIGN:
      n.type = (is_float) ? PF_CONST_TYPE_FLOAT : PF_CONST_TYPE_INTEGER;
      n = n1;
      break;
    case OPERATOR_NEG:
      if (is_float) {
        n.type = PF_CONST_TYPE_FLOAT;
        n.u.f = -n1.u.f;
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = -n1.u.n;
      }
      break;
    case OPERATOR_INCR:
      if (is_float) {
        n.type = PF_CONST_TYPE_FLOAT;
        n.u.f = n1.u.f + 1;
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n + 1;
      }
      break;
    case OPERATOR_DECR:
      if (is_float) {
        n.type = PF_CONST_TYPE_FLOAT;
        n.u.f = n1.u.f + 1;
      } else {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = n1.u.n - 1;
      }
      break;
    case OPERATOR_SHIFT_LEFT:
      if (!is_float) {
        n.type = PF_CONST_TYPE_INTEGER;
        n.u.n = (n2.u.n >= VARIABLE_CONST_BIT_CT)
          ? 0
          : (n1.u.n << (unsigned) n2.u.n);
      }
      break;
    case OPERATOR_SHIFT_RIGHT:
    case OPERATOR_SHIFT_RIGHT_ARITHMETIC:
      if (!is_float) {
        n.type = PF_CONST_TYPE_INTEGER;
        {
          unsigned sign_bit;

          if (value_is_signed(val1)
            && (value_byte_sz_get(val1) < variable_def_sz_get(rdef))) {
            sign_bit = pfile_op_def_bit_ct_get(def1);
          } else {
            sign_bit = pfile_op_def_bit_ct_get(rdef);
          }
          if (n2.u.n >= sz) {
            n.u.n = (variable_const_t) (((OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)
              && variable_const_is_negative(n1.u.n, sign_bit))
            ? -1
            : 0);
          } else {
            n.u.n = (n1.u.n & mask) >> (unsigned) n2.u.n;
            if ((OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)
                && variable_const_is_negative(n1.u.n, sign_bit)) {
              n.u.n = variable_const_sign_extend(n.u.n,
                  (unsigned) (sz - n2.u.n));
            }
          }
        }
        n.u.n &= mask;
      }
      break;
    case OPERATOR_LOGICAL:
      n.type = PF_CONST_TYPE_INTEGER;
      if (is_float) {
        n.u.n = !!n1.u.f;
      } else {
        n.u.n = !!n1.u.n;
      }
      break;
  }
  if (!is_float) {
    n.u.n = pfile_op_val_get(n.u.n, rdef);
  }
  return n;
}

value_t pf_const_to_const(pfile_t *pf, pf_const_t c,
  variable_def_t def)
{
  value_t tmp;

  tmp = VALUE_NONE;
  switch (c.type) {
    case PF_CONST_TYPE_NONE:
      break;
    case PF_CONST_TYPE_INTEGER:
      tmp = pfile_constant_get(pf, c.u.n, def);
      break;
    case PF_CONST_TYPE_FLOAT:
      tmp = pfile_constant_float_get(pf, c.u.f, def);
      break;
  }
  return tmp;
}



/************************************************************
 **
 ** pic_op.c : PIC operator generation -- entry points and shared helpers
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
 * general note:
 *   all operations must take into account:
 *     constants
 *     single bit
 *     multi-bit
 *     simple
 *     indirect
 *     in W
 *     different banks
 * nb: operations on bitfields will occur *as if*
 *     the bitfield was promoted to the smallest
 *     integer type that can contain it
 *
 * naming convention
 *   pic_op_* -- only called via pic_op(...)
 */
/*
 * these are used to prevent duplicate calls to _multiply or _divide
 * for example:
 *    ch = n % 10
 *    n  = n / 10
 * they are reset when a label or branch is encountered
 * also if any of the values are used as a destination
 */

value_t pic_last_values[PIC_LAST_VALUE_CT];

void pic_last_value_set(pic_last_value_t which, value_t n)
{
  assert(which < PIC_LAST_VALUE_CT);

  value_lock(n);
  value_release(pic_last_values[which]);
  pic_last_values[which] = n;
}

void pic_last_values_reset(void)
{
  pic_last_value_t which;
  
  for (which = PIC_LAST_VALUE_FIRST; 
       which < PIC_LAST_VALUE_CT;
       which++) {
    pic_last_value_set(which, VALUE_NONE);
  }
}

boolean_t pic_value_is_w(value_t val)
{
  return (VALUE_NONE == val)
    || (VARIABLE_NONE == value_variable_get(val));
}

/* 
 * for a binary op, the result size is the greater of the two
 * operand sizes
 * If the result size is greater than the destination size, the
 * result size is lowered to the destination size
 */
variable_sz_t pic_result_sz_get(value_t val1, value_t val2, 
    value_t dst)
{
  variable_sz_t sz;

  sz = pic_value_is_w(val1) ? 1 : value_byte_sz_get(val1);

  if (val2 && !pic_value_is_w(val2)) {
    variable_sz_t val2_sz;

    val2_sz = value_byte_sz_get(val2);
    if (val2_sz > sz) {
      sz = val2_sz;
    }
  }
  /* if dst is boolean, the result must be done in the full
   * size of val1 & val2 because dst will be 1 if the result
   * is non-0, and 0 if the result is 0
   */
  if (dst && !value_is_boolean(dst)) {
    variable_sz_t dst_sz;

    dst_sz = (pic_value_is_w(dst)) ? 1 : value_byte_sz_get(dst);
    if (dst_sz < sz) {
      sz = dst_sz;
    }
  }
  return sz;
}

/*
 * determine the result sign flag based on the promotion rules
 */
flag_t pic_result_flag_get(pfile_t *pf, value_t val1, value_t val2)
{
  variable_def_t rdef;

  rdef = pfile_variable_def_promotion_get(pf, OPERATOR_NULL, val1, val2);
  return variable_def_flag_test(rdef, VARIABLE_DEF_FLAG_SIGNED)
    ? VARIABLE_DEF_FLAG_SIGNED
    : VARIABLE_DEF_FLAG_NONE;
}

boolean_t pic_result_is_signed(pfile_t *pf, value_t val1, 
    value_t val2)
{
  return VARIABLE_DEF_FLAG_SIGNED == pic_result_flag_get(pf, val1, val2);
}

/* 
 * allocate a pic temp variable & assign val to it. The size of the result
 * is less{dst, greater{val1, val2}}
 */
value_t pic_var_temp_get_assign(pfile_t *pf, value_t val, value_t val2,
    value_t dst)
{
  value_t        tmp;
  size_t         sz;

  UNUSED(dst);
  sz = pic_result_sz_get(val, val2, VALUE_NONE/*dst*/);
  tmp = pic_var_temp_get(pf, pic_result_flag_get(pf, val, val2), sz);
  pic_op(pf, OPERATOR_ASSIGN, tmp, val, VALUE_NONE);
  return tmp;
}

/* get the constant byte at offset n. This differs from simply shifting
 * because it preserves the sign bit at higher offsets */
uchar pic_value_const_byte_get(const value_t val, unsigned ofs)
{
  variable_const_t cn;
  unsigned         bits;

  cn = value_const_get(val);
  bits = value_sz_get(val);
  if (!value_is_bit(val)) {
    bits *= 8;
  }
  if (ofs >= value_sz_get(val)) {
    cn = (value_is_signed(val) && variable_const_is_negative(cn, bits))
      ? 0xff
      : 0x00;
  } else {
    cn = variable_const_byte_get(cn, ofs);
  }
  return (uchar) cn;
}

value_t pic_cast(value_t src, variable_sz_t sz)
{
  variable_def_t def;
  value_t        tmp;

  tmp = value_clone(src);
  def = variable_def_sz_set(value_def_get(tmp), sz);
  value_def_set(tmp, def);
  return tmp;
}

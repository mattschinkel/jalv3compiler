/************************************************************
 **
 ** pic_op_bank.c : PIC operator generation -- bank, sign and value coercion
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
 * return the pic sign value appropriately set if val is signed
 * indirect values are expected to be at the MSB entry.
 * If accum is VALUE_NONE, the returned value will be 
 *   pic_var_accum so pic_var_accum_release() must be used!
 */
void pic_value_sign_get_in_w(pfile_t *pf, value_t val)
{
  pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
  if (value_is_signed(val)) {
    pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val, value_sz_get(val) - 1, 7);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 255);
  }
}

value_t pic_value_sign_get(pfile_t *pf, value_t val, value_t accum)
{
  if (value_is_signed(val)) {
    if (VALUE_NONE == accum) {
      accum = pic_var_accum_get(pf);
    }
    pic_value_sign_get_in_w(pf, val);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
  } else {
    accum = VALUE_NONE;
  }
  return accum;
}

/*
 * absolute file address of a value's first byte, for bank-window compares.
 * `value_base_get` is VARIABLE_BASE_UNKNOWN for values that never got a
 * file address; those compare on the sentinel itself (unchanged upstream
 * behaviour) rather than being offset.
 */
static unsigned long pic_val_flat_addr(const value_t val)
{
  unsigned long addr;

  addr = (unsigned long) value_base_get(val);
  if (VARIABLE_BASE_UNKNOWN != value_base_get(val)) {
    addr += (unsigned long) value_const_get(value_baseofs_get(val));
    addr += (unsigned long) (value_bit_offset_get(val) / 8);
  }
  return addr;
}

/*
 * determine if two values reside in the same bank
 */
boolean_t pic_val_bank_same(pfile_t *pf, value_t val1, value_t val2)
{
  boolean_t  rc;

  if (value_is_indirect(val1) 
      || value_is_indirect(val2)
      || pic_value_is_w(val1)
      || pic_value_is_w(val2)
      || value_is_shared(val1)
      || value_is_shared(val2)
      || value_is_const(val1)
      || value_is_const(val2)) {
    rc = BOOLEAN_TRUE;
  } else {
    variable_t var1;
    variable_t var2;

    rc   = BOOLEAN_FALSE;
    var1 = value_variable_get(val1);
    var2 = value_variable_get(val2);

    if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
      /* Both wide cores compare full file addresses inside a bank window.
       * The window is the DEVICE's declared bank size (PIC18 files declare
       * `pragma target bank 0x0100`, classic 0x0080), the same number the
       * allocator and the MOVLB/BSR emitters use. This used to be two
       * hardcoded literals (256 for 16-bit, 128 for 14H) side by side --
       * the exact shape that let `fix_ptr_slot_rt_dispatch_32bit_flat_size`
       * drift. */
      unsigned long window;

      window = (unsigned long) pic_target_bank_size_get(pf);
      rc = ((pic_val_flat_addr(val1) / window)
            == (pic_val_flat_addr(val2) / window));
    } else {
      size_t ii;

      for (ii = 0, rc = BOOLEAN_FALSE; 
           (BOOLEAN_FALSE == rc) && (ii < VARIABLE_MIRROR_CT);
           ii++) {
        variable_base_t base1;
        size_t          jj;

        base1 = variable_base_get(var1, ii);
        if (VARIABLE_BASE_UNKNOWN == base1) {
          /* nothing more to do! */
          break; /* <--- */
        }
        for (jj = 0;
             (BOOLEAN_FALSE == rc) && (jj < VARIABLE_MIRROR_CT);
             jj++) {
          variable_base_t base2;
          unsigned        bank_sz;

          base2 = variable_base_get(var2, jj);
          if (VARIABLE_BASE_UNKNOWN == base2) {
            /* nothing more to do! */
            break; /* <--- */
          }
          bank_sz = pic_target_bank_size_get(pf);
          rc = (((base1 ^ base2) & (bank_sz * 2 | bank_sz)) == 0);
        }
      }
    }
  }
  return rc;
}

/*
 * PIC18 / PIC18-hybrid: true when the MOVLB bank for data byte v1+ofs1
 * equals that for v2+ofs2 (same full-file-address / window as
 * pic_instr_append_daop). Otherwise false (including classic 14-bit).
 */
boolean_t pic_val_movl_bank_equal(pfile_t *pf,
    value_t v1, size_t ofs1, value_t v2, size_t ofs2)
{
  variable_base_t b1;
  variable_base_t b2;
  unsigned long   addr1;
  unsigned long   addr2;
  unsigned long   window;

  if (!pic_is_16bit(pf) && !pic_is_14bit_hybrid(pf)) {
    return BOOLEAN_FALSE;
  }
  if (value_is_indirect(v1) || value_is_indirect(v2)
      || pic_value_is_w(v1) || pic_value_is_w(v2)
      || value_is_const(v1) || value_is_const(v2)
      || value_is_shared(v1) || value_is_shared(v2)) {
    return BOOLEAN_FALSE;
  }
  b1 = value_base_get(v1);
  b2 = value_base_get(v2);
  if (VARIABLE_BASE_UNKNOWN == b1 || VARIABLE_BASE_UNKNOWN == b2) {
    return BOOLEAN_FALSE;
  }
  addr1 = (unsigned long) b1
      + (unsigned long) value_const_get(value_baseofs_get(v1))
      + (unsigned long) (value_bit_offset_get(v1) / 8)
      + (unsigned long) ofs1;
  addr2 = (unsigned long) b2
      + (unsigned long) value_const_get(value_baseofs_get(v2))
      + (unsigned long) (value_bit_offset_get(v2) / 8)
      + (unsigned long) ofs2;
  /* one window rule for both wide cores -- the device's declared bank
   * size, matching the allocator and the emitters (was 256/128 by core). */
  window = (unsigned long) pic_target_bank_size_get(pf);
  return (boolean_t) ((addr1 / window) == (addr2 / window));
}

/*
 * sign extend a value. If the value is indirect, irp:ind are expected
 * to point to the highest byte in the value.
 */
void pic_value_sign_extend(pfile_t *pf, value_t val, 
    variable_sz_t pos, boolean_t is_signed)
{
  if (pos + 1 < value_sz_get(val)) {
    if (is_signed) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val, pos, 7);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 255);
    }
    while (pos + 1 < value_sz_get(val)) {
      pos++;
      if (value_is_indirect(val)) {
        value_t fsr;

        fsr = pic_fsr_get(pf);
        pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, PIC_OPDST_F);
        value_release(fsr);
      }
      pic_instr_append_f(pf, is_signed ? PIC_OPCODE_MOVWF : PIC_OPCODE_CLRF, 
          val, pos);
    }
  }
}

void pic_assign_const(pfile_t *pf, value_t dst,
  value_t val1, value_t val2, variable_const_t n)
{
  value_t        tmp;
  variable_def_t def;

  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      pic_result_flag_get(pf, val1, val2),
      pic_result_sz_get(val1, val2, 
        (value_is_boolean(dst) ? VALUE_NONE : dst)));
  tmp = pfile_constant_get(pf, n, def);
  pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
  value_release(tmp);
}


/* determine if the work must be done in a temporary
 * returns TRUE if work was done in a temporary, FALSE if no
 * nb: in the 16 bit cores, increment and decrement muck with
 *     STATUS<C>. To simplify things, I'll simply disallow
 *     indirect operations for the 16 bit cores
 */
boolean_t pic_work_in_temp(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, unsigned flags)
{
  boolean_t rc;
#define PWIT_FLAG_IND \
  (VARIABLE_FLAG_PTR_LOOKUP \
   | VARIABLE_FLAG_PTR_EEPROM \
   | VARIABLE_FLAG_PTR_FLASH)

  rc = BOOLEAN_TRUE;
  if (pic_is_16bit(pf) && value_is_indirect(dst)) {
    flags |= PIC_WORK_IN_TEMP_FLAG_VALS_DST;
  }
  if ((flags & PIC_WORK_IN_TEMP_FLAG_VAL1)
      && !value_is_const(val1)
      && ((value_is_indirect(val1) 
          && value_is_indirect(dst)
          && ((flags & PIC_WORK_IN_TEMP_FLAG_DST_VAL_EQUAL)
            || !value_is_same(val1, dst)))
        || (value_is_indirect(val1)
          && value_is_indirect(val2)
          && !value_is_same(val1, val2))
        || value_is_bit(val1)
        || value_is_lookup(val1)
        || (value_is_indirect(val1)
          && (variable_flags_get_all(value_variable_get(val1)) 
            & PWIT_FLAG_IND))
        || (value_is_indirect(val1)
          && (pic_is_16bit(pf)))
        || value_is_value(val1)
        || value_is_label(val1))) {
    /* val1 cannot be used as is, get a temp for it instead */
    value_t tmp;

    tmp = pic_var_temp_get_assign(pf, val1, val2, dst);
    pic_op(pf, op, dst, tmp, val2);
    pic_var_temp_release(pf, tmp);
  } else if ((flags & PIC_WORK_IN_TEMP_FLAG_VAL2)
      && !value_is_const(val2)
      && ((value_is_indirect(val2)
          && value_is_indirect(dst)
          && ((flags & PIC_WORK_IN_TEMP_FLAG_DST_VAL_EQUAL)
           || !value_is_same(dst, val2)))
        || value_is_bit(val2)
        || value_is_lookup(val2)
        || (value_is_indirect(val2)
          && (variable_flags_get_all(value_variable_get(val2))
            & PWIT_FLAG_IND))
        || (value_is_indirect(val2)
          && pic_is_16bit(pf))
        || value_is_value(val2)
        || value_is_label(val2))) {
    /* val2 cannot be used as it, get a temp for it instead */
    value_t tmp;

    tmp = pic_var_temp_get_assign(pf, val2, val1, dst);
    pic_op(pf, op, dst, val1, tmp);
    pic_var_temp_release(pf, tmp);
  } else if ((flags & PIC_WORK_IN_TEMP_FLAG_DST)
      && ((value_is_indirect(dst)
          && (variable_flags_get_all(value_variable_get(dst)) & PWIT_FLAG_IND))
        || value_is_float(dst)
        || (value_is_bit(dst)
          || ((flags & PIC_WORK_IN_TEMP_FLAG_DST_VOLATILE)
            && value_is_volatile(dst)))
        || (value_is_indirect(dst)
          && pic_is_16bit(pf)))) {
    value_t tmp;

    tmp = pic_var_temp_get(pf, pic_result_flag_get(pf, val1, val2),
        pic_result_sz_get(val1, val2, dst));
    pic_op(pf, op, tmp, val1, val2);
    pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
    pic_var_temp_release(pf, tmp);
  } else {
    rc = BOOLEAN_FALSE;
  }
  return rc;
#undef PWIT_FLAG_IND
}

/* on exit, Z is set if src is 0, clear otherwise */
void pic_value_is_zero(pfile_t *pf, value_t src)
{
  assert(!value_is_const(src));
  if (value_is_const(src)
      && pic_variable_is_rom_int_const_table(value_variable_get(src))) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, value_const_get(src) ? 1 : 0);
  } else if (pic_value_is_w(src)) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0xff);
  } else if (pic_value_is_dynamic_bit_array_element(src)) {
    (void) pic_dynamic_bit_load_to_w(pf, src);
  } else if (value_is_single_bit(src) && !value_is_indirect(src)) {
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_z");
    pic_instr_append_f(pf, PIC_OPCODE_BTFSS, src, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_z");
  } else if (value_is_bit(src)) {
    value_t tmp;

    tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE,
      value_byte_sz_get(src));
    pic_assign_from_bit(pf, tmp, src);
    pic_value_is_zero(pf, tmp);
    pic_var_temp_release(pf, tmp);
  } else if (value_is_indirect(src) 
    && value_flag_test(src, VALUE_FLAG_POINTER)) {
    value_t tmp;

    tmp = pic_var_temp_get_assign(pf, src, VALUE_NONE, VALUE_NONE);
    pic_value_is_zero(pf, tmp);
    pic_var_temp_release(pf, tmp);
  } else {
    variable_sz_t ii;
    variable_sz_t ipos;

    pic_indirect_setup3(pf, src, VALUE_NONE, VALUE_NONE, 0, &ipos);
    for (ii = 0; ii < value_sz_get(src); ii++) {
      pic_indirect_bump3(pf, src, VALUE_NONE, VALUE_NONE, 
          ii, &ipos);
      pic_instr_append_f_d(pf, 
          (ii) ? PIC_OPCODE_IORWF : PIC_OPCODE_MOVF, src, ii, PIC_OPDST_W);
    }
  }
}

/*
 * convert an integer (any size) to float, leaving the result in
 * _pic_fval1
 */
void pic_integer_to_float(pfile_t *pf, value_t src)
{
  value_t       dst;
  label_t       lbl;
  variable_sz_t sz;

  sz = value_sz_get(src);
  if (value_is_bit(src)) {
    sz = (sz + 7) / 8;
  }

  dst = pic_var_float_conv_get(pf, sz);
  lbl = pic_label_find(pf, value_is_signed(src) 
    ? PIC_LABEL_FLOAT_SCONV 
    : PIC_LABEL_FLOAT_CONV, BOOLEAN_TRUE);
  pic_op(pf, OPERATOR_ASSIGN, dst, src, VALUE_NONE);
  pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
  label_release(lbl);
  pic_var_float_conv_release(pf, dst);
}

void pic_float_to_integer(pfile_t *pf, value_t src)
{
  pic_var_float_t flt;
  label_t         lbl;

  pic_var_float_get(pf, &flt);
  pic_op(pf, OPERATOR_ASSIGN, flt.fval1, src, VALUE_NONE);
  lbl = pic_label_find(pf, PIC_LABEL_FLOAT_TOINT, BOOLEAN_TRUE);
  pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
  label_release(lbl);
  pic_var_float_release(pf, &flt);
}

/************************************************************
 **
 ** pic_op_shift.c : PIC operator generation -- shifts
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
static void pic_assign_shift_left_1_bit(pfile_t *pf, value_t dst, value_t src,
    variable_sz_t sz, unsigned flags);
static void pic_assign_shift_left_4_bits(pfile_t *pf, value_t dst,
  value_t src, unsigned flags);
static void pic_assign_shift_left_n_bytes(pfile_t *pf, value_t dst, 
  value_t src, variable_sz_t sz, variable_sz_t byte_shift,
  pic_assign_shift_n_bytes_action_t action,
  unsigned mask, unsigned flags);
static void pic_assign_shift_right_n_bytes(pfile_t *pf, operator_t op,
    value_t dst, value_t src, variable_const_t byte_shift,
    pic_assign_shift_n_bytes_action_t action, unsigned sign_bit,
    unsigned flags);
static void pic_assign_shift_right_1_bit(pfile_t *pf, operator_t op,
    value_t dst, value_t src, boolean_t set_sign, unsigned flags);
static boolean_t pic_shift_right_resize(pfile_t *pf, operator_t op,
    value_t dst, value_t val1, value_t val2, unsigned flags);
static void pic_assign_shift_right_4_bits(pfile_t *pf,
  operator_t op, value_t dst, value_t src, boolean_t set_sign,
  unsigned flags);

/* assign src to dst, shifting left one bit in the process */
static void pic_assign_shift_left_1_bit(pfile_t *pf, value_t dst, value_t src,
    variable_sz_t sz, unsigned flags)
{
  variable_sz_t ii;
  variable_sz_t ipos;
  pic_opdst_t   opdst;
  value_t       sign;

  assert(VALUE_NONE != dst);
  assert(VALUE_NONE != src);
  assert(!value_is_lookup(src)
    && !value_is_bit(src));
  assert(!value_is_bit(dst));
  assert(!(value_is_indirect(dst) && value_is_indirect(src))
    || value_is_same(dst, src));

  sign  = VALUE_NONE;
  opdst = (value_is_same(dst, src)) ? PIC_OPDST_F : PIC_OPDST_W;
  pic_indirect_setup3(pf, dst, src, VALUE_NONE, 0, &ipos);
  if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
  }
  for (ii = 0; ii < sz; ii++) {
    if (ii == value_sz_get(src)) {
      sign = pic_value_sign_get(pf, src, VALUE_NONE);
      if (VALUE_NONE == sign) {
        /* cannot use clrf as that clears the "C" flag */
        sign = pic_var_accum_get(pf);
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sign, 0);
      }
      src  = sign;
    }
    pic_indirect_bump3(pf, dst, src, VALUE_NONE, ii, &ipos);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, src, 
        (VALUE_NONE == sign) ? ii : 0, opdst);
    if (PIC_OPDST_W == opdst) {
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
    }
  }
  if (VALUE_NONE != sign) {
    pic_var_accum_release(pf, sign);
  }
}

static void pic_assign_shift_left_4_bits(pfile_t *pf, value_t dst,
  value_t src, unsigned flags)
{
  variable_sz_t    ipos;

  pic_indirect_setup3(pf, dst, src, VALUE_NONE, 0, &ipos);
  pic_instr_append_f_d(pf, PIC_OPCODE_SWAPF, src, 0, PIC_OPDST_W);
  if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0xf0);
  }
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
}

/* assign src to dst, shifting left (n) bytes in the process */

/*
 * pf         : pfile handle
 * dst        : destination value
 * src        : source vlaue
 * sz         : result size (will be <= value_size_get(dst))
 * byte_shift : number of bytes to shift
 * action     : *_COPY : all shifted bytes are copied
 *              *_SWAP : the MSB has the nibbles swapped and the
 *                       lower four bits cleared
 * mask       : the the MSB is ANDed with this before stored
 */
static void pic_assign_shift_left_n_bytes(pfile_t *pf, value_t dst, 
  value_t src, variable_sz_t sz, variable_sz_t byte_shift,
  pic_assign_shift_n_bytes_action_t action,
  unsigned mask, unsigned flags)
{
  /* if dst & src are equal and indirect, we need to have one of them
     moved to a temporary */
  if (!pic_work_in_temp(pf, OPERATOR_SHIFT_LEFT, dst, src, VALUE_NONE,
      PIC_WORK_IN_TEMP_FLAG_DST_VAL_EQUAL)) {
    /* we need to work *backward* in case src & dst are the same! */
    /* start by positioning src to byte_ofs */
    variable_sz_t ipos;
    variable_sz_t ii;
    value_t       accum;

    assert(sz > byte_shift);

    pic_indirect_setup3(pf, src, VALUE_NONE, VALUE_NONE,
      sz - byte_shift - 1, &ipos);
    if (sz - byte_shift > value_sz_get(src)) {
      accum = pic_value_sign_get(pf, src, VALUE_NONE);
    } else {
      accum = src;
    }
    if (value_is_indirect(dst)) {
      pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE,
        sz - 1, &ipos);
    }
    ii = sz;
    while (ii--) {
      if (value_is_indirect(dst)) {
        pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
      } 
      if (ii >= byte_shift) {
        if ((accum != src) && (ii - byte_shift < value_sz_get(src))) {
          if (VALUE_NONE != accum) {
            pic_var_accum_release(pf, accum);
          }
          accum = src;
        }
        if (VALUE_NONE == accum) {
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
        } else {
          if ((ii == sz - 1) 
            && (PIC_ASSIGN_SHIFT_N_BYTES_ACTION_SWAP == action)) {
            pic_instr_append_f_d(pf, 
              PIC_OPCODE_SWAPF, 
              accum, 
              (accum == src) ? ii - byte_shift : 0,
              PIC_OPDST_W);
            if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
              pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0xf0);
            }
          } else {
            pic_instr_append_f_d(pf, 
              PIC_OPCODE_MOVF, 
              accum, 
              (accum == src) ? ii - byte_shift : 0,
              PIC_OPDST_W);
          }
          if ((ii == sz - 1) && (255 != mask)) {
            if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
              pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, mask);
            }
          }
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
      } else {
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
      }
      if (ii && value_is_indirect(accum) && ipos) {
        value_t fsr;

        fsr = pic_fsr_get(pf);
        pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0, PIC_OPDST_F);
        value_release(fsr);
        ipos--;
      }
    }
    assert(accum == src);
    if (value_is_indirect(dst) && (sz < value_sz_get(dst))) {
      value_t fsr;

      fsr = pic_fsr_get(pf);
      if (2 == sz) {
        pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, PIC_OPDST_F);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sz - 1);
        pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, fsr, 0, PIC_OPDST_F);
      }
      value_release(fsr);
    }
  }
}

void pic_shift_left(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2, unsigned flags)
{
  variable_sz_t sz;

  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  sz = pic_result_sz_get(val1, val2, dst);

  if (value_is_const(val2)) {
    variable_const_t cn;

    cn = value_const_get(val2);
    if (0 == cn) {
      /* this is a simple assignment */
      pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
    } else if (cn >= 8U * sz) {
      pic_assign_const(pf, dst, val1, val2, 0);
    } else if (!pic_work_in_temp(pf, op, dst, val1, val2, 
      PIC_WORK_IN_TEMP_FLAG_VALS
      | PIC_WORK_IN_TEMP_FLAG_DST)) {
      variable_const_t byte_shift;
      variable_const_t bit_shift;
      value_t          fsr;

      byte_shift = cn / 8;
      bit_shift  = cn & 7;
      fsr        = pic_fsr_get(pf);

      if (byte_shift && !bit_shift) {
        pic_assign_shift_left_n_bytes(pf, dst, val1, sz, byte_shift,
          PIC_ASSIGN_SHIFT_N_BYTES_ACTION_COPY, 0xff, flags);
        pic_value_sign_extend(pf, dst, sz - 1,
            pic_result_is_signed(pf, val1, val2));
      } else if (!byte_shift && (1 == bit_shift)) {
        /* this can be done in one pass */
        pic_assign_shift_left_1_bit(pf, dst, val1, sz, flags);
        pic_value_sign_extend(pf, dst, sz - 1,
            pic_result_is_signed(pf, val1, val2));
      } else if ((4 == cn) && (1 == sz)) {
        /* this can be done in one pass */
        pic_assign_shift_left_4_bits(pf, dst, val1, flags);
        pic_value_sign_extend(pf, dst, sz - 1,
            pic_result_is_signed(pf, val1, val2));
      } else if (!pic_work_in_temp(pf, op, dst, val1, val2,
        PIC_WORK_IN_TEMP_FLAG_DST_VOLATILE)) {
        /* shift bytes and/or bits */
        if ((5 == cn) && (1 == sz)) {
          pic_assign_shift_left_4_bits(pf, dst, val1, flags);
          if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
          }
          pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, 0, PIC_OPDST_F);
        } else if (8U * sz - 2 == cn) {
          variable_sz_t ipos;
          pic_opdst_t   opdst;

          if (value_is_indirect(dst)) {
            pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, sz - 1, 
              &ipos);
          } else if (value_is_indirect(val1)) {
            pic_indirect_setup3(pf, val1, VALUE_NONE, VALUE_NONE, 0, 
              &ipos);
          }
          opdst = (value_is_same(dst, val1)) ? PIC_OPDST_F : PIC_OPDST_W;
          pic_instr_append_f_d(pf, PIC_OPCODE_RRF, val1, 0, opdst);
          if (PIC_OPDST_W == opdst) {
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, sz - 1);
          }
          pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, sz - 1, PIC_OPDST_F);
          pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, sz - 1, PIC_OPDST_W);
          if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0xc0);
          }
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, sz - 1);
          for (ipos = sz - 1; ipos; ipos--) {
            if (value_is_indirect(dst)) {
              pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0, PIC_OPDST_F);
            }
            pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ipos - 1);
          }
          if ((sz > 1) && (value_sz_get(dst) > sz) && value_is_indirect(dst)) {
            /* we'll be sign extending, so move fsr to the end of dst */
            if (sz < 2) {
              pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, PIC_OPDST_F);
            } else {
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sz - 1);
              pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, fsr, 0, PIC_OPDST_F);
            }
          }
        } else if (8U * sz - 1 == cn) {
          variable_sz_t ipos;
          variable_sz_t ii;

          pic_indirect_setup3(pf, dst, val1, VALUE_NONE, 0, &ipos);
          if (value_is_same(dst, val1) || !pic_val_bank_same(pf, dst, val1)) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val1, 0, 0);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0x80);
          }
          for (ii = 0; ii < sz - 1; ii++) {
            pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
            pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii + 1, &ipos);
          }
          if (value_is_same(dst, val1) || !pic_val_bank_same(pf, dst, val1)) {
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
          } else {
            pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val1, 0, 0);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst, ii, 7);
          }
        } else {
          boolean_t always_clear_c;
          unsigned  loop_cost;
          unsigned  unrolled_cost;
          unsigned  bit_shift_real;

          if (cn == 8U * sz - 4) {
            bit_shift_real = 0;
          } else if (cn == 8U * sz - 3) {
            bit_shift_real = 1;
          } else {
            bit_shift_real = bit_shift;
          }

          loop_cost     = 5 + (sz - byte_shift);
          
          unrolled_cost = 1 + ((sz - byte_shift) * bit_shift_real);

          always_clear_c = BOOLEAN_TRUE;
          if (byte_shift) {
            unsigned mask;

            mask = 0xff;
            if (unrolled_cost < loop_cost) {
              if ((cn & 7) == 2) {
                mask = 0x7f;
                always_clear_c = BOOLEAN_FALSE;
              } else if ((cn & 7) == 3) {
                mask = 0x3f;
                always_clear_c = BOOLEAN_FALSE;
              }
            }
            if (always_clear_c) {
              unrolled_cost = (1 + (sz - byte_shift)) * bit_shift_real;
            } else {
              unrolled_cost++;
            }
            pic_assign_shift_left_n_bytes(pf, dst, val1, sz, byte_shift,
              cn >= (8U * sz - 4)
              ? PIC_ASSIGN_SHIFT_N_BYTES_ACTION_SWAP
              : PIC_ASSIGN_SHIFT_N_BYTES_ACTION_COPY, mask, flags);
          } else if (bit_shift && !value_is_same(dst, val1)) {
            pic_assign_shift_left_1_bit(pf, dst, val1, sz, flags);
            bit_shift--;
          }
          if (cn != 8U * sz - 4) {
            if (cn == 8U * sz - 3) {
              bit_shift = 1;
            }
            if (!pfile_flag_test(pf, PFILE_FLAG_CMD_SPEED)
              && (loop_cost < unrolled_cost)) {
              value_t       loop;
              label_t       top;
              variable_sz_t ipos;
              variable_sz_t ii;

              loop = pic_var_accum_get(pf);
              top  = pfile_label_alloc(pf, 0);
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, bit_shift);
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, loop, 0);
              pic_instr_append_label(pf, top);
              pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, byte_shift,
                &ipos);
              if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
              }
              for (ii = byte_shift; ii < sz; ii++) {
                pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
                pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, ii, PIC_OPDST_F);
              }
              pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, loop, 0, 
                PIC_OPDST_F);
              pic_instr_append_n(pf, PIC_OPCODE_GOTO, top);
              label_release(top);
              pic_var_accum_release(pf, loop);
            } else {
              if (!always_clear_c) {
                if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
                  pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", 
                    "_c");
                }
              }
              while (bit_shift--) {
                variable_sz_t ii;
                variable_sz_t ipos;

                pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, byte_shift,
                  &ipos);
                if (always_clear_c) {
                  if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
                    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", 
                      "_c");
                  }
                }
                for (ii = byte_shift; ii < sz; ii++) {
                  pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, 
                    &ipos);
                  pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, ii, 
                    PIC_OPDST_F);
                }
              }
            }
          }
        }
        pic_value_sign_extend(pf, dst, sz - 1,
            pic_result_is_signed(pf, val1, val2));
      }
      value_release(fsr);
    }
  } else if (!pic_work_in_temp(pf, op, dst, val1, val2,
    PIC_WORK_IN_TEMP_FLAG_DST
    | PIC_WORK_IN_TEMP_FLAG_DST_VOLATILE)) {
      /* nothing to do */
    if (!pic_is_12bit(pf)
        && value_is_same(dst, val1) 
        && !value_is_indirect(dst)) {
      /* minor optimization, val2 goes into W so we don't need a temp */
      variable_sz_t ipos;
      variable_sz_t ii;
      label_t       lbl_top;
      label_t       lbl_test;

      lbl_top  = pfile_label_alloc(pf, 0);
      lbl_test = pfile_label_alloc(pf, 0);

      pic_indirect_setup3(pf, VALUE_NONE, VALUE_NONE, val2, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val2, 0, PIC_OPDST_W);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_test);
      pic_instr_append_label(pf, lbl_top);
      if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
      }
      for (ii = 0; ii < sz; ii++) {
        pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, ii, PIC_OPDST_F);
      }
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 255);
      pic_instr_append_label(pf, lbl_test);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_top);

      label_release(lbl_test);
      label_release(lbl_top);
    } else if (pic_result_sz_get(val1, val2, dst) < value_sz_get(dst)) {
      variable_def_t def;
      variable_def_t def_fixed;

      def = value_def_get(dst);
      def_fixed = variable_def_alloc(0, 
          variable_def_type_get(def),
          variable_def_flags_get_all(def),
          pic_result_sz_get(val1, val2, dst));
      value_def_set(dst, def_fixed);
      pic_shift_left(pf, op, dst, val1, val2, flags);
      value_def_set(dst, def);
      pic_value_sign_extend(pf, dst, pic_result_sz_get(val1, val2, dst) - 1,
          pic_result_is_signed(pf, val1, val2));
    } else {
      /* do the shift, one bit at a time */
      value_t       accum;
      label_t       lbl_top;
      label_t       lbl_test;
      variable_sz_t ipos;
      variable_sz_t ii;
      value_t       fsr;

      fsr      = pic_fsr_get(pf);
      lbl_top  = pfile_label_alloc(pf, 0);
      lbl_test = pfile_label_alloc(pf, 0);

      accum = pic_var_accum_get(pf);
      pic_op(pf, OPERATOR_ASSIGN, accum, val2, VALUE_NONE);

      if (!value_is_same(dst, val1)) {
        pic_op(pf, OPERATOR_ASSIGN, dst, val1, VALUE_NONE);
      }
      /* start dst at the end */
      pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, sz - 1, &ipos);
      if (!value_is_same(dst, val1) || value_is_indirect(dst)) {
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, accum, 0, PIC_OPDST_W);
      }
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_test);
      pic_instr_append_label(pf, lbl_top);
      if (value_is_indirect(dst)) {
        /* adjust dst to point to the beginning */
        if ((sz > 1) && value_is_indirect(dst)) {
          if (1 == sz - 1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0, PIC_OPDST_F);
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sz - 1);
            pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, fsr, 0, PIC_OPDST_F);
          }
        }
      }
      ipos = 0;
      if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
      }
      for (ii = 0; ii < sz; ii++) {
        pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
        pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, ii, PIC_OPDST_F);
      }
      pic_instr_append_f_d(pf, PIC_OPCODE_DECF, accum, 0, PIC_OPDST_F);
      pic_instr_append_label(pf, lbl_test);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_top);
      pic_value_sign_extend(pf, dst, ii - 1, 
          pic_result_is_signed(pf, val1, val2));

      label_release(lbl_test);
      label_release(lbl_top);
      value_release(fsr);
      pic_var_accum_release(pf, accum);
    }
  }
}

void pic_op_shift_left(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  pic_shift_left(pf, op, dst, val1, val2, PIC_SHIFT_FLAG_MASK_BITS);
}

/* shift right 4 bits:
   swapf  f, w
   andlw  0x0f
   movwf  f
   * [
     swapf  f + n, w
     movwf  f + n
     andlw  0xf0
     xorwf  f + n, f
     iorwf  f + n - 1, f
   ]
*/

static void pic_assign_shift_right_n_bytes(pfile_t *pf, operator_t op,
    value_t dst, value_t src, variable_const_t byte_shift,
    pic_assign_shift_n_bytes_action_t action, unsigned sign_bit,
    unsigned flags)
{
  variable_sz_t ii;
  variable_sz_t ipos;
  boolean_t     fix_sign;

  fix_sign = BOOLEAN_FALSE;
  if (value_is_indirect(dst)) {
    pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE,
        0, &ipos);
  } else if (value_is_indirect(src)) {
    pic_indirect_setup3(pf, src, VALUE_NONE, VALUE_NONE,
        byte_shift, &ipos);
  }
  for (ii = 0; ii + byte_shift < value_sz_get(src); ii++) {
    if (value_is_indirect(dst)) {
      pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
    } else if (value_is_indirect(src)) {
      pic_indirect_bump3(pf, src, VALUE_NONE, VALUE_NONE, ii + byte_shift, 
          &ipos);
    }
    if ((PIC_ASSIGN_SHIFT_N_BYTES_ACTION_SWAP == action)
      && (ii + byte_shift + 1 == value_sz_get(src))) {
      pic_instr_append_f_d(pf, PIC_OPCODE_SWAPF, src, ii + byte_shift,
        PIC_OPDST_W);
      if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0x0f);
      }
      fix_sign = BOOLEAN_TRUE;
    } else {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, ii + byte_shift, 
          PIC_OPDST_W);
    }
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
    if (fix_sign &&  (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0xf0);
      pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, ii, 3);
      pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, dst, ii, PIC_OPDST_F);
    }
  }
  if (ii < value_sz_get(dst)) {
    if (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src, ii + byte_shift - 1,
          sign_bit);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0xff);
      while (ii < value_sz_get(dst)) {
        pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        ii++;
      }
    } else {
      while (ii < value_sz_get(dst)) {
        pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
        ii++;
      }
    }
  }
}

static void pic_assign_shift_right_1_bit(pfile_t *pf, operator_t op,
    value_t dst, value_t src, boolean_t set_sign, unsigned flags)
{
  variable_sz_t ipos;
  variable_sz_t ii;
  pic_opdst_t   opdst;
  boolean_t     is_first;

  opdst = (value_is_same(dst, src)) ? PIC_OPDST_F : PIC_OPDST_W;
  pic_indirect_setup3(pf, dst, src, VALUE_NONE, value_sz_get(src) - 1, &ipos);
  ii = value_sz_get(src);
  if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
  }
  is_first = set_sign && (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op);
  while (ii--) {
    pic_indirect_bump3(pf, dst, src, VALUE_NONE, ii, &ipos);
    if (is_first) {
      is_first = BOOLEAN_FALSE;
      pic_instr_append_f_d(pf, PIC_OPCODE_RRF, src, ii, PIC_OPDST_W);
      pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src, ii, 7);
      pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW, 0x80);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
    } else {
      pic_instr_append_f_d(pf, PIC_OPCODE_RRF, src, ii, opdst);
      if (PIC_OPDST_W == opdst) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
      }
    }
  }
}

static boolean_t pic_shift_right_resize(pfile_t *pf, operator_t op,
    value_t dst, value_t val1, value_t val2, unsigned flags)
{
  boolean_t rc;

  rc = BOOLEAN_FALSE;
  if ((value_sz_get(val1) < value_sz_get(val2)) && !value_is_universal(val2)) {
    value_t tmp;

    tmp = pic_var_temp_get(pf,
        value_is_signed(val1) 
        ? VARIABLE_DEF_FLAG_SIGNED : VARIABLE_DEF_FLAG_NONE,
        value_sz_get(val2));
    pic_op(pf, OPERATOR_ASSIGN, tmp, val1, VALUE_NONE);
    pic_shift_right(pf, op, dst, tmp, val2, flags);
    pic_var_temp_release(pf, tmp);
    rc = BOOLEAN_TRUE;
  } else if (value_is_bit(dst)
      || (value_sz_get(dst) < pic_result_sz_get(val1, val2, VALUE_NONE))) {
    value_t tmp;

    tmp = pic_var_temp_get(pf, pic_result_flag_get(pf, val1, val2),
      pic_result_sz_get(val1, val2, VALUE_NONE));
    pic_shift_right(pf, op, tmp, val1, val2, flags);
    pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
    pic_var_temp_release(pf, tmp);
    rc = BOOLEAN_TRUE;
  } else {
    variable_sz_t  sz;

    sz = pic_result_sz_get(val1, val2, VALUE_NONE);
    if (value_sz_get(dst) > sz) {
      /* only work in the bottom part of dst, then sign extend */
      value_t        tmp;
      variable_def_t def;
      variable_sz_t  ipos;

      tmp = value_clone(dst);
      def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
          pic_result_flag_get(pf, val1, val2), sz);
      value_def_set(tmp, def);
      pic_shift_right(pf, op, tmp, val1, val2, flags);
      pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, sz - 1, &ipos);
      pic_value_sign_extend(pf, dst, sz - 1,
          pic_result_is_signed(pf, val1, val2));
      value_release(tmp);
      rc = BOOLEAN_TRUE;
    }
  }
  return rc;
}

/* note: op can be OPERATOR_SHIFT_RIGHT_ARITHMETIC which preserves the
 *       high bit during shifting
 */
static void pic_assign_shift_right_4_bits(pfile_t *pf,
  operator_t op, value_t dst, value_t src, boolean_t set_sign,
  unsigned flags)
{
  /* this can be done in one pass */
  variable_sz_t ipos;

  pic_indirect_setup3(pf, dst, src, VALUE_NONE, 0, &ipos);
  pic_instr_append_f_d(pf, PIC_OPCODE_SWAPF, src, 0, PIC_OPDST_W);
  if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0x0f);
  }
  if (set_sign && (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)) {
    pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src, 0, 7);
    pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW, 0xf0);
  }
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
}

void pic_shift_right(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2, unsigned flags)
{
  variable_sz_t sz;

  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(val1));
  assert(!pic_value_is_w(val2));

  /* the only size that matters is val1
   * if dst is smaller, right shifting could end up shifting into dst
   */

  sz = pic_result_sz_get(val1, val2, VALUE_NONE);
  if (value_is_const(val2) && !value_const_get(val2)) {
    /* shifting 0 = 0 */
    pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
  } else if ((OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)
      && ((value_is_single_bit(val1) && (value_sz_get(val1) >= sz * 8))
        || (value_is_const(val1)
          && (value_const_get(val1) == (variable_const_t) -1)))) {
    /* arithmetic shifting of -1 == -1 */
    /* arithmetic shifting of a single bit = assignment */
    pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
  } else if (value_is_const(val2)) {
    variable_const_t cn;

    cn = value_const_get(val2);
    if (0 == cn) {
      /* shouldn't get here */
      pic_unary_fixup(pf, OPERATOR_ASSIGN, dst, val1, val2);
    } else if (cn >= 8U * sz) {
      if (!pic_work_in_temp(pf, op, dst, val1, val2,
            PIC_WORK_IN_TEMP_FLAG_DST)) {
        /* dst is either all 0x00s or all 0xffs */
        if ((OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)) {
          variable_sz_t ii;
          variable_sz_t ipos;

          if (value_is_signed(val1) || (value_sz_get(val1) == sz)) {
            pic_indirect_setup3(pf, val1, VALUE_NONE, VALUE_NONE, 
                value_sz_get(val1) - 1, &ipos);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0x00);
            if (value_is_bit(val1)) {
              pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC,
                  val1, 0, value_sz_get(val1) - 1);
            } else {
              pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val1,
                  value_sz_get(val1) - 1, 7);
            }
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0xff);
            if (value_is_indirect(dst)) {
              value_t accum;

              accum = pic_var_accum_get(pf);
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
              pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
              pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, accum, 0, PIC_OPDST_W);
              pic_var_accum_release(pf, accum);
            }
            if (pic_result_is_signed(pf, val1, val2)) {
              sz = value_sz_get(dst);
            }
            for (ii = 0; ii < sz; ii++) {
              pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
            }
          } else {
            pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
            for (ii = 0; ii < sz; ii++) {
              pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
            }
          }
          sz = value_sz_get(dst);
          while (ii < sz) {
            pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
            pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
            ii++;
          }
        } else {
          variable_sz_t ii;
          variable_sz_t ipos;

          pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
          sz = value_sz_get(dst);
          for (ii = 0; ii < sz; ii++) {
            pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
            pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
          }
        }
      }
    } else if (!pic_shift_right_resize(pf, op, dst, val1, val2, flags)
      && !pic_work_in_temp(pf, op, dst, val1, val2,
          PIC_WORK_IN_TEMP_FLAG_VAL1)) {
      variable_sz_t bit_shift;
      variable_sz_t byte_shift;
      value_t       fsr;

      byte_shift = cn / 8;
      bit_shift  = cn & 0x07;
      fsr        = pic_fsr_get(pf);

      if (byte_shift && !bit_shift) {
        /* this can be done in one pass */
        variable_sz_t ipos;

        pic_assign_shift_right_n_bytes(pf, op, dst, val1, byte_shift,
          PIC_ASSIGN_SHIFT_N_BYTES_ACTION_COPY, 7, flags);
        pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 
            value_sz_get(val1) - 1, &ipos);
        pic_value_sign_extend(pf, dst, value_sz_get(val1) - 1,
            pic_result_is_signed(pf, val1, val2));
      } else if (!byte_shift && (1 == bit_shift)) {
        /* this can be done in one pass */
        variable_sz_t ipos;

        pic_assign_shift_right_1_bit(pf, op, dst, val1, BOOLEAN_TRUE,
          flags);
        pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 
            value_sz_get(val1) - 1, &ipos);
        pic_value_sign_extend(pf, dst, value_sz_get(val1) - 1,
            pic_result_is_signed(pf, val1, val2));
      } else if (!byte_shift && (4 == bit_shift) && (1 == sz)) {
        pic_assign_shift_right_4_bits(pf, op, dst, val1, BOOLEAN_TRUE,
          flags);
      } else if (!pic_work_in_temp(pf, op, dst, val1, val2,
            PIC_WORK_IN_TEMP_FLAG_DST_VOLATILE)) {
        if ((5 == cn) && (1 == sz)) {
          pic_assign_shift_right_4_bits(pf, op, dst, val1,
            BOOLEAN_FALSE, flags);
          if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
          }
          if (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op) {
            pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, 0, PIC_OPDST_W);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, 0, 3);
            pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW, (unsigned char) ~3);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, 0, PIC_OPDST_F);
          }
        } else if (8U * sz - 2 == cn) {
          variable_sz_t ipos;
          pic_opdst_t   opdst;

          if (value_is_indirect(dst)) {
            pic_indirect_setup(pf, dst, 0);
          } else if (value_is_indirect(val1)) {
            pic_indirect_setup(pf, val1, sz - 1);
          } 
          opdst = (value_is_same(dst, val1)) ? PIC_OPDST_F : PIC_OPDST_W;
          pic_instr_append_f_d(pf, PIC_OPCODE_RLF, val1, sz - 1, opdst);
          if (PIC_OPDST_W == opdst) {
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
          }
          pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, 0, PIC_OPDST_F);
          pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, 0, PIC_OPDST_W);
          if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0x03);
          }
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
          if (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, 0, 1);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (unsigned char) ~3);
            pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, dst, 0, PIC_OPDST_F);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, 0, 1);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0xff);
            for (ipos = 1; ipos < sz; ipos++) {
              if (value_is_indirect(dst)) {
                pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, 
                  PIC_OPDST_F);
              }
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ipos);
            }
          } else {
            for (ipos = 1; ipos < sz; ipos++) {
              if (value_is_indirect(dst)) {
                pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, 
                  PIC_OPDST_F);
              }
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ipos);
            }
          }
        } else if (8U * sz - 1 == cn) {
          if (OPERATOR_SHIFT_RIGHT == op) {
            variable_sz_t ii;
            pic_opdst_t   opdst;

            if (value_is_indirect(dst)) {
              pic_indirect_setup(pf, dst, 0);
            } else if (value_is_indirect(val1)) {
              pic_indirect_setup(pf, val1, sz - 1);
            } 
            opdst = (value_is_same(dst, val1)) ? PIC_OPDST_F : PIC_OPDST_W;
            pic_instr_append_f_d(pf, PIC_OPCODE_RLF, val1, sz - 1, opdst);
            pic_instr_append_f_d(pf, PIC_OPCODE_RLF, dst, 0, PIC_OPDST_W);
            if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
              pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0x01);
            }
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
            for (ii = 1; ii < sz; ii++) {
              if (value_is_indirect(dst)) {
                pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, 
                  PIC_OPDST_F);
              }
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
            }
          } else {
            variable_sz_t ii;

            if (value_is_indirect(dst)) {
              pic_indirect_setup(pf, dst, 0);
            } else if (value_is_indirect(val1)) {
              pic_indirect_setup(pf, val1, sz - 1);
            } 

            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
            pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val1, sz - 1, 7);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 255);
            for (ii = 0; ii < sz; ii++) {
              if (ii && value_is_indirect(dst)) {
                pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, 
                  PIC_OPDST_F);
              }
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
            }
          }
        } else if ((8U * sz - 4 == cn) || (8U * sz - 3 == cn)) {
          pic_assign_shift_right_n_bytes(pf, op, dst, val1, byte_shift,
            PIC_ASSIGN_SHIFT_N_BYTES_ACTION_SWAP, 3, flags);
          if (8U * sz - 3 == cn) {
            if (value_is_indirect(dst)) {
              if (2 == sz) {
                pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0, PIC_OPDST_F);
              } else {
                pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
                  byte_shift);
                pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, fsr, 0, 
                  PIC_OPDST_F);
              }
            }
            if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
              pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
            }
            if (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op) {
              pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, 0, 7);
              pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_c");
            }
            assert(sz - byte_shift == 1);
            pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, 0, 
              PIC_OPDST_F);
          }
        } else {
          unsigned cost_loop;
          unsigned cost_unrolled;
          unsigned fix;

          fix = 0;
          if (byte_shift) {
            pic_assign_shift_right_n_bytes(pf, op, dst, val1, byte_shift,
              PIC_ASSIGN_SHIFT_N_BYTES_ACTION_COPY, 7, flags);
          } else if (bit_shift 
              && !value_is_same(dst, val1)) {
            pic_assign_shift_right_1_bit(pf, op, dst, val1, BOOLEAN_TRUE, 
              flags);
            bit_shift--;
            fix = 1;
          }
          pic_value_sign_extend(pf, dst, value_sz_get(val1) - 1,
              pic_result_is_signed(pf, val1, val2)
              || (value_is_signed(val1) 
                && (value_sz_get(val2) > value_sz_get(val1))));
          cost_loop     = (sz - byte_shift) * 2 + 4;
          cost_unrolled = ((sz - byte_shift) + 1) * bit_shift;

          if (pfile_flag_test(pf, PFILE_FLAG_CMD_SPEED)
            || (cost_unrolled <= cost_loop)) {
            variable_sz_t bit;

            for (bit = 0; bit < bit_shift; bit++) {
              variable_sz_t ii;

              /* we're at 0 and need to move back to the end */
              if (value_is_indirect(dst) && (sz != 1)) {
                if (byte_shift) {
                  /* if we've byte-shifted and are indirect, we're positioned
                   * at the end of the indirect already, so set byte_shift
                   * to zero */
                  byte_shift = 0;
                } else {
                  pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sz - 1);
                  pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, fsr, 0,
                      PIC_OPDST_F);
                }
              }
              if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
              }
              ii = sz - byte_shift;
              while (ii) {
                if ((ii != sz) && value_is_indirect(dst)) {
                  pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0,
                      PIC_OPDST_F);
                }
                ii--;
                if ((bit + 1 == bit_shift) 
                  && (ii + 1 == sz - byte_shift)
                  && (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)) {
                  /* set the high bits if necessary */
                  unsigned char mask;

                  pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, ii,
                    PIC_OPDST_W);
                  pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, ii,
                    7 - bit - fix);
                  mask = 0x00; /* fix compiler warning */
                  switch (bit + fix) {
                    case 0: mask = 0x80; break;
                    case 1: mask = 0xc0; break;
                    case 2: mask = 0xe0; break;
                    case 3: mask = 0xf0; break;
                    case 4: mask = 0xf8; break;
                    case 5: mask = 0xfc; break;
                    case 6: mask = 0xfe; break;
                    case 7: mask = 0xff; break;
                  }
                  pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW, mask);
                  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
                } else {
                  pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, ii, 
                    PIC_OPDST_F);
                }
              }
            }
          } else {
            value_t       loop;
            label_t       top;
            variable_sz_t ipos;
            variable_sz_t ii;

            loop = pic_var_temp_get(pf, VARIABLE_FLAG_NONE, 1);
            top = pfile_label_alloc(pf, 0);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, bit_shift);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, loop, 0);
            pic_instr_append_label(pf, top);
            pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 
              sz - byte_shift, &ipos);
            if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
              pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
            }
            if (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op) {
              pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, sz - 1, 7);
              pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF,"_status", "_c");
            }
            for (ii = sz - byte_shift; ii; ii--) {
              pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii - 1, 
                &ipos);
              pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, ii - 1, 
                PIC_OPDST_F);
            }
            pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, loop, 0, PIC_OPDST_F);
            pic_instr_append_n(pf, PIC_OPCODE_GOTO, top);
            label_release(top);
            pic_var_temp_release(pf, loop);
          }
        }
      }
      value_release(fsr);
    }
  } else if (!pic_shift_right_resize(pf, op, dst, val1, val2, flags)
      && !pic_work_in_temp(pf, op, dst, val1, val2,
        PIC_WORK_IN_TEMP_FLAG_VAL1
        | PIC_WORK_IN_TEMP_FLAG_DST
        | PIC_WORK_IN_TEMP_FLAG_DST_VOLATILE)) {
    value_t       accum;
    label_t       lbl_top;
    label_t       lbl_end;
    value_t       c1;
    variable_sz_t ipos;
    variable_sz_t ii;

    lbl_top = pfile_label_alloc(pf, 0);
    lbl_end = pfile_label_alloc(pf, 0);

    c1 = pfile_constant_get(pf, 1, VARIABLE_DEF_NONE);
    accum = pic_var_accum_get(pf);
    pic_op(pf, OPERATOR_ASSIGN, accum, val2, VALUE_NONE);
    pic_op(pf, OPERATOR_ASSIGN, dst, val1, VALUE_NONE);
    if (!value_is_same(dst, val1)) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, accum, 0, PIC_OPDST_F);
    }
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_end);
    pic_instr_append_label(pf, lbl_top);
    pic_indirect_setup3(pf, val1, VALUE_NONE, dst, sz - 1, &ipos);
    if (flags & PIC_SHIFT_FLAG_MASK_BITS) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
    }
    for (ii = sz; ii; ii--) {
      pic_indirect_bump3(pf, val1, VALUE_NONE, dst, ii - 1, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_RRF, dst, ii - 1, PIC_OPDST_F);
      if ((ii == sz) && (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)) {
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, dst, ii - 1, 6);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst, ii - 1, 7);
      }
    }
    pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, accum, 0, PIC_OPDST_F);
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_top);
    pic_instr_append_label(pf, lbl_end);

    pic_var_accum_release(pf, accum);
    value_release(c1);
    label_release(lbl_end);
    label_release(lbl_top);
  }
}

void pic_op_shift_right(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  pic_shift_right(pf, op, dst, val1, val2, PIC_SHIFT_FLAG_MASK_BITS);
}

/* set dst to 0 if val is 0, 1 if val != 0 */
void pic_op_logical_or_not(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  value_t res_tmp;

  UNUSED(val2);

  assert(!pic_value_is_w(src));

  if (value_is_single_bit(dst)
      && pic_value_is_dynamic_bit_array_element(dst)) {
    /* A runtime-indexed bit-array element destination cannot take the
     * raw BSF/BCF forms below (they hardcode bit 0 of an INDIRECT
     * pseudo with no FSR setup): evaluate the 0/1 result into a temp
     * and go through the dynamic-bit store. */
    res_tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
    if (res_tmp) {
      pic_value_is_zero(pf, src); /* Z is set if src is zero */
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (OPERATOR_LOGICAL == op) ? 0 : 1);
      pic_instr_append_reg_flag(pf, (OPERATOR_LOGICAL == op)
          ? PIC_OPCODE_BTFSC : PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (OPERATOR_LOGICAL == op) ? 1 : 0);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, res_tmp, 0);
      pic_assign_to_bit(pf, dst, res_tmp);
      pic_var_temp_release(pf, res_tmp);
      return;
    }
  }

  if (value_is_single_bit(src)
      && !value_is_indirect(src)
      && !pic_value_is_dynamic_bit_array_element(src)) {
    if (value_is_single_bit(dst)) {
      pic_opcode_t opcode;

      opcode = (OPERATOR_LOGICAL == op) ? PIC_OPCODE_BTFSC : PIC_OPCODE_BTFSS;

      if ((OPERATOR_LOGICAL == op) && value_is_same(dst, src)) {
        /* this is a no-op */
      } else if (value_is_same(dst, src) 
          || value_is_volatile(dst)
          || !pic_val_bank_same(pf, dst, src)) {
        label_t lbl_set;
        label_t lbl_done;

        lbl_set  = pfile_label_alloc(pf, 0);
        lbl_done = pfile_label_alloc(pf, 0);
        pic_instr_append_f(pf, opcode, src, 0);
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
        pic_instr_append_f(pf, opcode, src, 0);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
      }
    } else {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_instr_append_f(pf, (OPERATOR_LOGICAL == op)
          ? PIC_OPCODE_BTFSC
          : PIC_OPCODE_BTFSS,
          src, 0);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
      pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
    }
  } else {
    pic_value_is_zero(pf, src); /* Z is set if src is zero */
    if (value_is_single_bit(dst)) {
      if (value_is_volatile(dst)) {
        label_t lbl_clr;
        label_t lbl_done;

        lbl_clr = pfile_label_alloc(pf, 0);
        lbl_done = pfile_label_alloc(pf, 0);
        pic_instr_append_reg_flag(pf, (OPERATOR_LOGICAL == op)
            ? PIC_OPCODE_BTFSC : PIC_OPCODE_BTFSS, "_status", "_z");
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clr);
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_label(pf, lbl_clr);
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
        pic_instr_append_label(pf, lbl_done);
        label_release(lbl_done);
        label_release(lbl_clr);
      } else {
        pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        pic_instr_append_reg_flag(pf, (OPERATOR_LOGICAL == op)
            ? PIC_OPCODE_BTFSC : PIC_OPCODE_BTFSS, "_status", "_z");
        pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
      }
    } else if (VALUE_NONE != dst) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
      pic_instr_append_reg_flag(pf, (OPERATOR_LOGICAL == op)
          ? PIC_OPCODE_BTFSC : PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_assign_from_w(pf, dst, BOOLEAN_FALSE);
    }
  }
}

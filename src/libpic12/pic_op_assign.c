/************************************************************
 **
 ** pic_op_assign.c : PIC operator generation -- assignment emitters
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
static void pic_assign_from_const(pfile_t *pf, value_t dst, value_t src);
static void pic_assign_from_lookup(pfile_t *pf, value_t dst,
    value_t src);
static void pic_assign_from_label_offset(pfile_t *pf, value_t dst, label_t lbl,
    variable_const_t offset);
static void pic_assign_from_label2(pfile_t *pf, value_t dst, label_t lbl);
static void pic_assign_from_value(pfile_t *pf, value_t dst, value_t src);
static void pic_assign_address_of_lvalue_to_pointer(pfile_t *pf, value_t dst,
    value_t src);
static boolean_t pic_assign_from_whereis_const(pfile_t *pf, value_t dst,
    value_t src);
static void pic_assign_from_label(pfile_t *pf, value_t dst, value_t src);
static void pic_assign_from_function(pfile_t *pf, value_t dst,
  value_t src);

/* dst is not bit, src is const */
static void pic_assign_from_const(pfile_t *pf, value_t dst, value_t src)
{
  variable_sz_t sz;
  variable_sz_t ii;
  variable_sz_t ipos;
  variable_sz_t ofs;
  variable_sz_t limit;
  variable_sz_t src_sz;
  uchar         last;
  const char   *data;

  sz     = (pic_value_is_w(dst) ? 1 : value_sz_get(dst));
  src_sz = value_sz_get(src);
  last   = 0;
  ofs    = 0;
  pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
  data = variable_data_get(value_variable_get(src));
  limit = variable_sz_get(value_variable_get(src));
  /* If data is NULL, neither offset nor limit matter./ */
  if (data) {
    if (value_baseofs_get(src)) {
         ofs = value_const_get(value_baseofs_get(src));
    }
  } else {
    src_sz = 0;
    limit  = 0;
  }
  for (ii = 0; ii < sz; ii++) {
    uchar ch;

    pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
    /* note: data can be 0 if a variable is later determined to be const */
    if ((ii < src_sz) && (ofs + ii < limit)) {
      ch = data[ofs + ii];
    } else {
      ch = last;
      if ((ii == src_sz) || (ofs + ii == limit)) {
        if (value_is_signed(src)) {
          ch = (last & 0x80) ? 0xff : 0x00;
        } else {
          ch = 0;
        }
      }
    }
    if ((ii == 0) || (ch != last)) {
      if (ch || pic_value_is_w(dst)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, ch);
      }
      last = ch;
    }
    if (!pic_value_is_w(dst)) {
      pic_instr_append_f(pf, (ch) ? PIC_OPCODE_MOVWF : PIC_OPCODE_CLRF, dst, 
          ii);
    }
  }
}

/* dst is anything, src is lookup */
static void pic_assign_from_lookup(pfile_t *pf, value_t dst,
    value_t src)
{
  if (value_is_bit(dst)) {
    /* assign src to a temporary, then the temporary to dst */
    value_t       tmp;
    variable_sz_t sz;

    sz = value_sz_get(src);
    if (sz > value_byte_sz_get(dst)) {
      sz = value_byte_sz_get(dst);
    }
    tmp = pic_var_temp_get(pf, pic_result_flag_get(pf, src, VALUE_NONE), sz);
    pic_op(pf, OPERATOR_ASSIGN, tmp, src, VALUE_NONE);
    pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
    pic_var_temp_release(pf, tmp);
  } else {
    variable_sz_t  ii;
    variable_sz_t  ipos;
    variable_sz_t  sz;
    value_t        tmp;
    variable_def_t def;
    value_t        baseofs;
    label_t        lbl;
    value_t         tablat;
    variable_def_member_t mbr;

    def = variable_def_get(value_variable_get(src));
    mbr = variable_def_member_get(def);
    baseofs = value_baseofs_get(src);
    tmp = VALUE_NONE;

    sz = pic_result_sz_get(src, VALUE_NONE, dst);
    pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
    if (pic_is_16bit(pf)) {
      pic_code_t code;
      value_t    tblptr;

      lbl = pic_lookup_label_find(pf, value_variable_get(src), 
          PIC_LOOKUP_LABEL_FIND_FLAG_DATA);
      tblptr = pfile_value_find(pf, PFILE_LOG_ERR, "_tblptr");
      pic_stvar_tblptr_mark(pf);
      for (ii = 0; ii < 3; ii++) {
        code   = pic_instr_append(pf, PIC_OPCODE_MOVLW);
        pic_code_brdst_set(code, lbl);
        pic_code_ofs_set(code, ii);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tblptr, ii);
      }
      if (baseofs) {
        pic_op(pf, OPERATOR_ADD, tblptr, baseofs, VALUE_NONE);
      }
      value_release(tblptr);
      tablat = pfile_value_find(pf, PFILE_LOG_ERR, "_tablat");
    } else {
      lbl = pic_lookup_label_find(pf, value_variable_get(src), 
          PIC_LOOKUP_LABEL_FIND_FLAG_NONE);
      tablat = VALUE_NONE;
      if (variable_def_member_sz_get(mbr) * variable_def_member_ct_get(mbr)
          > 255) {
        /* need to pass LSB in _pic_loop, MSB in W */
        tmp = pic_var_loop_get(pf);
        pic_lookup_index_load_w(pf, baseofs);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 0);
        if (value_byte_sz_get(baseofs) > 1) {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, baseofs, 1, PIC_OPDST_W);
        } else {
          /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
        }
      } else {
        /* need to find a way to set PCLATH to the high byte of the dst
         * nb: not needed on the 14bit hybrid as pclath will be set correctly
         *     by `movlp' */
        if (!pic_is_14bit_hybrid(pf)) {
          pic_code_t code;

          code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
          pic_code_brdst_set(code, lbl);
          pic_code_ofs_set(code, 1);
          pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_pclath");
        }
        pic_lookup_index_load_w(pf, baseofs);
        tmp = baseofs;
      }
    }
    for (ii = 0; ii < sz; ii++) {
      pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
      if (pic_is_16bit(pf)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_TBLRD, PIC_TBLPTR_CHANGE_POST_INC);
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tablat, 0, PIC_OPDST_W);
      } else {
        if (ii) {
           if ((variable_def_member_sz_get(mbr) * variable_def_member_ct_get(mbr)
              > 255) && (value_byte_sz_get(baseofs) > 1)) {
              /* Since we use _pic_loop we have to increment with 1, not with ii.*/
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
              pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, tmp, 0, PIC_OPDST_F);
              pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, baseofs, 1, PIC_OPDST_W);
          }
          else {
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, ii);
              pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, tmp, 0, PIC_OPDST_W);
          }
        }
        pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
      }
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
      }
    }
    if (tmp && (tmp != baseofs)) {
      pic_var_loop_release(pf, tmp);
    }
    value_release(tablat);
    label_release(lbl);
    pic_value_sign_extend(pf, dst, ii - 1, value_is_signed(src));
  }
}


void pic_assign_lookup_label_to_pointer(pfile_t *pf, value_t dst,
    label_t lbl, variable_const_t offset)
{
  variable_sz_t ii;

  for (ii = 0; ii < pic_pointer_size_get(pf); ii++) {
    pic_code_t code;

    code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
    pic_code_brdst_set(code, lbl);
    pic_code_ofs_set(code, ii);
    if (offset) {
      if (ii) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
        pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
      }
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW,
          (offset >> (8 * ii)) & 0xff);
    }
    if (ii + 1 == pic_pointer_size_get(pf)) {
      /* jalv25r9: bit 7 marks lookup/flash pointers (issue #30). */
      pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW, 0x80);
    }
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
  }
}

static void pic_assign_from_label_offset(pfile_t *pf, value_t dst, label_t lbl,
    variable_const_t offset)
{
  pic_code_t    code;
  variable_sz_t ipos;

  pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
  code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
  pic_code_brdst_set(code, lbl);
  if (offset) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, offset & 0xff);
  }
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
  if (value_sz_get(dst) > 1) {
    variable_sz_t ii;

    pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, 1, &ipos);
    code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
    pic_code_brdst_set(code, lbl);
    pic_code_ofs_set(code, 1);
    if (offset) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, (offset >> 8) & 0xff);
    }
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 1);
    for (ii = 2; ii < value_sz_get(dst); ii++) {
      pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
    }
  }
}

static void pic_assign_from_label2(pfile_t *pf, value_t dst, label_t lbl)
{
  pic_assign_from_label_offset(pf, dst, lbl, 0);
}



/* In-place add of an offset expression into a pointer SLOT. dst here is a
 * pointer VALUE-VIEW (`*-V-`): routing the add through pic_op's generic
 * emitters computes into a temp and stores the result THROUGH the view --
 * store-through semantics (bound-trap + FSR + INDF), correct for `p = v`
 * but wrong for a slot update: it writes the pointee instead of the slot
 * and left `p is arr[k]` pointing at the array base with the element
 * address smashed over arr[0] (both 14-bit cores; the 16-bit core adds
 * in place and was fine). A plain slot view over the same variable stores
 * to the slot bytes on every core. */
void pic_pointer_slot_add_emit(pfile_t *pf, value_t dst, value_t ofs)
{
  value_t       slot;
  value_t       tmp;
  variable_sz_t ii;
  variable_sz_t psz;

  if (pic_is_16bit(pf)) {
    /* the wide core's add emitters update the slot in place (addwf ,f) */
    pic_op(pf, OPERATOR_ADD, dst, dst, ofs);
    return;
  }
  /* 14-bit: pic_op on a pointer-typed dst is store-through semantics
   * (bound-trap + FSR + INDF) -- compute into a temp, then move the
   * bytes into a cleaned slot view (pic_assign_ptr_to_ptr's recipe) */
  slot = value_clone(dst);
  tmp  = VALUE_NONE;
  if (slot) {
    value_baseofs_set(slot, VALUE_NONE);
    value_indirect_clear(slot);
    psz = pic_pointer_size_get(pf);
    tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, psz);
  }
  if (slot && tmp) {
    pic_op(pf, OPERATOR_ADD, tmp, slot, ofs);
    for (ii = 0; ii < psz; ii++) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, ii, PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, slot, ii);
    }
  } else {
    pic_op(pf, OPERATOR_ADD, dst, dst, ofs);
  }
  if (VALUE_NONE != tmp) {
    pic_var_temp_release(pf, tmp);
  }
  value_release(slot);
}

static void pic_assign_from_value(pfile_t *pf, value_t dst, value_t src)
{
  variable_sz_t ipos;
  variable_t    var;

  /* *p = <numeric literal>: dst is indirect (write-through pointer). The const
   * branch below only runs pass-1 label glue and never emits per-byte INDF
   * stores on pass 2 — PIC14 movlw 255 / wrong store (volatile byte literal);
   * extend to word / dword / etc. (little-endian, sign-extend when dst wider
   * than the constant's byte width). See fix_pic14_volatile_pointer_byte_assign_constant.md. */
  if (value_is_indirect(dst) && value_is_const(src) && value_is_number(src)) {
    variable_sz_t       sz;
    variable_sz_t       ii;
    variable_sz_t       srcsz;
    variable_const_t    v;
    uchar               ch;
    uchar               msb_ch;

    sz = (pic_value_is_w(dst) ? 1 : value_sz_get(dst));
    var = value_variable_get(src);
    if (var) {
      variable_use_ct_bump(var, CTR_BUMP_INCR);
    }
    srcsz = value_byte_sz_get(src);
    if (0 == srcsz) {
      srcsz = value_sz_get(src);
    }
    if (0 == srcsz) {
      srcsz = 1;
    }
    v = value_const_get(src);
    msb_ch = (uchar) ((v >> (8 * (srcsz - 1))) & 0xff);
    pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
    for (ii = 0; ii < sz; ii++) {
      pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
      if (ii < srcsz) {
        ch = (uchar) ((v >> (8 * ii)) & 0xff);
      } else if (value_is_signed(src)) {
        ch = (msb_ch & 0x80) ? (uchar) 0xff : (uchar) 0;
      } else {
        ch = 0;
      }
      if (ch || pic_value_is_w(dst)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, ch);
      }
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, ch ? PIC_OPCODE_MOVWF : PIC_OPCODE_CLRF, dst, ii);
      }
    }
    return;
  }

  /* Before variable_value_get: folded `p is buf` passes a VALUE aggregate as
   * `src` (pic_try_bind / value_variable_get miss); PTR_LOOKUP marks the dest
   * pointer; pic_src_is_data_array includes VALUE_FLAG_ARRAY. */
  {
    variable_t dvb;

    dvb = value_variable_get(dst);
    if (dvb && VARIABLE_NONE != dvb
        && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(variable_def_get(dvb))
        && !value_is_pointer(src)
        && value_is_value(src)
        && value_sz_get(src) > 1
        && value_sz_get(src) == value_sz_get(dst)
        && (pic_src_is_data_array(src)
            || pic_variable_is_rom_int_const_table(value_variable_get(src))
            || value_dflag_test(src, VARIABLE_DEF_FLAG_CONST))) {
      value_t ptr_slot;

      ptr_slot = value_alloc(dvb);
      pic_assign_array_to_pointer(pf, ptr_slot, src);
      value_release(ptr_slot);
      return;
    }
  }

  var = value_variable_get(src);
  /* Mark source variable as used when accessing its value */
  if (var) {
    variable_use_ct_bump(var, CTR_BUMP_INCR);
  }
  src = variable_value_get(var);

  if (value_is_const(src)
      && pic_variable_is_rom_int_const_table(value_variable_get(src))) {
    if (pic_code_gen_pass_get(pf) == 2) {
      label_t lbl;
      variable_const_t offset;

      lbl = pic_lookup_label_find(pf,
          value_variable_get(variable_value_get(var)),
          (pic_is_16bit(pf))
          ? PIC_LOOKUP_LABEL_FIND_FLAG_DATA
          : PIC_LOOKUP_LABEL_FIND_FLAG_NONE);
      if (lbl) {
        if (!value_baseofs_const_get(src, &offset)) {
          pfile_log(pf, PFILE_LOG_ERR,
              "WHEREIS() runtime offsets are not supported for constants");
        } else {
          pic_assign_from_label_offset(pf, dst, lbl, offset);
        }
        label_release(lbl);
        return;
      }
    }
  }
  {
    variable_const_t offset;
    variable_base_t  base;

    if (!value_baseofs_const_get(src, &offset)) {
      value_t base_val;

      base_val = pfile_constant_get(pf, value_base_get(src),
          VARIABLE_DEF_NONE);
      pic_op(pf, OPERATOR_ASSIGN, dst, base_val, VALUE_NONE);
      pic_pointer_slot_add_emit(pf, dst, value_baseofs_get(src));
      value_release(base_val);
      return;
    }
    base = value_base_get(src) + offset;
    pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);

    if (value_sz_get(dst) > 1) {
      variable_sz_t ii;

      pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, 1, &ipos);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          base >> 8);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 1);
      for (ii = 2; ii < value_sz_get(dst); ii++) {
        pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
      }
    }
  }
}

/* Store address of a data lvalue (scalar or aggregate) into a data pointer slot.
 * Unlike pic_assign_from_value, does not replace src with variable_value_get(var)
 * (which loses the lvalue for const scalars and breaks address extraction). */
static void pic_assign_address_of_lvalue_to_pointer(pfile_t *pf, value_t dst,
    value_t src)
{
  variable_sz_t   ii;
  variable_t      src_var;
  value_t         rt_baseofs;
  boolean_t       slot_rt;

  src_var = value_variable_get(src);
  if (src_var) {
    variable_use_ct_bump(src_var, CTR_BUMP_INCR);
  }
  if (value_is_pointer(dst)) {
    variable_def_t src_def = value_def_get(src);
    variable_def_t ptr_def = value_def_get(dst);
    variable_def_member_t existing_mbr = variable_def_member_get(ptr_def);

    if (!existing_mbr) {
      variable_def_member_add(ptr_def, 0, variable_def_dup(src_def), 1);
    }
  }

  {
    variable_sz_t dst_sz;

    /* Same as pic_assign_array_to_pointer: folded `var pointer const byte p[N]`
     * is one pointer slot; value_sz_get(dst) can reflect the pointee aggregate. */
    dst_sz = (value_is_pointer(dst) && !value_is_array(dst))
      ? pic_pointer_size_get(pf)
      : value_sz_get(dst);

    /* Runtime-indexed ARRAY-of-pointers element as the bind DESTINATION
     * (`slots[k] is <var>`): the plain MOVWF dst,ii below resolves a
     * subscripted destination to the _ind scratch -- both pointer bytes
     * landed there and the slot was never written (silent no-op bind on
     * BOTH classic cores, 2026-10-28). Stage FSR = &slot[k] directly --
     * NOT pic_indirect_setup, which has through-access semantics for
     * slot elements (it would dereference the slot) -- and store each
     * byte through INDF with a carry-safe increment on wide cores. */
    rt_baseofs = value_baseofs_get(dst);
    slot_rt = pic_value_variable_is_array_of_pointers(dst)
      && (VALUE_NONE != rt_baseofs) && !value_is_const(rt_baseofs);
    if (slot_rt) {
      value_t   indv;
      value_t   fsrv;
      boolean_t wide16;

      wide16 = pic_is_16bit(pf) || pic_is_14bit_hybrid(pf);
      indv = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
      fsrv = pic_fsr_get(pf);
      pic_classic_fsr_load_base_dynamic_index(pf,
          value_variable_get(dst), value_base_get(dst), rt_baseofs,
          (variable_sz_t) value_byte_sz_get(dst));
      if (src_var) {
        for (ii = 0; ii < dst_sz; ii++) {
          pic_code_t code;
          unsigned   flag;

          code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
          pic_code_value_set(code, src);
          flag = PIC_CODE_FLAG_NONE;
          switch (ii) {
            case 0: break;
            case 1: flag = PIC_CODE_FLAG_LABEL_HIGH;  break;
            case 2: flag = PIC_CODE_FLAG_LABEL_UPPER; break;
          }
          pic_code_flag_set(code, flag);
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, indv, 0);
          if (ii + 1 < dst_sz) {
            pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsrv, 0,
              PIC_OPDST_F);
            if (wide16) {
              pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC,
                "_status", "_z");
              pic_instr_append_reg(pf, PIC_OPCODE_INCF, "_fsr0h");
            }
          }
        }
      } else {
        variable_base_t base;

        for (ii = 0, base = value_base_get(src);
            ii < dst_sz;
            ii++, base >>= 8) {
          if (base & 0xff) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (uchar) (base & 0xff));
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, indv, 0);
          } else {
            pic_instr_append_f(pf, PIC_OPCODE_CLRF, indv, 0);
          }
          if (ii + 1 < dst_sz) {
            pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsrv, 0,
              PIC_OPDST_F);
            if (wide16) {
              pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC,
                "_status", "_z");
              pic_instr_append_reg(pf, PIC_OPCODE_INCF, "_fsr0h");
            }
          }
        }
      }
      value_release(fsrv);
      value_release(indv);
      return;
    }

    if (src_var) {
      /* Do not use `if (value_base_get(src))`: the low byte of the address may be
       * 0 (e.g. 0x100), and bases can be unknown during early passes — still emit
       * relocatable MOVLW via pic_code_value_set for every pointer byte. */
      for (ii = 0; ii < dst_sz; ii++) {
        pic_code_t code;
        unsigned   flag;

        code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
        pic_code_value_set(code, src);
        flag = PIC_CODE_FLAG_NONE;
        switch (ii) {
          case 0: break;
          case 1: flag = PIC_CODE_FLAG_LABEL_HIGH;  break;
          case 2: flag = PIC_CODE_FLAG_LABEL_UPPER; break;
        }
        pic_code_flag_set(code, flag);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
      }
    } else {
      variable_base_t base;

      for (ii = 0, base = value_base_get(src);
          ii < dst_sz;
          ii++, base >>= 8) {
        if (base & 0xff) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (uchar) (base & 0xff));
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        } else {
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
        }
      }
    }
  }
}

static boolean_t pic_assign_from_whereis_const(pfile_t *pf, value_t dst,
    value_t src)
{
  variable_t src_var;
  label_t    lbl;
  value_t    target;

  src_var = value_variable_get(src);
  if (!src_var) {
    return BOOLEAN_FALSE;
  }
  lbl = variable_label_get(src_var);
  if (lbl) {
    pic_assign_from_label2(pf, dst, lbl);
    return BOOLEAN_TRUE;
  }
  target = variable_value_get(src_var);
  if (target) {
    pic_assign_address_of_lvalue_to_pointer(pf, dst, target);
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

static void pic_assign_from_label(pfile_t *pf, value_t dst, value_t src)
{
  /* Mark source variable as used for pointer aliasing (current_sensor is sensor1_data) */
  variable_t src_var = value_variable_get(src);
  if (src_var) {
    variable_use_ct_bump(src_var, CTR_BUMP_INCR);
  }
  
  /* For pointer assignments, set up member definition to match the source variable */
  if (value_is_pointer(dst)) {
    variable_def_t src_def = value_def_get(src);
    variable_def_t ptr_def = value_def_get(dst);
    variable_def_member_t existing_mbr = variable_def_member_get(ptr_def);
    
    if (!existing_mbr) {
      /* No member definition exists - add one that matches the source variable */
      variable_def_member_add(ptr_def, 0, variable_def_dup(src_def), 1);
    }
    /* Continue with the label assignment */
  }
  pic_assign_from_label2(pf, dst, value_label_get(src));
}

static void pic_assign_from_function(pfile_t *pf, value_t dst,
  value_t src)
{
  variable_def_t src_def;
  variable_def_t dst_def;
  pic_code_t     code;
  pfile_proc_t  *proc;
  label_t        lbl;
  variable_sz_t  ipos;
  size_t         ii;

  assert(value_is_pointer(dst));
  src_def = value_def_get(src);
  dst_def = value_def_get(dst);
  /* dereference dst def */
  dst_def = variable_def_member_def_get(variable_def_member_get(dst_def));
  
  /* For generic pointers, set up the member definition to match the function */
  if (!dst_def || !variable_def_is_same(src_def, dst_def)) {
    /* The pointer doesn't have a proper member definition - set it up now */
    variable_def_t ptr_def = value_def_get(dst);
    variable_def_member_t existing_mbr = variable_def_member_get(ptr_def);
    
    if (!existing_mbr) {
      /* No member definition exists - add one that matches the function */
      variable_def_member_add(ptr_def, 0, variable_def_dup(src_def), 1);
    } else {
      /* Member exists but doesn't match - this is expected for generic pointers */
      /* Continue with assignment */
    }
  }

  proc = value_proc_get(src);
  lbl  = pfile_proc_label_get(proc);

  pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
  for (ii = 0; ii < pic_pointer_size_get(pf); ii++) {
    code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
    pic_code_brdst_set(code, lbl);
    pic_code_ofs_set(code, ii);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
  }
}

/* note: dst or src can be W */
void pic_op_assign(pfile_t *pf, operator_t op, value_t dst, 
    value_t src, value_t val2)
{
  UNUSED(val2);
  /* Layer 2: a pointer bind makes the target's addressing linear (*p +
   * i cannot walk a chunk map) -- record it for the placement refusal,
   * before any dispatch (several early branches fold pointer assigns) */
  if (OPERATOR_POINTER_ASSIGN == op) {
    pic_var_pointer_target_bump(value_variable_get(src));
    /* bind registry for the escape analysis: an unescaped pointer's
     * derefs can walk the target's chunk map (map-carrying open-array
     * formals included); an ESCAPED one is refused after the scan */
    pic_var_pointer_bind_record(value_variable_get(dst),
        value_variable_get(src));
  }
  if (value_is_same(dst, src)) {
    /* this might happen during some internal work, ignore it */
  } else if ((OPERATOR_POINTER_ASSIGN == op
        || (OPERATOR_ASSIGN == op && !value_flag_test(dst, VALUE_FLAG_INDIRECT)))
      && pic_try_bind_array_to_pointer_slot(pf, dst, src)) {
    /* `slot[i] = x` is OPERATOR_ASSIGN with INDIRECT (store through the slot's
     * pointer); do not run array-to-pointer bind (that would treat RHS as an
     * address source and crash in pic_assign_array_to_pointer). Folded `p is buf`
     * uses OPERATOR_POINTER_ASSIGN, or ASSIGN without INDIRECT on dst. */
  } else if ((OPERATOR_ASSIGN == op) && value_is_pointer(dst)
      && value_is_function(src)) {
    variable_t dv;

    /* Formal IN parameters use pointer slots for the getter (volatile IN,
     * etc.); store the function triplet. Other pointer=function assigns
     * (e.g. in libraries) must still dereference. */
    dv = value_variable_get(dst);
    if (dv && variable_dflag_test(dv, VARIABLE_DEF_FLAG_IN)) {
      pic_assign_from_function(pf, dst, src);
    } else {
      value_t indirect_dst;

      indirect_dst = value_clone(dst);
      value_dereference(indirect_dst);
      value_indirect_set(indirect_dst);
      pic_assign_from_value(pf, indirect_dst, src);
      value_release(indirect_dst);
    }
  } else if ((OPERATOR_ASSIGN == op) && value_is_pointer(dst)
      && !value_is_pointer(src) && !value_is_array(src)
      && !pic_try_src_arraylike_for_pointer_bind(src)) {
    /* Dereference assignment only: store a value through a pointer (e.g. *p = x).
     * If src is another pointer or array, we must copy the address into dst's
     * slot (pic_assign_array_to_pointer / simple assign), not write through an
     * uninitialized formal pointer — e.g. checksum_16_byte_calc(buff,...) from
     * checksum_16_byte_verify where buff is byte in buff[].
     * Exclude pic_src_is_data_array: `p is buf` for folded pointer-to-const-array
     * can reach here with src typed as VALUE while the variable is still ARRAY;
     * without this, we never reach pic_try_bind_array_to_pointer_slot. */
    value_t indirect_dst = value_clone(dst);
    value_dereference(indirect_dst);
    value_indirect_set(indirect_dst);
    if (value_is_const(src) && value_is_number(src)) {
      pic_assign_from_value(pf, indirect_dst, src);
    } else {
      /* Runtime expression source: store the VALUE through the pointer.
       * pic_assign_from_value treats a non-const src as an address bind
       * (pointer-slot semantics), which would clobber the address with
       * the expression result -- walk the pointee bytes via W instead
       * (same shape as the const path in pic_assign_from_value).
       * Widening bytes past the source width store 0; sign extension
       * needs the runtime top bit and is not attempted here. */
      variable_sz_t sz;
      variable_sz_t srcsz;
      variable_sz_t ii;
      variable_sz_t ipos;
      variable_t   var;

      sz    = value_sz_get(indirect_dst);
      srcsz = value_byte_sz_get(src);
      if (!srcsz) {
        srcsz = value_sz_get(src);
      }
      if (!srcsz) {
        srcsz = 1;
      }
      var = value_variable_get(src);
      if (var) {
        variable_use_ct_bump(var, CTR_BUMP_INCR);
      }
      pic_indirect_setup3(pf, indirect_dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
      for (ii = 0; ii < sz; ii++) {
        pic_indirect_bump3(pf, indirect_dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
        if (ii < srcsz) {
          pic_value_to_w(pf, PIC_VALUE_TO_W_MOV, src, VALUE_NONE, ii);
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, indirect_dst, ii);
        } else {
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, indirect_dst, ii);
        }
      }
    }
    value_release(indirect_dst);
  } else if (OPERATOR_POINTER_ASSIGN == op) {
    /* Handle pointer assignment using 'is' operator */
    if (value_is_pointer(dst) && value_is_array(src)) {
      pic_assign_array_to_pointer(pf, dst, src);
    } else if (value_is_pointer(dst) && value_is_function(src)) {
      pic_assign_from_function(pf, dst, src);
    } else if (value_is_pointer(dst) && value_is_pointer(src)) {
      pic_assign_ptr_to_ptr(pf, dst, src);
    } else if (value_is_pointer(dst)
      && (!value_is_const(src) || (value_variable_get(src) != NULL))) {
      /* Generic pointer assignment - store address of src in dst. Use
       * pic_assign_from_value for data objects: value_label_get(src) is often
       * LABEL_NONE for GPR scalars, so pic_assign_from_label2 would emit a bare
       * movlw with no operand. Code / procedure labels keep pic_assign_from_label. */
      if (value_is_label(src)) {
        pic_assign_from_label(pf, dst, src);
      } else {
        pic_assign_address_of_lvalue_to_pointer(pf, dst, src);
      }
    } else {
      pfile_log(pf, PFILE_LOG_ERR, "invalid pointer assignment");
    }
  } else if (value_is_float(dst) && !value_is_float(src)) {
    pic_assign_to_float(pf, dst, src);
  } else if (value_is_float(src) && !value_is_float(dst)) {
    pic_assign_from_float(pf, dst, src);
  } else if (value_is_function(src)) {
    /* pic_assign_from_function stores a function triplet into a pointer
     * slot and asserts value_is_pointer(dst). A non-pointer destination
     * (alias of a procedure: `alias serial_write is serial_hw_write`)
     * must keep the plain label assignment. */
    if (value_is_pointer(dst)) {
      pic_assign_from_function(pf, dst, src);
    } else {
      pic_assign_from_label(pf, dst, src);
    }
  } else if (value_is_label(src)) {
    pic_assign_from_label(pf, dst, src);
  } else if (value_is_value(src)) {
    pic_assign_from_value(pf, dst, src);
  } else if (/* value_flag_test(dst, VALUE_FLAG_INDIRECT) && */
      (variable_flags_get_all(value_variable_get(dst))
        & (VARIABLE_FLAG_PTR_LOOKUP
          | VARIABLE_FLAG_PTR_EEPROM
          | VARIABLE_FLAG_PTR_FLASH))) {
    pic_assign_to_ptr(pf, dst, src);
  } else if (!value_flag_test(src, VALUE_FLAG_PTR_SLOT_RAW)
      && /*value_flag_test(src, VALUE_FLAG_INDIRECT) && */
      (variable_flags_get_all(value_variable_get(src))
        & (VARIABLE_FLAG_PTR_LOOKUP
          | VARIABLE_FLAG_PTR_EEPROM
          | VARIABLE_FLAG_PTR_FLASH))) {
    pic_assign_from_ptr(pf, dst, src);
  } else if (value_is_indirect(dst) && value_is_indirect(src)) {
    /* src must move to a temporary */
    value_t tmp;

    tmp = pic_var_temp_get_assign(pf, src, VALUE_NONE, dst);
    pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
    pic_var_temp_release(pf, tmp);
  } else if (value_is_lookup(src)) {
    pic_assign_from_lookup(pf, dst, src);
  } else if (value_is_bit(dst)) {
    pic_assign_to_bit(pf, dst, src);
  } else if (pic_assign_from_whereis_const(pf, dst, src)) {
    /* Relocatable const initialized from WHEREIS(...). */
  } else if (value_is_const(src) && !value_is_indirect(src)
      && !value_flag_test(src, VALUE_FLAG_POINTER)) {
    /* Const *pointee* lvalues (e.g. read through `var pointer const byte p`) still
     * have VARIABLE_DEF_FLAG_CONST on the value and test as value_is_const, but
     * they are RAM/INDF loads — not pic_assign_from_const (ROM/initializer data).
     * value_is_indirect() alone misses POINTER+const offset (INDIRECT clear). */
    pic_assign_from_const(pf, dst, src);
  } else if (value_is_bit(src)) {
    pic_assign_from_bit(pf, dst, src);
  } else {
    /* simple assignment from src to dst */
    variable_sz_t ipos;
    variable_sz_t ii;
    variable_sz_t sz;
    value_t       accum;

    accum = VALUE_NONE;
    sz = pic_result_sz_get(src, VALUE_NONE, dst);
    if (pic_value_is_w(dst)) {
      sz = 1;
    }
    if (pic_value_is_w(src) && value_is_indirect(dst)) {
      /* store src in the accumulator */
      accum = pic_var_accum_get(pf);
      src   = accum;
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, accum, 0);
    }
    pic_indirect_setup3(pf, src, VALUE_NONE, dst, 0, &ipos);
    for (ii = 0; ii < sz; ii++) {
      pic_indirect_bump3(pf, dst, src, VALUE_NONE, ii, &ipos);
      if (!pic_value_is_w(src)) {
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, ii, PIC_OPDST_W);
      }
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
      }
    }
    pic_value_sign_extend(pf, dst, ii - 1, value_is_signed(src));
    if (accum) {
      pic_var_accum_release(pf, accum);
    }
  }
}

/************************************************************
 **
 ** pic_op_ptr.c : PIC operator generation -- pointer assignment shapes
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

void pic_assign_array_to_pointer(pfile_t *pf, value_t dst, value_t src)
{
  /* Set up member definition for array pointer */
  if (value_is_pointer(dst) && value_is_array(src)) {
    variable_def_t src_def = value_def_get(src);
    variable_def_t ptr_def = value_def_get(dst);
    variable_def_member_t existing_mbr = variable_def_member_get(ptr_def);
    
    if (!existing_mbr) {
      /* No member definition exists - add one that matches the array element type */
      variable_def_t array_element_def = variable_def_member_def_get(variable_def_member_get(src_def));
      if (array_element_def) {
        variable_def_member_add(ptr_def, 0, variable_def_dup(array_element_def), 1);
      } else {
        /* Fallback: assume byte array if no element definition found */
        variable_def_member_add(ptr_def, 0, 
          variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_NONE, 1), 1);
      }
    }
  }
  
  {
    variable_t sv0;

    sv0 = value_variable_get(src);
    /* `value_is_const(src)` is false when baseofs is non-const even if the
     * variable is a const ROM lookup array — still load `_data_<name>`. */
    if (sv0 && VARIABLE_NONE != sv0
        && (value_is_const(src) || variable_is_lookup(sv0)
            || variable_is_const(sv0))) {
      label_t    lbl;

      lbl = pic_lookup_label_find(pf, sv0,
          PIC_LOOKUP_LABEL_FIND_FLAG_ALLOC
          | PIC_LOOKUP_LABEL_FIND_FLAG_DATA);
      if (lbl) {
        variable_const_t offset;

        offset = 0;
        if (value_baseofs_const_get(src, &offset)) {
          pic_assign_lookup_label_to_pointer(pf, dst, lbl, offset);
        } else {
          /* jalv2compiler #36 (runtime-index variant): the actual is
           * `table[runtime_index].field` -- baseofs is the front end's
           * index * stride + member-offset expression, not a constant.
           * Bind the table base, then add that expression; the top-byte
           * program-memory marker survives the add because real table
           * addresses keep that bit clear. */
          pic_assign_lookup_label_to_pointer(pf, dst, lbl, 0);
          if (value_baseofs_get(src)) {
            pic_pointer_slot_add_emit(pf, dst, value_baseofs_get(src));
          }
        }
        label_release(lbl);
        return;
      }
    }
  }
  {
    variable_base_t base;
    variable_sz_t   ii;
    variable_sz_t   dst_sz;
    variable_t       sv;
    boolean_t        addr_from_data;

    sv = value_variable_get(src);
    addr_from_data = (sv && (variable_data_get(sv) || variable_is_lookup(sv)));
    /* `var pointer const byte p[N]` folds to one pointer; value_sz_get(dst) is
     * the pointee aggregate size (N bytes), not the pointer slot width — using
     * it here wrote N bytes through INDF (e.g. movlw 0xff / movwf v__ind) and
     * never loaded the array address into the slot. */
    dst_sz = (value_is_pointer(dst) && !value_is_array(dst))
      ? pic_pointer_size_get(pf)
      : value_sz_get(dst);
    for (ii = 0, base = value_base_get(src);
         ii < dst_sz;
         ii++, base >>= 8) {
      if (base || addr_from_data) {
        pic_code_t code;
        unsigned   flag;

        if (addr_from_data) {
          code = pic_instr_append(pf, PIC_OPCODE_MOVLW);
          pic_code_value_set(code, src);
          flag = PIC_CODE_FLAG_NONE;
          switch (ii) {
            case 0: break;
            case 1: flag = PIC_CODE_FLAG_LABEL_HIGH;  break;
            case 2: flag = PIC_CODE_FLAG_LABEL_UPPER; break;
          }
          pic_code_flag_set(code, flag);
        } else {
          /* RAM base: emit the address byte as a LITERAL. Binding via
           * pic_code_value_set resolves through the variable's current
           * value, which a subscript parse leaves pointing at the
           * indirect scratch -- `ram_array[index].field` then bound the
           * scratch address instead of the array (#36 runtime-index). */
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (variable_const_t) (base & 0xff));
        }
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
      } else {
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
      }
    }
    /* RAM twin of the #36 runtime-index fix: the loop above binds only
     * the array base; a runtime subscript (`ram_array[index].field`)
     * rides in baseofs and must be added, or the formal binds the
     * array start instead of the element. */
    if (value_baseofs_get(src)
        && (!value_is_const(value_baseofs_get(src))
          || value_const_get(value_baseofs_get(src)))) {
      pic_pointer_slot_add_emit(pf, dst, value_baseofs_get(src));
    }
  }
}

void pic_assign_ptr_to_ptr(pfile_t *pf, value_t dst, value_t src)
{
  variable_sz_t ii;

  /* nb: src might have baseofs set if it is called indirectly or the
   *     function is re-entrant. In either case, src will be an offset
   *     into pic_temp
   */
  /*assert(!value_baseofs_get(src));*/
  src = value_clone(src);
  value_baseofs_set(src, VALUE_NONE);
  value_indirect_clear(src);

  assert(!value_baseofs_get(dst));
  dst = value_clone(dst);
  value_baseofs_set(dst, VALUE_NONE);
  value_indirect_clear(dst);

  for (ii = 0; ii < pic_pointer_size_get(pf); ii++) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, ii, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
  }
  value_release(dst);
  value_release(src);
}

/* dst has one or more of the LOOKUP/EEPROM/FLASH bits set */
void pic_assign_to_ptr(pfile_t *pf, value_t dst, value_t src)
{
  variable_t      dvar;
  variable_t      svar;
  variable_def_t  sdef;
  boolean_t       src_array_like;

  /* PTR_LOOKUP is set on the pointer when a const array is bound for reads, but
   * `p is buf` is still "store buf's address in p" — not the lookup-label loop
   * below (which treats dst as an INDF destination). */
  dvar = value_variable_get(dst);
  src_array_like = value_is_array(src);
  if (!src_array_like) {
    src_array_like = value_flag_test(src, VALUE_FLAG_ARRAY);
  }
  if (!src_array_like && (svar = value_variable_get(src)) != VARIABLE_NONE) {
    sdef = variable_def_get(svar);
    src_array_like = (sdef != 0)
      && (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(sdef));
  }
  /* Folded `var pointer const byte p[N]` is VARIABLE_DEF_TYPE_ARRAY on the
   * backing var, not POINTER — match pic_dst_backs_pointer_storage /
   * pic_try_bind_array_to_pointer_slot so `p is rom[]` does not fall through to
   * "Attempt to assign to a lookup table". */
  if (pic_dst_backs_pointer_storage(dst) && dvar && VARIABLE_NONE != dvar
      && src_array_like && !value_is_pointer(src)) {
    value_t ptr_slot;

    ptr_slot = value_alloc(dvar);
    if (ptr_slot) {
      pic_assign_array_to_pointer(pf, ptr_slot, src);
      value_release(ptr_slot);
      return;
    }
  }

  if (value_is_pointer(src)) {
    pic_assign_ptr_to_ptr(pf, dst, src);
  } else if (variable_is_lookup(value_variable_get(src))) {
    label_t       lbl;
    variable_const_t offset;

    lbl = pic_lookup_label_find(pf, value_variable_get(src), 
        PIC_LOOKUP_LABEL_FIND_FLAG_ALLOC
        | PIC_LOOKUP_LABEL_FIND_FLAG_DATA);
    {
      value_t bofs;

      bofs = value_baseofs_get(src);
      offset = (bofs && value_baseofs_const_get(src, &offset)) ? offset : 0;
    }
    pic_assign_lookup_label_to_pointer(pf, dst, lbl, offset);
    label_release(lbl);
  } else if (value_is_array(src)) {
    pic_assign_array_to_pointer(pf, dst, src);
  } else {
    if (variable_flag_test(value_variable_get(dst),
      VARIABLE_FLAG_PTR_LOOKUP)) {
      pfile_log(pf, PFILE_LOG_ERR, 
        "Attempt to assign to a lookup table");
    } else {
      assert(0);
    }
  }
}

/* src has one or more of the LOOKUP/EEPROM/FLASH bits set */
void pic_assign_from_ptr(pfile_t *pf, value_t dst, value_t src)
{
  if (value_is_pointer(dst)) {
    pic_assign_ptr_to_ptr(pf, dst, src);
  } else {
    variable_t     svar;
    value_t        tmp;
    value_t        baseofs;
    variable_sz_t  ii;
    variable_def_t src_def;

    src     = value_clone(src);
    baseofs = value_baseofs_get(src);
    value_baseofs_set(src, VALUE_NONE);
    value_indirect_clear(src);

    /*#error "this needs fixin!!!";*/
    /* src needs another variable placed at it, with the flags clear
     * to make the new variable appear to be an integer of the same
     * size
     */
    tmp = pic_var_pointer_get(pf);
    src_def = value_def_get(src);
    value_def_set(src, value_def_get(tmp));

    svar = value_variable_get(src);
    if ((variable_flag_test(svar, VARIABLE_FLAG_PTR_PTR)
          && variable_flag_test(svar, VARIABLE_FLAG_PTR_LOOKUP))
        || variable_flag_test(svar, VARIABLE_FLAG_PTR_EEPROM)
        || variable_flag_test(svar, VARIABLE_FLAG_PTR_FLASH)) {
      /* this will necessitate a function call */
      variable_sz_t sz;
      variable_sz_t ipos;
      label_t       lbl;

      if ((VALUE_NONE == baseofs)
        || (value_is_const(baseofs) && !value_const_get(baseofs))) {
        /* nb: cannot call pic_op(...OPERATOR_ASSIGN...) here as we
         *     end up in an infinite loop, so just do the necessary
         *     inline 
         * pic_op(pf, OPERATOR_ASSIGN, tmp, src, VALUE_NONE);
         */
        assert(value_sz_get(src) == value_sz_get(tmp));
        for (ii = 0; ii < value_sz_get(tmp); ii++) {
          if (ii < value_sz_get(src)) {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, ii, PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, ii);
          } else {
            pic_instr_append_f(pf, PIC_OPCODE_CLRF, tmp, ii);
          }
        }
      } else if (pic_value_variable_is_array_of_pointers(src)) {
        /* ARRAY-of-pointers element read at a runtime index: DOUBLE
         * indirection -- FSR = &slot[k], load the slot's pointer into
         * tmp, and the existing PTR_READ flow dereferences tmp (the
         * old *slot0+ofs staging read the bound code address as the
         * data base). 16-bit core + hybrid variant added 2026-10-28
         * (carry-safe FSR0 increment; RAM slots carry a zero upper
         * pointer byte, so byte 2 is cleared rather than loaded) */
        value_t   indv;
        value_t   fsrv;
        boolean_t wide16;

        wide16 = pic_is_16bit(pf) || pic_is_14bit_hybrid(pf);
        indv = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
        fsrv = pic_fsr_get(pf);
        pic_classic_fsr_load_base_dynamic_index(pf,
            value_variable_get(src), value_base_get(src), baseofs,
            (variable_sz_t) value_byte_sz_get(src));
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, indv, 0,
            PIC_OPDST_W);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 0);
        pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsrv, 0,
            PIC_OPDST_F);
        if (wide16) {
          /* INCF _fsr0l alone cannot cross a 256-byte boundary */
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status",
              "_z");
          pic_instr_append_reg(pf, PIC_OPCODE_INCF, "_fsr0h");
        }
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, indv, 0,
            PIC_OPDST_W);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 1);
        if (wide16 && (value_sz_get(tmp) > 2)) {
          pic_instr_append_f(pf, PIC_OPCODE_CLRF, tmp, 2);
        }
        value_release(fsrv);
        value_release(indv);
      } else {
        pic_op(pf, OPERATOR_ADD, tmp, src, baseofs);
      }
      value_def_set(src, src_def);
      lbl = pic_label_find(pf, PIC_LABEL_PTR_READ, BOOLEAN_TRUE);
      sz  = pic_result_sz_get(src, VALUE_NONE, dst);
      pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
      for (ii = 0; ii < sz; ii++) {
        if (ii) {
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, tmp, 0, PIC_OPDST_F);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_f_d(pf, PIC_OPCODE_INCF, tmp, 1, PIC_OPDST_F);
        }
        pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
        pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
        if (!pic_value_is_w(dst)) {
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
      }
      pic_value_sign_extend(pf, dst, ii - 1, value_is_signed(src));

      label_release(lbl);
    } else if (variable_flag_test(svar, VARIABLE_FLAG_PTR_PTR)) {
      /* ptr --> dst */
      assert(0);
    } else if (variable_flag_test(svar, VARIABLE_FLAG_PTR_LOOKUP)) {
      /* lookup --> dst */
      /* pic_sign holds LSB */
      /* W holds MSB */
      variable_sz_t sz;
      variable_sz_t ipos;
      value_t       mask;
      ulong         cmask;

      /*
       * bit 6 of the high byte of the pointer is 1, bit 7 is 0
       * so, simply clear bit 6
       */
      /* jalv25r9: We should mask off only bit 7, not bit 6 to address issue#30. */
      cmask = ~(1UL << (8 * pic_pointer_size_get(pf) - 1));
      mask = pfile_constant_get(pf, cmask, VARIABLE_DEF_NONE);
      if (pic_is_16bit(pf)) {
        pic_var_pointer_release(pf, tmp);
        tmp = pfile_value_find(pf, PFILE_LOG_ERR, "_tblptr");
        pic_stvar_tblptr_mark(pf);
      }
      /*
       * if baseofs is zero, `OPERATOR_ADD' --> `OPERATOR_ASSIGN'
       * which brings us right back here!
       */
      if ((VALUE_NONE != baseofs)
          && (!value_is_const(baseofs) || value_const_get(baseofs))) {
          pic_op(pf, OPERATOR_ADD, tmp, src, baseofs);
          pic_op(pf, OPERATOR_ANDB, tmp, tmp, mask);
      } else {
          pic_op(pf, OPERATOR_ANDB, tmp, src, mask);
      }
      value_def_set(src, src_def);
      value_release(mask);
      pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
      sz = pic_result_sz_get(src, VALUE_NONE, dst);
      if (pic_is_16bit(pf)) {
        value_t tablat;

        tablat = pfile_value_find(pf, PFILE_LOG_ERR, "_tablat");
        for (ii = 0; ii < sz; ii++) {
          pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
          pic_instr_append_w_kn(pf, PIC_OPCODE_TBLRD, PIC_TBLPTR_CHANGE_POST_INC);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tablat, 0, PIC_OPDST_W);
          if (VALUE_NONE != dst) {
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
          }
        }
        value_release(tablat);
        value_release(tmp);
        tmp = VALUE_NONE;
      } else {
        label_t       lbl;

        lbl = pic_label_find(pf, PIC_LABEL_INDIRECT, BOOLEAN_TRUE);
        for (ii = 0; ii < sz; ii++) {
          if (ii) {
            pic_op(pf, OPERATOR_INCR, tmp, VALUE_NONE, VALUE_NONE);
          }
          pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 
            pic_pointer_size_get(pf) - 1, PIC_OPDST_W);
          pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
          if (!pic_value_is_w(dst)) {
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
          }
        }
        label_release(lbl);
      }
      pic_value_sign_extend(pf, dst, ii - 1, value_is_signed(src));
    }
    value_release(src);
    if (VALUE_NONE != tmp) {
      pic_var_pointer_release(pf, tmp);
    }
  }
}

void pic_assign_to_float(pfile_t *pf, value_t dst, value_t src)
{

  if (value_is_const(src)) {
    value_t tmp;

    tmp = pfile_constant_float_get(pf, value_const_get(src), 
      value_def_get(dst));
    pic_op(pf, OPERATOR_ASSIGN, dst, tmp, VALUE_NONE);
    value_release(tmp);
  } else {
    pic_var_float_t flt;
    pic_integer_to_float(pf, src);
    pic_var_float_get(pf, &flt);
    pic_op(pf, OPERATOR_ASSIGN, dst, flt.fval1, VALUE_NONE);
    pic_var_float_release(pf, &flt);
  }
}

void pic_assign_from_float(pfile_t *pf, value_t dst, value_t src)
{
  value_t       fconv;
  variable_sz_t sz;

  sz = value_sz_get(dst);
  if (value_is_bit(dst)) {
    sz = (sz + 7) / 8;
  }
  fconv = pic_var_float_conv_get(pf, sz);
  pic_float_to_integer(pf, src);
  pic_op(pf, OPERATOR_ASSIGN, dst, fconv, VALUE_NONE);
  value_release(fconv);
}

/* True if `sv` is a true ARRAY variable (not INTEGER lowered const tables). */
boolean_t pic_variable_is_array_table(variable_t sv)
{
  variable_def_t sdef;

  if (!sv || VARIABLE_NONE == sv) {
    return BOOLEAN_FALSE;
  }
  sdef = variable_def_get(sv);
  return sdef && VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(sdef);
}

/* `const byte buf[N]` may use VARIABLE_DEF_TYPE_INTEGER + CONST on the variable. */
boolean_t pic_variable_is_rom_int_const_table(variable_t sv)
{
  variable_def_t sdef;
  const char     *nm;

  if (!sv || VARIABLE_NONE == sv) {
    return BOOLEAN_FALSE;
  }
  nm = variable_name_get(sv);
  if (!nm || !nm[0]) {
    /* Anonymous const temps (e.g. literal `7` as a wide INTEGER) are not ROM
     * tables for `p is buf`; they must not satisfy arraylike-for-bind. */
    return BOOLEAN_FALSE;
  }
  sdef = variable_def_get(sv);
  if (!sdef) {
    return BOOLEAN_FALSE;
  }
  /* Exclude plain numeric constants: they use INTEGER+CONST with sz>1 but have
   * no initializer bytes / lookup — only true ROM tables do (e.g. const byte
   * buf[N] lowered to INTEGER), else `slot[0] = 7` mis-triggers array-to-pointer. */
  return (VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(sdef)
      && variable_def_flag_test(sdef, VARIABLE_DEF_FLAG_CONST)
      && variable_def_sz_get(sdef) > 1
      && variable_is_const(sv)
      && !variable_is_pointer(sv)
      && (variable_data_get(sv) || variable_is_lookup(sv)));
}

/* True if dst names GPR slot(s) for a data pointer (`var pointer …`). */
boolean_t pic_dst_backs_pointer_storage(value_t dst)
{
  variable_t         v;
  variable_def_t    vd;
  variable_def_member_t mbr;
  variable_def_t    em;

  if (!dst) {
    return BOOLEAN_FALSE;
  }
  if (value_is_pointer(dst)) {
    return BOOLEAN_TRUE;
  }
  /* `value_dereference` leaves `VALUE_FLAG_POINTER` while `value_type_get` is
   * the pointee (not VARIABLE_DEF_TYPE_POINTER on the value). */
  if (value_flag_test(dst, VALUE_FLAG_POINTER)) {
    return BOOLEAN_TRUE;
  }
  v = value_variable_get(dst);
  if (!v || VARIABLE_NONE == v) {
    return BOOLEAN_FALSE;
  }
  vd = variable_def_get(v);
  if (!vd) {
    return BOOLEAN_FALSE;
  }
  if (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(vd)) {
    return BOOLEAN_TRUE;
  }
  if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(vd)) {
    mbr = variable_def_member_get(vd);
    em  = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
    if (em && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(em)) {
      return BOOLEAN_TRUE;
    }
    /* Folded `var pointer const byte p[N]`: outer ARRAY, inner ARRAY(pointee). */
    if (em && VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(em)) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* True if src is an array aggregate (value or variable def is ARRAY). */
boolean_t pic_src_is_data_array(const value_t src)
{
  variable_t sv;

  if (!src) {
    return BOOLEAN_FALSE;
  }
  /* Folded `p is buf` can lower the RHS to VARIABLE_DEF_TYPE_VALUE (aggregate)
   * while VALUE_FLAG_ARRAY remains set from the array lvalue (value.c). */
  if (value_flag_test(src, VALUE_FLAG_ARRAY)) {
    return BOOLEAN_TRUE;
  }
  if (value_is_array(src)) {
    return BOOLEAN_TRUE;
  }
  sv = value_variable_get(src);
  /* Do not reject on value_is_pointer(src): const ROM arrays can still test as
   * a pointer value (lookup) while variable_def_get(sv) remains ARRAY. */
  return pic_variable_is_array_table(sv);
}

boolean_t pic_try_src_arraylike_for_pointer_bind(const value_t src)
{
  if (pic_src_is_data_array(src)) {
    return BOOLEAN_TRUE;
  }
  return pic_variable_is_rom_int_const_table(value_variable_get(src));
}

/* `p is buf` may reach here as OPERATOR_ASSIGN with dst already dereferenced
 * (*p in the cmd stream) while the backing variable is still a POINTER def.
 * pic_assign_array_to_pointer needs a non-indirect value typed as pointer. */
boolean_t pic_try_bind_array_to_pointer_slot(pfile_t *pf, value_t dst,
    value_t src)
{
  variable_t      vbind;
  value_t         ptr_slot;
  value_t         src_use;
  boolean_t       src_use_extra;

  if (!pic_dst_backs_pointer_storage(dst)) {
    return BOOLEAN_FALSE;
  }

  /* Before `src_use` / name resolution: `pic_try_src_arraylike_for_pointer_bind`
   * can be true for a plain scalar (VALUE_FLAG_ARRAY) even when `variable_t` is
   * missing — do not route `slot[i] is v` through pic_assign_array_to_pointer. */
  if (pic_try_src_arraylike_for_pointer_bind(src)) {
    variable_t sx;

    sx = value_variable_get(src);
    if (!sx || VARIABLE_NONE == sx) {
      if (!pic_src_is_data_array(src)) {
        return BOOLEAN_FALSE;
      }
      /* VALUE_FLAG_ARRAY alone can be set on a 1-byte scalar; require real size. */
      if ((1 == value_sz_get(src)) && !value_is_array(src)) {
        return BOOLEAN_FALSE;
      }
    } else if (!pic_variable_is_array_table(sx)
        && !pic_variable_is_rom_int_const_table(sx)) {
      return BOOLEAN_FALSE;
    }
  }

  src_use        = src;
  src_use_extra  = BOOLEAN_FALSE;
  ptr_slot       = VALUE_NONE;

  if (!pic_try_src_arraylike_for_pointer_bind(src)) {
    /* `p is buf`: the RHS can be lowered to a VALUE aggregate whose
     * variable_t link is gone, so pic_src_is_data_array misses while the
     * symbol still names a const array — resolve by name for address-of. */
    const char     *snm;
    variable_t      vsrc;

    snm = value_name_get(src);
    if (!snm || !snm[0]) {
      return BOOLEAN_FALSE;
    }
    vsrc = pfile_variable_find(pf, PFILE_LOG_NONE, snm, 0);
    if (!vsrc || VARIABLE_NONE == vsrc) {
      return BOOLEAN_FALSE;
    }
    if (!pic_variable_is_array_table(vsrc)
        && !pic_variable_is_rom_int_const_table(vsrc)) {
      variable_release(vsrc);
      return BOOLEAN_FALSE;
    }
    src_use = value_alloc(vsrc);
    variable_release(vsrc);
    if (!src_use) {
      return BOOLEAN_FALSE;
    }
    src_use_extra = BOOLEAN_TRUE;
  }
  if (value_is_pointer(dst)) {
    ptr_slot = value_clone(dst);
    value_baseofs_set(ptr_slot, VALUE_NONE);
    value_indirect_clear(ptr_slot);
  } else if ((vbind = value_variable_get(dst)) != VARIABLE_NONE) {
    variable_def_t      vdef;
    variable_def_type_t vdt;

    vdef = variable_def_get(vbind);
    vdt  = vdef ? variable_def_type_get(vdef) : VARIABLE_DEF_TYPE_NONE;
    if (VARIABLE_DEF_TYPE_POINTER == vdt) {
      ptr_slot = value_alloc(vbind);
    } else if (VARIABLE_DEF_TYPE_ARRAY == vdt) {
      variable_def_member_t mbr;
      variable_def_t        em;

      mbr = variable_def_member_get(vdef);
      em  = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
      if (em && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(em)) {
        ptr_slot = value_alloc(vbind);
      } else if (em && VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(em)) {
        ptr_slot = value_alloc(vbind);
      }
    }
  }
  if (!ptr_slot) {
    if (src_use_extra) {
      value_release(src_use);
    }
    return BOOLEAN_FALSE;
  }
  pic_assign_array_to_pointer(pf, ptr_slot, src_use);
  value_release(ptr_slot);
  if (src_use_extra) {
    value_release(src_use);
  }
  return BOOLEAN_TRUE;
}

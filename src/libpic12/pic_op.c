/************************************************************
 **
 ** pic_op.c : PIC operator generation definitions
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **           (c) 2020-2025, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
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
/* sometimes it's not necessary to mask the bits while shifting
 * because they are masked elsewhere. assign to/from bit fields
 * is a good example of this */
#define PIC_SHIFT_FLAG_NONE      0x0000
#define PIC_SHIFT_FLAG_MASK_BITS 0x0001
static void pic_shift_right(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2, unsigned flags);
static void pic_shift_left(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2, unsigned flags);
static boolean_t pic_variable_is_array_table(variable_t sv);
static boolean_t pic_variable_is_rom_int_const_table(variable_t sv);
static boolean_t pic_dst_backs_pointer_storage(value_t dst);

typedef struct pic_dynamic_bit_ref_ {
  value_t byte;
  value_t mask;
  value_t byte_index;
  value_t bit_index;
} pic_dynamic_bit_ref_t;
static boolean_t pic_value_is_dynamic_bit_array_element(value_t val);
static boolean_t pic_dynamic_bit_load_to_w(pfile_t *pf, value_t src);

/*
 * these are used to prevent duplicate calls to _multiply or _divide
 * for example:
 *    ch = n % 10
 *    n  = n / 10
 * they are reset when a label or branch is encountered
 * also if any of the values are used as a destination
 */
typedef enum pic_last_value_ {
  PIC_LAST_VALUE_FIRST,
  PIC_LAST_VALUE_DIVISOR = PIC_LAST_VALUE_FIRST,
  PIC_LAST_VALUE_DIVIDEND,
  PIC_LAST_VALUE_MULTIPLIER,
  PIC_LAST_VALUE_MULTIPLICAND,
  PIC_LAST_VALUE_CT
} pic_last_value_t;

static value_t pic_last_values[PIC_LAST_VALUE_CT];

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

static boolean_t pic_value_is_w(value_t val)
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
static variable_sz_t pic_result_sz_get(value_t val1, value_t val2, 
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
static flag_t pic_result_flag_get(pfile_t *pf, value_t val1, value_t val2)
{
  variable_def_t rdef;

  rdef = pfile_variable_def_promotion_get(pf, OPERATOR_NULL, val1, val2);
  return variable_def_flag_test(rdef, VARIABLE_DEF_FLAG_SIGNED)
    ? VARIABLE_DEF_FLAG_SIGNED
    : VARIABLE_DEF_FLAG_NONE;
}

static boolean_t pic_result_is_signed(pfile_t *pf, value_t val1, 
    value_t val2)
{
  return VARIABLE_DEF_FLAG_SIGNED == pic_result_flag_get(pf, val1, val2);
}

/* 
 * allocate a pic temp variable & assign val to it. The size of the result
 * is less{dst, greater{val1, val2}}
 */
static value_t pic_var_temp_get_assign(pfile_t *pf, value_t val, value_t val2,
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
static uchar pic_value_const_byte_get(const value_t val, unsigned ofs)
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

value_t pic_fsr_get(pfile_t *pf)
{
  pic_stvar_fsr_mark(pf);
  return pfile_value_find(pf, PFILE_LOG_ERR, 
    (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
    ? "_fsr0l"
    : "_fsr");
}

void pic_fsr_setup(pfile_t *pf, value_t val)
{
  pic_stvar_fsr_mark(pf);
  if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
    /* Check if this is a function parameter that should use direct addressing */
    variable_t var = value_variable_get(val);
    if (var) {
      const char *name = variable_name_get(var);
      /* Skip FSR0 for function parameters, preserve for actual pointer operations */
      if (name && (strstr(name, "_device_put") ||
                   strstr(name, "_str_") ||
                   strstr(name, "___str"))) {
        /* Skip FSR0 for specific string-parameter hacks; do not use strstr(..., "str")
         * — it matches identifiers like string1/string2 and breaks FSR0 for indexing. */
        return;
      }
    }
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 1, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 0, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
  } else {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 0, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 1, PIC_OPDST_F);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
  }
}

static variable_sz_t pic_dynamic_index_byte_sz_get(value_t idx)
{
  variable_sz_t idx_sz;

  idx_sz = value_byte_sz_get(idx);
  return (idx_sz < 1) ? 1 : idx_sz;
}

static boolean_t pic_variable_chunks_holed(variable_t var);
static void pic_fsr_chunk_walk_emit_ofs(pfile_t *pf, variable_t var,
    value_t idx, size_t offset, boolean_t hybrid);
static void pic_fsr_chunk_walk_emit(pfile_t *pf, variable_t var,
    value_t idx, boolean_t hybrid);

static void pic_fsr0_add_dynamic_index(pfile_t *pf, value_t idx,
    value_t array_val)
{
  variable_sz_t idx_sz;
  variable_t    walk_var;

  walk_var = value_variable_get(array_val);
  if (!pic_variable_chunks_holed(walk_var)) {
    /* unescaped pointer bind: walk the TARGET's chunks */
    walk_var = pic_var_pointer_bind_walk_target(walk_var);
  }
  if (pic_variable_chunks_holed(walk_var)) {
    pic_fsr_chunk_walk_emit(pf, walk_var, idx, BOOLEAN_TRUE);
    return;
  }
  idx_sz = pic_dynamic_index_byte_sz_get(idx);
  /* the subscript byte offset cannot exceed the array's span, so an
   * array of at most 256 bytes has a single-byte index however wide
   * the index expression's type is (e.g. word `address / 2` into a
   * byte[256] sector buffer). Narrowing drops the high-byte
   * addwfc/addwf fsr0h pair per access site; the carry fixup below
   * keeps an unaligned base correct. */
  {
    variable_t array_var;

    array_var = value_variable_get(array_val);
    if (array_var) {
      variable_sz_t span;

      /* the SPAN must come from the array variable's def, not the
       * subscripted value's def: `arr[i]` carries the ELEMENT view
       * (storage need 1), which made this guard see span==1 for every
       * array and take the carry-free fast path whenever the BASE fit
       * a 256 window -- base+index then wrapped into low RAM (the
       * PIC18 FSR-carry defect, docs/investigations/
       * pic18_runtime_index_fsr_carry_20261005.md) */
      span = pic_variable_def_storage_need_get(pf,
          variable_def_get(array_var), 0);
      if (span && (span <= 256)) {
        idx_sz = 1;
        /* when the allocated base is known and the whole array fits
         * below the next 256 boundary, base_lo + idx cannot carry
         * into FSR0H and the fixup is dead (checked at emission:
         * bases are known from generate pass 2 on) */
        {
          variable_base_t base;

          base = value_base_get(array_val);
          if ((VARIABLE_BASE_UNKNOWN != base)
              && (((base & 0xFFU) + span) <= 256U)) {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
            pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0l",
                PIC_OPDST_F);
            return;
          }
        }
      }
    }
  }
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
  pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0l", PIC_OPDST_F);
  if (idx_sz > 1) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWFc, idx, 1, PIC_OPDST_W);
    pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0h", PIC_OPDST_F);
  } else {
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
  }
}

static void pic_classic_irp_set_from_w(pfile_t *pf)
{
  value_t hi;

  hi = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  if (!hi) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, hi, 0);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
  pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, hi, 0, 0);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
  pic_var_temp_release(pf, hi);
}

/* Layer 2: a variable placed across bank holes (non-adjacent chunks).
 * Contiguous multi-node runs (PIC18 bank splits) are still linear. */
static boolean_t pic_variable_chunks_holed(variable_t var)
{
  variable_ct_t ct;
  variable_ct_t ii;

  ct = var ? pic_variable_chunk_ct_get(var) : 0;
  if (ct <= 1) {
    return BOOLEAN_FALSE;
  }
  for (ii = 1; ii < ct; ii++) {
    if (pic_variable_chunk_base_get(var, ii)
        != (variable_base_t) (pic_variable_chunk_base_get(var, ii - 1)
          + pic_variable_chunk_sz_get(var, ii - 1))) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* Layer 2: holed linear arrays -- walk the chunk map at run time. The
 * byte offset is copied to a scratch (the walk consumes it: compare,
 * then subtract the chunk size), and each chunk branch computes the
 * element address completely before jumping to done. Chunk sizes are
 * bank-node bounded (< 256), so a byte compare with a high-byte
 * zero-test decides membership; the subtract rides the generic
 * OPERATOR_SUB emitter (carry-correct for wide offsets). Modeled on
 * the dynamic-bit chunk walk (pic_dynamic_bit_ref_addr_setup). */
static void pic_fsr_chunk_walk_emit(pfile_t *pf, variable_t var,
    value_t idx, boolean_t hybrid)
{
  pic_fsr_chunk_walk_emit_ofs(pf, var, idx, 0, hybrid);
}

/* offset-aware walk: the scratch t is initialized to idx + offset (the
 * const part folded out of a pointer-view subscript, e.g. p[256 + i]).
 * W-staged values around the caller are NOT touched -- the offset add
 * rides the walk's own scratch (found the hard way: emitting pic_op
 * arithmetic mid-addressing destroyed the staged RHS of the store). */
static void pic_fsr_chunk_walk_emit_ofs(pfile_t *pf, variable_t var,
    value_t idx, size_t offset, boolean_t hybrid)
{
  variable_ct_t  ct;
  variable_ct_t  ii;
  variable_sz_t  idx_sz;
  unsigned       t_wide;
  value_t        t;
  label_t        done;

  ct = pic_variable_chunk_ct_get(var);
  idx_sz = pic_dynamic_index_byte_sz_get(idx);
  t_wide = ((idx_sz > 1) || (offset > 0xFFU)) ? 1 : 0;
  t = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE,
      t_wide ? 2 : 1);
  if (!t) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 0);
  if (idx_sz > 1) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 1, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 1);
  } else if (offset > 0xFFU) {
    pic_instr_append_f(pf, PIC_OPCODE_CLRF, t, 1);
  }
  if (offset) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
        (unsigned) (offset & 0xFFU));
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 0, PIC_OPDST_F);
    if ((idx_sz > 1) || (offset > 0xFFU)) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, t, 1, PIC_OPDST_F);
      if (offset > 0xFFU) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
            (unsigned) ((offset >> 8) & 0xFFU));
        pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 1, PIC_OPDST_F);
      }
    }
  }

  done = pfile_label_alloc(pf, 0);
  for (ii = 0; ii < ct; ii++) {
    variable_sz_t    sz;
    variable_base_t  cb;
    label_t          next;
    value_t          cval;

    sz = pic_variable_chunk_sz_get(var, ii);
    cb = pic_variable_chunk_base_get(var, ii);
    next = LABEL_NONE;
    if (ii + 1 < ct) {
      next = pfile_label_alloc(pf, 0);
      if (idx_sz > 1) {
        /* offset >= 256 cannot be in this (< 256 byte) chunk */
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 1, PIC_OPDST_W);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, next);
      }
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sz);
      pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, t, 0, PIC_OPDST_W);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, next);
    }
    if (hybrid) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, cb & 0xff);
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (cb >> 8) & 0xff);
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 0, PIC_OPDST_W);
      pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0l", PIC_OPDST_F);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
      if (t_wide) {
        /* a single-chunk map never runs the subtract ladder, so t can
         * still hold the FULL (>= 256) linear index -- its high byte
         * belongs in fsr0h (ladder-reduced chunks carry t+1 == 0 and
         * the add is a no-op) */
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 1, PIC_OPDST_W);
        pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0h",
            PIC_OPDST_F);
      }
    } else {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, cb & 0xff);
      pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 0, PIC_OPDST_W);
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
      /* IRP = (chunk base >= 0x100) OR carry of the low-byte add: a
       * chunk in banks 2/3 needs IRP set even when the low-byte sum
       * does not carry (base 0x110 + offset 0x19 computed 0x23 with
       * IRP left clear and landed in bank 0 -- found by the 16f877
       * >256-byte runtime witness, 2026-10-26) */
      if (cb & 0x100) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
      } else {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
      }
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
      if (t_wide) {
        /* full linear index still in t (single-chunk map): IRP takes
         * t's high byte too */
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 1, PIC_OPDST_W);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
      }
    }
    if (LABEL_NONE != next) {
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);
      pic_instr_append_label(pf, next);
      cval = pfile_constant_get(pf, sz, VARIABLE_DEF_NONE);
      if (cval) {
        pic_op(pf, OPERATOR_SUB, t, t, cval);
        value_release(cval);
      } else {
        pfile_log_syserr(pf, ENOMEM);
      }
      label_release(next);
    }
  }
  pic_instr_append_label(pf, done);
  label_release(done);
  pic_var_temp_release(pf, t);
}

/* idx_range: statically-known byte span the index may legally cover
 * (array byte size minus element offset); 0 = unknown. var: the array
 * variable (VARIABLE_NONE for pointer-slot storage that has no chunk
 * map of its own). */
void pic_classic_fsr_load_base_dynamic_index(pfile_t *pf,
    variable_t var, variable_base_t base, value_t idx,
    variable_sz_t idx_range)
{
  variable_sz_t idx_sz;
  boolean_t     wide16;

  if (!pic_variable_chunks_holed(var)) {
    var = pic_var_pointer_bind_walk_target(var);
  }
  if (pic_variable_chunks_holed(var)) {
    pic_fsr_chunk_walk_emit(pf, var, idx, BOOLEAN_FALSE);
    return;
  }
  idx_sz = pic_dynamic_index_byte_sz_get(idx);
  wide16 = pic_is_16bit(pf) || pic_is_14bit_hybrid(pf);
  /* const index: fold into the base and use a literal high half (ADDWF
   * with a const value would read the const's value as a REGISTER
   * address -- offset 2 reads PCL) */
  if (value_is_const(idx)) {
    variable_base_t abs_addr;

    abs_addr = base + (variable_base_t) value_const_get(idx);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
        (unsigned) (abs_addr & 0xff));
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF,
        wide16 ? "_fsr0l" : "_fsr");
    if (wide16) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (unsigned) ((abs_addr >> 8) & 0xff));
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
    } else {
      pic_instr_append(pf, (abs_addr >= 256)
          ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
    }
    return;
  }
  /* Static-window fast path: a byte-sized index whose legal range keeps
   * base+index inside the first 256-byte FSR window needs no 16-bit
   * carry chain and a statically clear IRP.  Out-of-range subscripts
   * are undefined (no bounds checking), so the range precondition is
   * the language contract; jalv25r9 emitted this exact shape. */
  if ((1 == idx_sz) && (idx_range > 0)
      && ((unsigned long) base + (unsigned long) idx_range) <= 0x100UL) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, idx, 0, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF,
        wide16 ? "_fsr0l" : "_fsr");
    if (wide16) {
      pic_instr_append_reg(pf, PIC_OPCODE_CLRF, "_fsr0h");
    } else {
      pic_instr_append(pf, PIC_OPCODE_IRP_CLR);
    }
    return;
  }
  pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
  pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, idx, 0, PIC_OPDST_W);
  pic_instr_append_reg(pf, PIC_OPCODE_MOVWF,
      wide16 ? "_fsr0l" : "_fsr");
  pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
  pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
  if (idx_sz > 1) {
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, idx, 1, PIC_OPDST_W);
  }
  if (wide16) {
    /* W holds the computed high byte; the 16-bit FSR needs it stored,
     * not an IRP latch (there is none on these cores) */
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
  } else {
    pic_classic_irp_set_from_w(pf);
  }
}

/*
 * NAME
 *   pic_indirect_setup
 *
 * DESCRIPTION
 *   setup irp:fsr
 *
 * PARAMETERS
 *   pf    : file
 *   val   : value
 *   offset: offset from start of value
 *
 * RETURN
 *
 * NOTES
 *   1. A value is assumed to not span banks
 *   2. Normally this is called if val.baseofs is non-const
 */
static void pic_op_assign(pfile_t *pf, operator_t op, value_t dst,
    value_t src, value_t val2);

/* PIC14 lookup CALL needs the table index in W. pic_instr_append_f_d(MOVF)
 * is only correct for a plain file register; indexed RAM (INDF / v__ind)
 * needs FSR setup from pic_op_assign (nested index into var array as lookup
 * subscript — GitHub jalv2compiler #40). */
static boolean_t pic_lookup_index_ok_for_movf(value_t baseofs)
{
  variable_t v;

  if (!baseofs) {
    return BOOLEAN_FALSE;
  }
  if (value_is_bit(baseofs) || value_is_indirect(baseofs)) {
    return BOOLEAN_FALSE;
  }
  if (value_flag_test(baseofs, VALUE_FLAG_POINTER)) {
    return BOOLEAN_FALSE;
  }
  v = value_variable_get(baseofs);
  if (!v || VARIABLE_NONE == v) {
    return BOOLEAN_FALSE;
  }
  if (VALUE_NONE != value_baseofs_get(baseofs)) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

static void pic_lookup_index_load_w(pfile_t *pf, value_t baseofs)
{
  if (value_is_bit(baseofs)) {
    pic_op_assign(pf, OPERATOR_ASSIGN, VALUE_NONE, baseofs, VALUE_NONE);
  } else if (pic_lookup_index_ok_for_movf(baseofs)) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, baseofs, 0, PIC_OPDST_W);
  } else {
    pic_op_assign(pf, OPERATOR_ASSIGN, VALUE_NONE, baseofs, VALUE_NONE);
  }
}

/* True array-of-pointers (e.g. **`var pointer var pointer var byte x[N]`**):
 * outer def is ARRAY of POINTER; FSR must load from the pointer *slot* words,
 * not linear *slot+i pointee math (MOVLW-on-indirect **`v__ind`** bug on PIC14).
 * Folded **`var pointer var byte p[N]`** is POINTER→ARRAY (one pointer slot). */
static boolean_t pic_value_variable_is_array_of_pointers(const value_t val)
{
  variable_t              v;
  variable_def_t        vd;
  variable_def_member_t   mbr;
  variable_def_t        em;

  v = value_variable_get(val);
  if (!v || VARIABLE_NONE == v) {
    return BOOLEAN_FALSE;
  }
  vd = variable_def_get(v);
  if (!vd || VARIABLE_DEF_TYPE_ARRAY != variable_def_type_get(vd)) {
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(vd);
  em  = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (em && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(em));
}

static boolean_t pic_value_is_array_pointer_slot_storage(const value_t val)
{
  return value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW);
}

/* True if we should emit a runtime "all slot bytes zero" check before loading
 * FSR from a pointer variable. Skips legacy string / device_put paths that
 * are not normal `var pointer` GPR slots. */
static boolean_t pic_pointer_runtime_null_guard_eligible(variable_t v)
{
  const char *nm;

  if (!v || VARIABLE_NONE == v || !variable_is_pointer(v)) {
    return BOOLEAN_FALSE;
  }
  nm = variable_name_get(v);
  if (nm && (strstr(nm, "_device_put")
      || strstr(nm, "_str_")
      || strstr(nm, "___str"))) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

/*
 * If every byte of the pointer slot at base[slot_offs..] is zero, jump to a
 * local infinite loop (never uses a bogus FSR). Z=1 after IOR means all bytes
 * read as 0 (typical cleared BSS / never bound). BTFSS STATUS,Z skips the
 * `goto lbl_ok` when Z=1 (all slot bytes zero) so execution falls into hang.
 */
static void pic_pointer_slot_emit_null_guard(pfile_t *pf, value_t base,
    variable_sz_t slot_offs)
{
  label_t       lbl_hang;
  label_t       lbl_ok;
  variable_sz_t psz;
  variable_sz_t jj;
  variable_t    v;

  v = value_variable_get(base);
  if (!pic_pointer_runtime_null_guard_eligible(v)) {
    return;
  }
  lbl_hang = pfile_label_alloc(pf, 0);
  lbl_ok   = pfile_label_alloc(pf, 0);
  psz = pic_pointer_size_get(pf);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, base, slot_offs + 0, PIC_OPDST_W);
  for (jj = 1; jj < psz; jj++) {
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, base, slot_offs + jj,
        PIC_OPDST_W);
  }
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ok);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_hang);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_ok);
  label_release(lbl_hang);
  label_release(lbl_ok);
}

/* Dereferenced `var pointer … as var string`: pointee is STRING_DYNAMIC.
 * Indexing the handle must load FSR from the pointer slot (`*p`+field), not
 * LFSR to the slot's file address (`p`+field). */
static boolean_t pic_value_indirect_heap_string_through_pointer(const value_t val)
{
  variable_t            v;
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        pt;

  if (!value_is_indirect(val) || !value_flag_test(val, VALUE_FLAG_POINTER)) {
    return BOOLEAN_FALSE;
  }
  v = value_variable_get(val);
  if (!v || !variable_is_pointer(v)) {
    return BOOLEAN_FALSE;
  }
  def = variable_def_get(v);
  if (!def) {
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(def);
  if (!mbr) {
    return BOOLEAN_FALSE;
  }
  pt = variable_def_member_def_get(mbr);
  return (pt && variable_def_flag_test(pt,
      VARIABLE_DEF_FLAG_STRING | VARIABLE_DEF_FLAG_STRING_DYNAMIC))
      ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
}

void pic_indirect_setup(pfile_t *pf, value_t val, size_t offset)
{
  value_t baseofs;

  pic_stvar_fsr_mark(pf);
  baseofs = value_baseofs_get(val);
  if (value_is_bit(val)) {
    offset += (value_bit_offset_get(val) + 7) / 8;
  }

  if (value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    variable_base_t base;
    value_t         fsr;

    fsr = pic_fsr_get(pf);
    base = value_base_get(val) + offset;
    if (value_is_const(baseofs)) {
      base += (variable_base_t) value_const_get(baseofs);
      base = pic_var_chunk_addr_resolve(value_variable_get(val), base);
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, value_const_get(baseofs));
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    } else {
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, 0);
        if (VALUE_NONE != baseofs) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
        if (VALUE_NONE != baseofs) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else if (VALUE_NONE != baseofs) {
        pic_classic_fsr_load_base_dynamic_index(pf,
            value_variable_get(val), base, baseofs, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    }
    value_release(fsr);
    return;
  }

  if (value_is_indirect(val) || value_flag_test(val, VALUE_FLAG_POINTER)) {
    /* STATUS<IRP>:FSR = *value. Const-pointee indexed reads can retain POINTER
     * while INDIRECT is clear (baseofs is const 0), so value_is_indirect alone
     * would skip this block and emit movf INDF before FSR is loaded. */
    value_t tmp;

    tmp = value_clone(val);
    value_baseofs_set(tmp, VALUE_NONE);
    {
      /* chunk-walked addressing: an open-array formal carrying a
       * propagated chunk map (a direct call specialized to a chunked
       * actual), or an UNESCAPED pointer bind whose target is chunked
       * (the pointer is only ever dereferenced, so every access can
       * walk the target's chunks) -- the flat home base / linear
       * base+index cannot address across the holes */
      variable_t walk_var;
      boolean_t  formal_map;

      formal_map = pic_variable_chunks_holed(value_variable_get(val));
      if (!formal_map
          && (VALUE_NONE != baseofs) && value_is_const(baseofs)
          && (pic_var_pointer_bind_walk_target(
                value_variable_get(val)) != VARIABLE_NONE)) {
        /* const subscript through an unescaped walked bind: resolve
         * the linear offset through the TARGET's chunk map and load
         * the absolute address (the home-slot base path below would
         * address linearly across the holes) */
        variable_t wt;
        variable_base_t linear;
        variable_base_t phys;

        wt = pic_var_pointer_bind_walk_target(value_variable_get(val));
        linear = variable_base_get(wt, 0)
          + (variable_base_t) value_const_get(baseofs)
          + (variable_base_t) offset;
        phys = pic_var_chunk_addr_resolve(wt, linear);
        if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (unsigned) (phys & 0xff));
          pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (unsigned) ((phys >> 8) & 0xff));
          pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (unsigned) (phys & 0xff));
          pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
          pic_instr_append(pf, (phys >= 256)
              ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
        }
        value_release(tmp);
        return;
      }
      if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
          && (formal_map
            || (pic_var_pointer_bind_walk_target(
                  value_variable_get(val)) != VARIABLE_NONE))) {
        /* const baseofs on a map-carrying formal falls through to the
         * const path below, which resolves through the chunk map */
        walk_var = formal_map
          ? value_variable_get(val)
          : pic_var_pointer_bind_walk_target(value_variable_get(val));
        pic_fsr_chunk_walk_emit_ofs(pf, walk_var, baseofs, offset,
            pic_is_16bit(pf) || pic_is_14bit_hybrid(pf));
        value_release(tmp);
        return;
      }
    }

    /* `var pointer … p` then `p[i]` (pointee is array): linear *p+index.
     * True **`ARRAY` of `POINTER`** (nested data pointer): each element is a
     * pointer slot; use the same path as a plain subscripted pointer.
     * Heap `var string` through `var pointer … as var string`: see
     * `pic_value_indirect_heap_string_through_pointer`. */
    if (value_flag_test(val, VALUE_FLAG_ARRAY)
        && !pic_value_indirect_heap_string_through_pointer(val)
        && (!pic_value_variable_is_array_of_pointers(val)
          || pic_value_is_array_pointer_slot_storage(val))) {
      pic_opcode_t op;

      if (value_is_const(baseofs)) {
        offset += value_const_get(baseofs);
      }
      value_baseofs_set(tmp, VALUE_NONE);
      {
        value_t gtmp;

        gtmp = value_clone(tmp);
        value_baseofs_set(gtmp, VALUE_NONE);
        value_flag_clr(gtmp, VALUE_FLAG_POINTER);
        value_flag_clr(gtmp, VALUE_FLAG_INDIRECT);
        if (!pic_value_is_array_pointer_slot_storage(val)) {
          pic_pointer_slot_emit_null_guard(pf, gtmp, 0);
        }
        value_release(gtmp);
      }
      if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
        if (pic_is_16bit(pf)) {
          pic_instr_append_lfsr(pf, 0, tmp, offset);
        } else {
          unsigned char lsb;
          unsigned char msb;
          variable_base_t base;

          base = pic_var_chunk_addr_resolve(value_variable_get(tmp),
              value_base_get(tmp) + offset);
          lsb = (uchar) (base & 0xff);
          msb = (uchar) (base >> 8);

          if (!lsb) {
            pic_instr_append_reg(pf, PIC_OPCODE_CLRF, "_fsr0l");
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, lsb);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
          }
          if (!msb) {
            pic_instr_append_reg(pf, PIC_OPCODE_CLRF, "_fsr0h");
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, msb);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
          }
        }
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else {
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          if (value_is_bit(baseofs)) {
            pic_op_assign(pf, OPERATOR_ASSIGN, VALUE_NONE, baseofs, 
                VALUE_NONE);
            pic_instr_append_f(pf, PIC_OPCODE_ADDLW, tmp, offset);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
            pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
            pic_classic_irp_set_from_w(pf);
          } else {
            variable_sz_t range;

            range = (variable_sz_t) (value_byte_sz_get(tmp) - offset);
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp),
                value_base_get(tmp) + offset, baseofs, range);
          }
        } else {
            pic_instr_append_f(pf, PIC_OPCODE_MOVLW, tmp, offset);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
            op = (value_base_get(tmp) >= 256)
              ? PIC_OPCODE_IRP_SET
              : PIC_OPCODE_IRP_CLR;
            pic_instr_append(pf, op);
        }
      }
    } else {
      /* assert(value_is_pointer(val)); */
      value_flag_clr(tmp, VALUE_FLAG_INDIRECT);
      /* Element is typed as the pointee; POINTER stays set and makes
       * value_is_indirect() true (baseofs is often const 0). pic_instr_append_f_d
       * would then treat tmp as an INDF read (movf 0,w) before FSR is loaded.
       * Clear POINTER so MOV reads the pointer slot (see const-pointee loads). */
      value_flag_clr(tmp, VALUE_FLAG_POINTER);

      {
        variable_sz_t loff;
        boolean_t     slot_array;

        /* `p[i]` / `p`+offset: const baseofs is an index into the *pointee*
         * (MOVF p,0 then ADDLW). `slot[i]` on `var pointer … slot[N]`: const
         * baseofs is a byte offset within the pointer *aggregate* (MOVF slot+2). */
        slot_array = pic_value_variable_is_array_of_pointers(val);
        loff = 0;
        if (slot_array && VALUE_NONE != baseofs && value_is_const(baseofs)) {
          loff = (variable_sz_t) value_const_get(baseofs);
        }
        pic_pointer_slot_emit_null_guard(pf, tmp, loff);
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          if (slot_array) {
            /* ARRAY-of-pointers element at a runtime index: DOUBLE
             * indirection -- FSR = &slot[k], load the slot's pointer
             * bytes, then re-stage FSR (and IRP) from the loaded
             * pointer so the caller's INDF access hits the element's
             * real target. The previous `*slot0 + index` staging used
             * slot 0's VALUE (a bound code address) as the data base
             * (the classic runtime-index read+write defect + the
             * stage-2 call blocker, 2026-10-28). 16-bit core + hybrid
             * variant added 2026-10-28: those cores previously never
             * compiled here (the staging emitted 14-bit _fsr/_irp
             * names) and the generic FSR0 tail re-staged from the
             * ARRAY's own storage -- silently wrong element targets. */
            value_t indv;
            value_t fsrv;
            value_t ptmp;
            boolean_t wide16;

            wide16 = pic_is_16bit(pf) || pic_is_14bit_hybrid(pf);
            indv = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
            fsrv = pic_fsr_get(pf);
            ptmp = pic_var_pointer_get(pf);
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp), value_base_get(tmp),
                baseofs, (variable_sz_t) value_byte_sz_get(tmp));
            /* load the slot's own pointer into the SCRATCH (not tmp --
             * tmp maps to the array's own storage and would clobber
             * slot 0) */
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, indv, 0,
                PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ptmp, 0);
            pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsrv, 0,
                PIC_OPDST_F);
            if (wide16) {
                /* INCF _fsr0l alone cannot cross a 256-byte boundary */
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC,
                    "_status", "_z");
                pic_instr_append_reg(pf, PIC_OPCODE_INCF, "_fsr0h");
            }
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, indv, 0,
                PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ptmp, 1);
            /* FSR = the loaded pointer: fsr0l/fsr0h on wide cores,
             * fsr + IRP-latch from bit 8 on the 14-bit core */
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ptmp, 0,
                PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsrv, 0);
            if (wide16) {
                pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ptmp, 1, 0);
                pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
            } else {
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status",
                    "_irp");
                pic_instr_append_f_d(pf, PIC_OPCODE_BTFSC, ptmp, 1, 0);
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status",
                    "_irp");
            }
            pic_var_pointer_release(pf, ptmp);
            value_release(fsrv);
            value_release(indv);
            /* the caller's INDF access now hits the element's real
             * target; the common FSR/IRP tail below would re-stage
             * from the value's own storage and clobber it */
            value_release(tmp);
            return;
          } else if (variable_is_array(value_variable_get(tmp))) {
            /* plain ARRAY element at a runtime index: the base is the
             * array's own address (MOVLW), not its first element's
             * value (MOVF would read arr[0] as the base) */
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp), value_base_get(tmp),
                baseofs, (variable_sz_t) value_byte_sz_get(tmp));
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 0,
                PIC_OPDST_W);
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 0,
                PIC_OPDST_W);
          }
        } else if (slot_array) {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, loff, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 0, PIC_OPDST_W);
          if (baseofs) {
            if (value_is_const(baseofs)) {
              if (value_const_get(baseofs)) {
                pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW,
                    value_const_get(baseofs));
              }
            } else {
              pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 0,
                  PIC_OPDST_W);
            }
          }
        }
      }
      if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
        /* Check if this is a function parameter that should use direct addressing */
        variable_t var = value_variable_get(val);
        if (var) {
          const char *name = variable_name_get(var);
          /* Skip FSR0 for function parameters, preserve for actual pointer operations */
          if (name && (strstr(name, "_device_put") ||
                       strstr(name, "_str_") ||
                       strstr(name, "___str"))) {
            return;
          }
        }
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
            && !pic_value_variable_is_array_of_pointers(val)) {
          variable_sz_t idx_sz;

          idx_sz = pic_dynamic_index_byte_sz_get(baseofs);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
          if (idx_sz > 1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 1,
                PIC_OPDST_W);
          }
        } else if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
        } else if (pic_value_variable_is_array_of_pointers(val)) {
          variable_sz_t loff;

          loff = 0;
          if (VALUE_NONE != baseofs && value_is_const(baseofs)) {
            loff = (variable_sz_t) value_const_get(baseofs);
          }
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, loff + 1, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
        }
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
      } else {
        variable_sz_t loff;
        variable_sz_t irp_byte;

        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
        loff = 0;
        if (pic_value_variable_is_array_of_pointers(val)
            && VALUE_NONE != baseofs && value_is_const(baseofs)) {
          loff = (variable_sz_t) value_const_get(baseofs);
        }
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
            && !pic_value_variable_is_array_of_pointers(val)) {
          variable_sz_t idx_sz;

          idx_sz = pic_dynamic_index_byte_sz_get(baseofs);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
          if (idx_sz > 1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 1,
                PIC_OPDST_W);
          }
          pic_classic_irp_set_from_w(pf);
        } else {
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
          irp_byte = pic_value_variable_is_array_of_pointers(val)
              ? (variable_sz_t) (loff + pic_pointer_size_get(pf) - 1)
              : (variable_sz_t) (pic_pointer_size_get(pf) - 1);
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, tmp, irp_byte, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
        }
      }
    }
    value_release(tmp);
  } else {
    variable_base_t base;
    value_t         fsr;

    fsr = pic_fsr_get(pf);
    base = value_base_get(val) + offset;
    if (value_is_const(baseofs)) {
      /* fsr = value_base_get(value) + value_const_get(baseofs) + sz */
      base += (variable_base_t) value_const_get(baseofs);
      base = pic_var_chunk_addr_resolve(value_variable_get(val), base);
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, value_const_get(baseofs));
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    } else {
      /* fsr = base (+ sz - 1) */
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, 0);
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
      if (VALUE_NONE != baseofs) {
        if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        } else {
          pic_classic_fsr_load_base_dynamic_index(pf,
              value_variable_get(val), base, baseofs,
              (variable_sz_t) (value_byte_sz_get(val) - offset));
        }
      }
    }
    value_release(fsr);
  }
}

/*
 * given three values, determine which one is indirect. This assumes
 * (obviously) that only one is indirect!
 */
static value_t pic_indirect_value_get(value_t val1, value_t val2, value_t val3)
{
  value_t val;

  if (value_is_indirect(val1)
      || (val1 && value_flag_test(val1, VALUE_FLAG_POINTER))) {
    assert((value_is_same(val1, val2) || !value_is_indirect(val2))
           && (value_is_same(val1, val3) || !value_is_indirect(val3)));
    val = val1;
  } else if (value_is_indirect(val2)) {
    assert(value_is_same(val2, val3) || !value_is_indirect(val3));
    val = val2;
  } else if (value_is_indirect(val3)) {
    val = val3;
  } else {
    val = VALUE_NONE;
  }
  return val;
}

/*
 * given three values (only one of which is indirect) and an offset,
 * setup the irp:ind to the requested offset. This is a no-op
 * if none of the values is indirect
 *   ipos is used to track the relative indirect offset & should
 *        *never* be modified except by pic_indirect_bump3
 */
static void pic_indirect_setup3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ofs, variable_sz_t *ipos)
{
  value_t val;

  val = pic_indirect_value_get(val1, val2, val3);
  if (VALUE_NONE != val) {
    variable_sz_t n;

    n = (ofs >= value_sz_get(val))
      ? value_sz_get(val) - 1
      : ofs;
    pic_indirect_setup(pf, val, n);
  }
  *ipos = ofs;
}

/*
 * if any of the three values is indirect, and the position is in range
 * bump the position in the desired direction
 */
static void pic_indirect_bump3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ix, variable_sz_t *ipos)
{
  value_t val;

  val = pic_indirect_value_get(val1, val2, val3);
  if (VALUE_NONE != val) {
    /* we can only move one in either direction */
    value_t fsr;
    
    assert((ix == *ipos)
        || (ix + 1 == *ipos) 
        || (ix - 1 == *ipos));

    fsr = pic_fsr_get(pf);
    if ((ix > *ipos) && (ix < value_sz_get(val))) {
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, PIC_OPDST_F);
    } else if ((ix < *ipos) && (*ipos < value_sz_get(val))) {
      pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0, PIC_OPDST_F);
    }
    value_release(fsr);
    *ipos = ix;
  }
}

/*
 * return the pic sign value appropriately set if val is signed
 * indirect values are expected to be at the MSB entry.
 * If accum is VALUE_NONE, the returned value will be 
 *   pic_var_accum so pic_var_accum_release() must be used!
 */
static void pic_value_sign_get_in_w(pfile_t *pf, value_t val)
{
  pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
  if (value_is_signed(val)) {
    pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, val, value_sz_get(val) - 1, 7);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 255);
  }
}

static value_t pic_value_sign_get(pfile_t *pf, value_t val, value_t accum)
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
 * determine if two values reside in the same bank
 */
static boolean_t pic_val_bank_same(pfile_t *pf, value_t val1, value_t val2)
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

    if (pic_is_16bit(pf)) {
      unsigned long addr1;
      unsigned long addr2;

      addr1 = (unsigned long) value_base_get(val1);
      if (VARIABLE_BASE_UNKNOWN != value_base_get(val1)) {
        addr1 += (unsigned long) value_const_get(value_baseofs_get(val1));
        addr1 += (unsigned long) (value_bit_offset_get(val1) / 8);
      }
      addr2 = (unsigned long) value_base_get(val2);
      if (VARIABLE_BASE_UNKNOWN != value_base_get(val2)) {
        addr2 += (unsigned long) value_const_get(value_baseofs_get(val2));
        addr2 += (unsigned long) (value_bit_offset_get(val2) / 8);
      }
      rc = ((addr1 / 256UL) == (addr2 / 256UL));
    } else if (pic_is_14bit_hybrid(pf)) {
      unsigned long addr1;
      unsigned long addr2;

      addr1 = (unsigned long) value_base_get(val1);
      if (VARIABLE_BASE_UNKNOWN != value_base_get(val1)) {
        addr1 += (unsigned long) value_const_get(value_baseofs_get(val1));
        addr1 += (unsigned long) (value_bit_offset_get(val1) / 8);
      }
      addr2 = (unsigned long) value_base_get(val2);
      if (VARIABLE_BASE_UNKNOWN != value_base_get(val2)) {
        addr2 += (unsigned long) value_const_get(value_baseofs_get(val2));
        addr2 += (unsigned long) (value_bit_offset_get(val2) / 8);
      }
      rc = ((addr1 / 128UL) == (addr2 / 128UL));
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
 * equals that for v2+ofs2 (same full-file-address /256 or /128 as
 * pic_instr_append_daop). Otherwise false (including classic 14-bit).
 */
static boolean_t pic_val_movl_bank_equal(pfile_t *pf,
    value_t v1, size_t ofs1, value_t v2, size_t ofs2)
{
  variable_base_t b1;
  variable_base_t b2;
  unsigned long   addr1;
  unsigned long   addr2;

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
  if (pic_is_16bit(pf)) {
    return (boolean_t) ((addr1 / 256UL) == (addr2 / 256UL));
  }
  return (boolean_t) ((addr1 / 128UL) == (addr2 / 128UL));
}

/*
 * sign extend a value. If the value is indirect, irp:ind are expected
 * to point to the highest byte in the value.
 */
static void pic_value_sign_extend(pfile_t *pf, value_t val, 
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

static void pic_assign_const(pfile_t *pf, value_t dst,
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

#define PIC_WORK_IN_TEMP_FLAG_NONE          0x0000
#define PIC_WORK_IN_TEMP_FLAG_VAL1          0x0001 /* check val1 */
#define PIC_WORK_IN_TEMP_FLAG_VAL2          0x0002 /* check val2 */
#define PIC_WORK_IN_TEMP_FLAG_DST           0x0004 /* check dst  */
/* if DST is volatile assume we cannot work in it */
#define PIC_WORK_IN_TEMP_FLAG_DST_VOLATILE  0x0008
/* if {DST, VALx} are indirect *and* equal, move VALx to a temp */
#define PIC_WORK_IN_TEMP_FLAG_DST_VAL_EQUAL 0x0010
/* check both values (most commmon) */
#define PIC_WORK_IN_TEMP_FLAG_VALS \
  PIC_WORK_IN_TEMP_FLAG_VAL1       \
  | PIC_WORK_IN_TEMP_FLAG_VAL2
/* check all values */
#define PIC_WORK_IN_TEMP_FLAG_VALS_DST \
  PIC_WORK_IN_TEMP_FLAG_VALS      \
  | PIC_WORK_IN_TEMP_FLAG_DST

/* determine if the work must be done in a temporary
 * returns TRUE if work was done in a temporary, FALSE if no
 * nb: in the 16 bit cores, increment and decrement muck with
 *     STATUS<C>. To simplify things, I'll simply disallow
 *     indirect operations for the 16 bit cores
 */
static boolean_t pic_work_in_temp(pfile_t *pf, operator_t op,
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

static void pic_assign_from_bit(pfile_t *pf, value_t dst, value_t src);
/* on exit, Z is set if src is 0, clear otherwise */
static void pic_value_is_zero(pfile_t *pf, value_t src)
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
static void pic_integer_to_float(pfile_t *pf, value_t src)
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

static void pic_float_to_integer(pfile_t *pf, value_t src)
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

/* create an integer variable that overlays a bit variable */
static value_t pic_bit_overlay_create(value_t val)
{
  variable_t     var;
  variable_def_t def;
  unsigned       bit_ofs;
  unsigned       sz;
  value_t        dst;

  bit_ofs = value_bit_offset_get(val);
  sz      = value_sz_get(val);

  /* create a byte array covering dst */
  def   = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, (bit_ofs + sz + 7) / 8);
  var   = variable_alloc(TAG_NONE, def);
  variable_master_set(var, variable_master_get(value_variable_get(val)));

  dst   = value_alloc(var);
  variable_release(var);
  return dst;
}

static boolean_t pic_value_is_dynamic_bit_array_element(value_t val)
{
  value_t baseofs;
  variable_t var;

  baseofs = value_baseofs_get(val);
  var = value_variable_get(val);
  return value_is_bit(val)
    && value_flag_test(val, VALUE_FLAG_ARRAY)
    && ((baseofs && !value_is_const(baseofs))
      || (var && (pic_variable_chunk_ct_get(var) > 1)));
}

static void pic_dynamic_bit_ref_release(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  value_release(ref->byte);
  if (ref->mask) {
    pic_var_temp_release(pf, ref->mask);
  }
  if (ref->bit_index) {
    pic_var_temp_release(pf, ref->bit_index);
  }
  if (ref->byte_index) {
    pic_var_temp_release(pf, ref->byte_index);
  }
  ref->byte       = VALUE_NONE;
  ref->mask       = VALUE_NONE;
  ref->byte_index = VALUE_NONE;
  ref->bit_index  = VALUE_NONE;
}

static boolean_t pic_dynamic_bit_ref_prepare(pfile_t *pf, value_t val,
    pic_dynamic_bit_ref_t *ref)
{
  value_t       baseofs;
  value_t       cval;
  boolean_t     baseofs_release;
  variable_t    var;
  variable_t    master;
  variable_def_t def;
  variable_sz_t idx_sz;
  unsigned      bit_base;

  ref->byte       = VALUE_NONE;
  ref->mask       = VALUE_NONE;
  ref->byte_index = VALUE_NONE;
  ref->bit_index  = VALUE_NONE;

  baseofs_release = BOOLEAN_FALSE;
  baseofs = value_baseofs_get(val);
  var     = value_variable_get(val);
  if (!var) {
    return BOOLEAN_FALSE;
  }
  if (!baseofs) {
    baseofs = pfile_constant_get(pf, 0, VARIABLE_DEF_NONE);
    baseofs_release = BOOLEAN_TRUE;
  }
  if (!baseofs) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  bit_base = value_bit_offset_get(val);
  /*
   * Open `bit in bits[]` formals read buffer-relative bits: the normalize
   * buffer re-bases the packed actual at bit 0, but the formal binding
   * leaks the actual's intra-byte offset (classic reserved bitbucket bit 0)
   * onto the value — drop it so bits[i] resolves at buffer bit i on every
   * core (same contract as the static-offset arm in pic_assign_from_bit).
   * fix_pic18_open_bit_formal_indirect.md
   */
  if (bit_base && var && variable_is_pointer(var)
      && value_is_indirect(val) && baseofs
      && !value_is_const(baseofs)) {
    bit_base = 0;
  }
  idx_sz = value_byte_sz_get(baseofs);
  if (idx_sz < 1) {
    idx_sz = 1;
  }
  if ((idx_sz < 2) && (bit_base + variable_sz_get(var) > 255)) {
    idx_sz = 2;
  }

  ref->byte_index = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, idx_sz);
  ref->bit_index  = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  ref->mask       = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  if (!ref->byte_index || !ref->bit_index || !ref->mask) {
    if (baseofs_release) {
      value_release(baseofs);
    }
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }

  pic_op(pf, OPERATOR_ASSIGN, ref->byte_index, baseofs, VALUE_NONE);
  if (baseofs_release) {
    value_release(baseofs);
  }
  if (bit_base) {
    cval = pfile_constant_get(pf, bit_base, VARIABLE_DEF_NONE);
    pic_op(pf, OPERATOR_ADD, ref->byte_index, ref->byte_index, cval);
    value_release(cval);
  }

  cval = pfile_constant_get(pf, 7, VARIABLE_DEF_NONE);
  pic_op(pf, OPERATOR_ANDB, ref->bit_index, ref->byte_index, cval);
  value_release(cval);

  cval = pfile_constant_get(pf, 3, VARIABLE_DEF_NONE);
  pic_op(pf, OPERATOR_SHIFT_RIGHT, ref->byte_index, ref->byte_index, cval);
  value_release(cval);

  cval = pfile_constant_get(pf, 1, VARIABLE_DEF_NONE);
  pic_op(pf, OPERATOR_SHIFT_LEFT, ref->mask, cval, ref->bit_index);
  value_release(cval);

  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, 1);
  if (!def) {
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  master = variable_master_get(var);
  if (!master) {
    master = var;
  }
  var = variable_alloc(TAG_NONE, def);
  if (!var) {
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  variable_master_set(var, master);
  ref->byte = value_alloc(var);
  variable_release(var);
  if (!ref->byte) {
    pic_dynamic_bit_ref_release(pf, ref);
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  value_flag_set(ref->byte, VALUE_FLAG_ARRAY);
  value_baseofs_set(ref->byte, ref->byte_index);
  return BOOLEAN_TRUE;
}

static void pic_dynamic_bit_ref_linear_addr_setup(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref, variable_base_t base)
{
  variable_sz_t   idx_sz;

  idx_sz = value_byte_sz_get(ref->byte_index);
  if (idx_sz < 1) {
    idx_sz = 1;
  }
  pic_stvar_fsr_mark(pf);
  if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, ref->byte_index, 0,
        PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_ADDWFc, ref->byte_index, 1,
          PIC_OPDST_W);
    } else {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
    }
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
  } else {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, ref->byte_index, 0,
        PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, ref->byte_index, 1,
          PIC_OPDST_W);
    }
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref->bit_index, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
    pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, ref->bit_index, 0, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
  }
}

static void pic_dynamic_bit_ref_index_ge_chunk(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref, variable_sz_t chunk_sz, label_t lbl_ge)
{
  variable_sz_t idx_sz;

  idx_sz = value_byte_sz_get(ref->byte_index);
  if (idx_sz < 1) {
    idx_sz = 1;
  }
  if (chunk_sz >= 256) {
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->byte_index, 1,
          PIC_OPDST_W);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ge);
    }
  } else {
    if (idx_sz > 1) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->byte_index, 1,
          PIC_OPDST_W);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ge);
    }
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, chunk_sz);
    pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, ref->byte_index, 0,
        PIC_OPDST_W);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ge);
  }
}

static void pic_dynamic_bit_ref_addr_setup(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  variable_t      var;
  variable_ct_t  chunk_ct;

  var = value_variable_get(ref->byte);
  chunk_ct = var ? pic_variable_chunk_ct_get(var) : 0;
  if (chunk_ct > 1) {
    label_t done;
    variable_ct_t ii;

    done = pfile_label_alloc(pf, 0);
    for (ii = 0; ii < chunk_ct; ii++) {
      label_t         next;
      variable_base_t chunk_base;
      variable_sz_t   chunk_sz;

      chunk_base = pic_variable_chunk_base_get(var, ii);
      chunk_sz   = pic_variable_chunk_sz_get(var, ii);
      next = LABEL_NONE;
      if (ii + 1 < chunk_ct) {
        next = pfile_label_alloc(pf, 0);
        pic_dynamic_bit_ref_index_ge_chunk(pf, ref, chunk_sz, next);
      }
      pic_dynamic_bit_ref_linear_addr_setup(pf, ref, chunk_base);
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);
      if (next) {
        value_t cval;

        pic_instr_append_label(pf, next);
        cval = pfile_constant_get(pf, chunk_sz, VARIABLE_DEF_NONE);
        if (cval) {
          pic_op(pf, OPERATOR_SUB, ref->byte_index, ref->byte_index, cval);
          value_release(cval);
        } else {
          pfile_log_syserr(pf, ENOMEM);
        }
        label_release(next);
      }
    }
    pic_instr_append_label(pf, done);
    label_release(done);
  } else {
    pic_dynamic_bit_ref_linear_addr_setup(pf, ref, value_base_get(ref->byte));
  }
}

static void pic_dynamic_bit_ref_emit_set(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  pic_dynamic_bit_ref_addr_setup(pf, ref);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->byte, 0, PIC_OPDST_W);
  pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, ref->mask, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref->byte, 0);
}

static void pic_dynamic_bit_ref_emit_clear(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref)
{
  pic_dynamic_bit_ref_addr_setup(pf, ref);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref->mask, 0, PIC_OPDST_W);
  pic_instr_append_w_kn(pf, PIC_OPCODE_XORLW, 0xff);
  pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, ref->byte, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref->byte, 0);
}

/* Width-aware load/store for runtime-indexed `bit*N` elements (N in
 * 2..8). The 1-bit dynamic machinery above handles `var bit` arrays;
 * until 2026-10-27 multi-bit fields also routed there and every access
 * collapsed to a single set/clear of bit (idx & 7) of byte idx/8 with
 * the index UNSCALED -- a[k] wrote one bit at the wrong position and
 * reads returned 0/1 (the bit*N runtime defect). These emitters treat
 * the element as an N-bit field at runtime (byte, bit): 16-bit
 * rotate-loop shifts build the positioned mask/value pair, a runtime
 * straddle test (bit_index + N > 8) covers the second byte, and the
 * second address is a re-run of the address setup after a carry-aware
 * increment of the byte-index temp. */
static boolean_t pic_dynamic_field_width_get(value_t val,
    variable_sz_t *width)
{
  variable_sz_t w;

  w = value_sz_get(val);
  if ((w > 1) && (w <= 8)) {
    if (width) {
      *width = w;
    }
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

/* leaves the N-bit field value in W; Z reflects it (callers of the
 * 1-bit loader contract) */
static boolean_t pic_dynamic_field_load_to_w(pfile_t *pf, value_t src)
{
  pic_dynamic_bit_ref_t ref;
  variable_sz_t        w;
  value_t              t0;
  value_t              t1;
  value_t              cnt;
  label_t              lbl_nohi;
  label_t              lbl_loop;
  label_t              lbl_done;

  if (!pic_dynamic_field_width_get(src, &w)) {
    return BOOLEAN_FALSE;
  }
  if (!pic_dynamic_bit_ref_prepare(pf, src, &ref)) {
    return BOOLEAN_FALSE;
  }
  t0 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  t1 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  cnt = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  if (!t0 || !t1 || !cnt) {
    pfile_log_syserr(pf, ENOMEM);
  } else {
    lbl_nohi = pfile_label_alloc(pf, 0);
    lbl_loop = pfile_label_alloc(pf, 0);
    lbl_done = pfile_label_alloc(pf, 0);

    /* t0 = *byte; t1 = straddle ? *(byte+1) : 0 */
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t0, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t1, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW, 8 - w);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_nohi);
    pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 0,
        PIC_OPDST_F);
    if (value_byte_sz_get(ref.byte_index) > 1) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 1,
          PIC_OPDST_F);
    }
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t1, 0);
    pic_instr_append_label(pf, lbl_nohi);

    /* (t1:t0) >>= bit_index, then W = t0 & ((1<<N)-1). The rrf pair
     * order shifts t1's LSB into t0's MSB; bits discarded from t0 can
     * never re-enter t0's low N bits within 7 shifts, so no carry
     * hygiene is needed between iterations. */
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, cnt, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
    pic_instr_append_label(pf, lbl_loop);
    pic_instr_append_f_d(pf, PIC_OPCODE_RRF, t1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RRF, t0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, cnt, 0, PIC_OPDST_F);
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_loop);
    pic_instr_append_label(pf, lbl_done);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t0, 0, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, (1 << w) - 1);

    label_release(lbl_done);
    label_release(lbl_loop);
    label_release(lbl_nohi);
  }
  if (cnt) {
    pic_var_temp_release(pf, cnt);
  }
  if (t1) {
    pic_var_temp_release(pf, t1);
  }
  if (t0) {
    pic_var_temp_release(pf, t0);
  }
  pic_dynamic_bit_ref_release(pf, &ref);
  return (t0 && t1 && cnt) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* N-bit field store at a runtime (byte, bit) */
static boolean_t pic_assign_to_dynamic_field(pfile_t *pf, value_t dst,
    value_t src)
{
  pic_dynamic_bit_ref_t ref;
  variable_sz_t        w;
  value_t              sval;
  value_t              m0;
  value_t              m1;
  value_t              v0;
  value_t              v1;
  value_t              cnt;
  label_t              lbl_nohi;
  label_t              lbl_loop;
  label_t              lbl_done;
  boolean_t            ok;

  if (!pic_dynamic_field_width_get(dst, &w)) {
    return BOOLEAN_FALSE;
  }
  if (!pic_dynamic_bit_ref_prepare(pf, dst, &ref)) {
    return BOOLEAN_FALSE;
  }
  sval = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  m0 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  m1 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  v0 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  v1 = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  cnt = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  ok = (sval && m0 && m1 && v0 && v1 && cnt) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  if (!ok) {
    pfile_log_syserr(pf, ENOMEM);
  } else {
    lbl_nohi = pfile_label_alloc(pf, 0);
    lbl_loop = pfile_label_alloc(pf, 0);
    lbl_done = pfile_label_alloc(pf, 0);

    /* sval = src & ((1<<N)-1) */
    if (value_is_const(src)) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (unsigned) (value_const_get(src) & ((1 << w) - 1)));
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 0);
    } else {
      value_t maskv;

      pic_op(pf, OPERATOR_ASSIGN, sval, src, VALUE_NONE);
      maskv = pfile_constant_get(pf, (1 << w) - 1, VARIABLE_DEF_NONE);
      if (maskv) {
        pic_op(pf, OPERATOR_ANDB, sval, sval, maskv);
        value_release(maskv);
      }
    }

    /* (m1:m0) = ((1<<N)-1) << bit_index ; (v1:v0) = sval << bit_index.
     * The interleaved rlf chain is carry-safe: both high bytes stay
     * below bit 7 for N <= 8 and bit_index < 8, so C is 0 between the
     * pairs and each pair zero-fills. */
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (1 << w) - 1);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, m0, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, m1, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, sval, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, v0, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, v1, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, cnt, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
    pic_instr_append_label(pf, lbl_loop);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, m0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, m1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, v0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_RLF, v1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, cnt, 0, PIC_OPDST_F);
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_loop);
    pic_instr_append_label(pf, lbl_done);

    /* low byte: *byte = (*byte & ~m0) | v0 */
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_COMF, m0, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, m0, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, v0, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref.byte, 0);

    /* high byte only when the field straddles (bit_index + N > 8):
     * SUBLW (8-N) leaves C=1 when bit_index <= 8-N (no straddle) */
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.bit_index, 0,
        PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW, 8 - w);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_nohi);
    pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 0,
        PIC_OPDST_F);
    if (value_byte_sz_get(ref.byte_index) > 1) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, ref.byte_index, 1,
          PIC_OPDST_F);
    }
    pic_dynamic_bit_ref_addr_setup(pf, &ref);
    pic_instr_append_f_d(pf, PIC_OPCODE_COMF, m1, 0, PIC_OPDST_F);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, m1, 0, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, v1, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ref.byte, 0);
    pic_instr_append_label(pf, lbl_nohi);

    label_release(lbl_nohi);
    label_release(lbl_loop);
    label_release(lbl_done);
  }
  if (cnt) {
    pic_var_temp_release(pf, cnt);
  }
  if (v1) {
    pic_var_temp_release(pf, v1);
  }
  if (v0) {
    pic_var_temp_release(pf, v0);
  }
  if (m1) {
    pic_var_temp_release(pf, m1);
  }
  if (m0) {
    pic_var_temp_release(pf, m0);
  }
  if (sval) {
    pic_var_temp_release(pf, sval);
  }
  pic_dynamic_bit_ref_release(pf, &ref);
  return ok;
}

static boolean_t pic_dynamic_bit_load_to_w(pfile_t *pf, value_t src)
{
  pic_dynamic_bit_ref_t ref;

  if ((value_sz_get(src) > 1)) {
    return pic_dynamic_field_load_to_w(pf, src);
  }
  if (!pic_dynamic_bit_ref_prepare(pf, src, &ref)) {
    return BOOLEAN_FALSE;
  }
  pic_dynamic_bit_ref_addr_setup(pf, &ref);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ref.byte, 0, PIC_OPDST_W);
  pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, ref.mask, 0, PIC_OPDST_W);
  pic_dynamic_bit_ref_release(pf, &ref);
  return BOOLEAN_TRUE;
}

static boolean_t pic_assign_to_dynamic_bit(pfile_t *pf, value_t dst,
    value_t src)
{
  pic_dynamic_bit_ref_t ref;
  boolean_t             rc;

  if (value_sz_get(dst) > 8) {
    pfile_log(pf, PFILE_LOG_ERR,
        "runtime-indexed bit*%u array elements are not supported yet "
        "(width above 8 bits); use a constant subscript or a byte-array "
        "representation", (unsigned) value_sz_get(dst));
    return BOOLEAN_TRUE;
  }
  if (value_sz_get(dst) > 1) {
    return pic_assign_to_dynamic_field(pf, dst, src);
  }
  rc = pic_dynamic_bit_ref_prepare(pf, dst, &ref);
  if (rc) {
    if (value_is_const(src)) {
      if (value_const_get(src)) {
        pic_dynamic_bit_ref_emit_set(pf, &ref);
      } else {
        pic_dynamic_bit_ref_emit_clear(pf, &ref);
      }
    } else {
      value_t src_tmp;
      label_t lbl_clear;
      label_t lbl_done;

      src_tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
      if (!src_tmp) {
        pfile_log_syserr(pf, ENOMEM);
      } else {
        pic_op(pf, OPERATOR_ASSIGN, src_tmp, src, VALUE_NONE);
        lbl_clear = pfile_label_alloc(pf, 0);
        lbl_done  = pfile_label_alloc(pf, 0);
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src_tmp, 0, PIC_OPDST_F);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_clear);
        pic_dynamic_bit_ref_emit_set(pf, &ref);
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_done);
        pic_instr_append_label(pf, lbl_clear);
        pic_dynamic_bit_ref_emit_clear(pf, &ref);
        pic_instr_append_label(pf, lbl_done);
        label_release(lbl_done);
        label_release(lbl_clear);
        pic_var_temp_release(pf, src_tmp);
      }
    }
    pic_dynamic_bit_ref_release(pf, &ref);
  }
  return rc;
}

/* dst is a bit, src is not lookup */
static void pic_assign_to_bit(pfile_t *pf, value_t dst, value_t src)
{
  unsigned dst_bit_ofs;

  if (pic_value_is_dynamic_bit_array_element(dst)) {
    (void) pic_assign_to_dynamic_bit(pf, dst, src);
    return;
  }
  dst_bit_ofs  = value_bit_offset_get(dst);
  if (value_sz_get(dst) == 1) {
    if (value_is_const(src)) {
      variable_const_t cn;

      cn = value_const_get(src);
      pic_instr_append_f_bn(pf,
          ((value_is_boolean(dst) && cn)
          || (cn & 1)) ? PIC_OPCODE_BSF : PIC_OPCODE_BCF,
          dst, 0, 0);
    } else {
      if (value_is_bit(src) && (value_sz_get(src) == 1)) {
        /* single bit --> single bit */
        if (pic_value_is_dynamic_bit_array_element(src)) {
          pic_dynamic_bit_load_to_w(pf, src);
          if (value_is_volatile(dst) || !pic_val_bank_same(pf, dst, src)) {
            label_t lbl_set;
            label_t lbl_done;

            lbl_set  = pfile_label_alloc(pf, 0);
            lbl_done = pfile_label_alloc(pf, 0);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
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
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
            pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
          }
        } else
        if (value_is_volatile(dst) || !pic_val_bank_same(pf, dst, src)) {
          label_t lbl_set;
          label_t lbl_done;

          lbl_set  = pfile_label_alloc(pf, 0);
          lbl_done = pfile_label_alloc(pf, 0);
          pic_instr_append_f(pf, PIC_OPCODE_BTFSC, src, 0);
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
          pic_instr_append_f(pf, PIC_OPCODE_BTFSC, src, 0);
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        }
      } else {
        /* determine if src is 0, set src to _status:_z, and invert test */
        if (pic_value_is_w(src)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 
            (value_is_boolean(dst) ? 0xff : 0x01));
        } else {
          if (value_is_boolean(dst)) {
            pic_value_is_zero(pf, src);
          } else {
            if (value_is_indirect(src)) {
              variable_sz_t ipos;

              pic_indirect_setup3(pf, src, VALUE_NONE, VALUE_NONE, 0, &ipos);
            }
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, 0, PIC_OPDST_W);
            pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 1);
          }
        }
        /* set or clear dst as appropriate */
        if (value_is_volatile(dst) || !pic_val_bank_same(pf, dst, src)) {
          /* cannot change dst twice or cannot skip a single inst. :( */
          label_t done;
          label_t skip;

          skip = pfile_label_alloc(pf, 0);
          done = pfile_label_alloc(pf, 0);

          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, skip);
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
          pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);
          pic_instr_append_label(pf, skip);
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
          pic_instr_append_label(pf, done);
          label_release(done);
          label_release(skip);
        } else {
          /* simple single-bit transform */
          pic_instr_append_f(pf, PIC_OPCODE_BCF, dst, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
          pic_instr_append_f(pf, PIC_OPCODE_BSF, dst, 0);
        }
      }
    }
  } else if (!value_is_const(src) && value_is_bit(src)) {
    value_t       tmp;
    variable_sz_t tmp_sz;

    tmp_sz = (value_sz_get(src) + 7) / 8;
    if (tmp_sz < 1) {
      tmp_sz = 1;
    }
    tmp = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, tmp_sz);
    if (!tmp) {
      pfile_log_syserr(pf, ENOMEM);
    } else {
      pic_assign_from_bit(pf, tmp, src);
      pic_assign_to_bit(pf, dst, tmp);
      pic_var_temp_release(pf, tmp);
    }
  } else if (!pic_work_in_temp(pf, OPERATOR_ASSIGN, dst, src, VALUE_NONE,
    PIC_WORK_IN_TEMP_FLAG_VALS)) {
    if (!value_is_const(src) 
      && !value_is_volatile(dst)
      && (value_sz_get(dst) <= 4)
      && ((dst_bit_ofs & 7) >= 5)) {
      variable_sz_t dst_sz;
      variable_sz_t dst_ofs;
      variable_sz_t ii;
      value_t       tmp;

      tmp = src;
      if (pic_value_is_w(tmp) || !pic_val_bank_same(pf, src, dst)) {
        tmp = pic_var_accum_get(pf);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, tmp, 0);
      }

      dst_sz  = value_sz_get(dst);
      dst_ofs = value_bit_offset_get(dst);
      dst = pic_bit_overlay_create(dst);
      for (ii = 0; ii < dst_sz; ii++) {
        pic_instr_append_f_bn(pf, PIC_OPCODE_BCF, dst,
            (dst_ofs + ii) / 8,
            (dst_ofs + ii) & 7);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, tmp, ii / 8, ii & 7);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst,
            (dst_ofs + ii) / 8,
            (dst_ofs + ii) & 7);
      }
      if (tmp != src) {
        pic_var_accum_release(pf, tmp);
      }
      value_release(dst);
    } else {
      /* optimize this a bit -- don't use the standard and/or
       * routines. instead do it directly (a combined and + or)
       */
      unsigned dst_byte_ofs;
      unsigned dst_byte_sz;  /* the number of bytes of interest */
      unsigned dst_bit_sz;
      unsigned mask;

      dst_byte_ofs = dst_bit_ofs / 8;
      dst_bit_ofs  = dst_bit_ofs & 7;
      dst_bit_sz   = value_sz_get(dst);
      dst_byte_sz  = (dst_bit_sz + dst_bit_ofs + 7)/ 8;

      /* we can ignore the first dst_byte_ofs bytes of dst as
       * they're not going to change */
      dst  = pic_bit_overlay_create(dst);
      mask = ~(((1 << dst_bit_sz) - 1) << dst_bit_ofs);

      /* tmp = src with bits masked off */
      if (value_is_const(src)) {
        unsigned sval;

        sval = (value_const_get(src) & ((1 << dst_bit_sz) - 1)) << dst_bit_ofs;

        while (dst_byte_sz) {
          if ((mask & 0xff) == 0x00) {
            /* there's nothing to mask off, simply copy this value in */
            if (sval & 0xff) {
              /* there is a value, copy it in */
              pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sval & 0xff);
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs);
            } else {
              /* no value, simply clear this */
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, dst_byte_ofs);
            }
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (unsigned char) (mask & 0xff));
            if (sval & 0xff) {
              /* sval is not 0, we'll need to AND and OR */
              pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, dst_byte_ofs, 
                PIC_OPDST_W);
              pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW,
                (unsigned char) (sval & 0xff));
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs);
            } else {
              /* sval is 0, we need only and */
              pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, dst_byte_ofs, 
                PIC_OPDST_F);
            }
          }
          dst_byte_ofs++;
          dst_byte_sz--;
          mask >>= 8;
          sval >>= 8;
        }
      } else {
        value_t tmp;
        value_t tmp2;
        size_t  ii;

        /* tmp = ( src << dst_bit_ofs ) & mask */
        tmp  = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, dst_byte_sz);
        if (value_sz_get(src) < value_sz_get(tmp)) {
          pic_op(pf, OPERATOR_ASSIGN, tmp, src, VALUE_NONE);
          src = tmp;
        }
        if (dst_bit_ofs) {
          tmp2 = pfile_constant_get(pf, dst_bit_ofs, VARIABLE_DEF_NONE);
          pic_shift_left(pf, OPERATOR_SHIFT_LEFT, tmp, src, tmp2,
            PIC_SHIFT_FLAG_NONE);
          value_release(tmp2);
          src = tmp;
        }
        /* if bit ofs is 4 and bit sz is 4 no further masking is necessary */
        /*if ((dst_bit_ofs != 4) || (dst_bit_sz != 4)) */{
          tmp2 = pfile_constant_get(pf, ~mask, VARIABLE_DEF_NONE);
          pic_op(pf, OPERATOR_ANDB, tmp, src, tmp2);
          value_release(tmp2);
        }
        for (ii = 0; ii < dst_byte_sz; ii++) {
          if ((mask & 0xff) == 0x00) {
            /* no mask, this is a straight copy */
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, ii, PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs + ii);
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, mask & 0xff);
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, dst_byte_ofs + ii,
              PIC_OPDST_W);
            pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, tmp, ii, PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, dst_byte_ofs + ii);
          }
          mask >>= 8;
        }
        pic_var_temp_release(pf, tmp);
      }
      value_release(dst);
    }
  }
}

/* dst is not bit, src is non-const bit */
static void pic_assign_from_bit(pfile_t *pf, value_t dst, value_t src)
{
  if (pic_value_is_dynamic_bit_array_element(src)) {
    if ((value_sz_get(src) > 1)
        && pic_dynamic_field_load_to_w(pf, src)) {
      /* N-bit field: the loader returns the FIELD VALUE in W (Z
       * reflects it) -- no 0/1 boolean normalization */
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
      }
    } else if (pic_dynamic_bit_load_to_w(pf, src)) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          value_is_signed(src) ? 0xff : 1);
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
      }
    }
  } else if ((value_sz_get(src) == 1)
      && (value_is_indirect(src) || (value_bit_offset_get(src) != 0))) {
    variable_sz_t ipos;

    if (value_is_indirect(src)) {
      /*
       * A bit read through a pointer (open `bit in bits[]` formal with a
       * packed actual) carries the bit subscript in baseofs. It is a BIT
       * index into the packed pointed-to bytes: the FSR byte offset is
       * baseofs / 8 and the tested bit is baseofs & 7 (plus any value bit
       * offset). The old code passed the subscript to setup as a plain
       * BYTE offset and emitted a byte-oriented BTFSC whose bit field then
       * leaked the INDF file address at encode time (PIC18 INDF0 0xFEF ->
       * bit 7); classic cores passed only via the _ind-at-address-0 bit-0
       * coincidence. fix_pic18_open_bit_formal_indirect.md
       */
      unsigned long bit_ix;
      value_t dbo;
      value_t src_use;

      /*
       * Contract: an open `bit in bits[]` formal reads buffer-relative bits
       * (the normalize buffer re-bases the actual's packed bits at 0). The
       * subscript rides in baseofs; any value bitofs here is the actual's
       * intra-byte offset leaking through the formal binding (classic
       * reserved bitbucket bit 0) and must be ignored.
       */
      dbo = value_baseofs_get(src);
      if (dbo && !value_is_const(dbo)) {
        /*
         * Runtime subscript: compute byte = i/8 and bit = i&7 at runtime
         * via the dynamic loader (same shift/mask emission the first
         * branch uses for array elements); the leaked bitofs is dropped
         * inside pic_dynamic_bit_ref_prepare for pointer formals.
         */
        /*
         * The array dynamic loader cannot be reused here: its master chain
         * resolves the pointer SLOT variable as a direct array base (wrong
         * address in fuller programs). Emit directly: byte = i >> 3 and
         * mask = 1 << (i & 7) as runtime temps, point the FSR at
         * slot + byte via a cloned baseofs, read INDF and AND the mask.
         */
        {
          value_t t_idx;
          value_t t_byt;
          value_t t_bit;
          value_t t_msk;
          value_t c3;
          value_t c7;
          value_t c1;
          value_t ind;
          boolean_t ok;

          t_idx = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          t_byt = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          t_bit = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          t_msk = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
          c3 = pfile_constant_get(pf, 3, VARIABLE_DEF_NONE);
          c7 = pfile_constant_get(pf, 7, VARIABLE_DEF_NONE);
          c1 = pfile_constant_get(pf, 1, VARIABLE_DEF_NONE);
          ok = t_idx && t_byt && t_bit && t_msk && c3 && c7 && c1;
          if (ok) {
            src_use = value_clone(src);
            value_baseofs_set(src_use, t_byt);

            pic_op(pf, OPERATOR_ASSIGN, t_idx, dbo, VALUE_NONE);
            pic_op(pf, OPERATOR_SHIFT_RIGHT, t_byt, t_idx, c3);
            pic_op(pf, OPERATOR_ANDB, t_bit, t_idx, c7);
            pic_op(pf, OPERATOR_ASSIGN, t_msk, c1, VALUE_NONE);
            pic_op(pf, OPERATOR_SHIFT_LEFT, t_msk, t_msk, t_bit);

            pic_indirect_setup3(pf, src_use, VALUE_NONE, dst, 0, &ipos);
            ind = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, t_msk, 0, PIC_OPDST_W);
            value_release(ind);
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
                value_is_signed(src) ? 0xff : 1);
            if (!pic_value_is_w(dst)) {
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
              pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
            }
            value_release(src_use);
          }
          value_release(c1);
          value_release(c7);
          value_release(c3);
          pic_var_temp_release(pf, t_msk);
          pic_var_temp_release(pf, t_bit);
          pic_var_temp_release(pf, t_byt);
          pic_var_temp_release(pf, t_idx);
          if (ok) {
            return;
          }
        }
        /* OOM: fall through to the original emission */
      }
      bit_ix = 0;
      if (getenv("JALV2_DBG_OBF")) {
        fprintf(stderr, "OBF pass=%u bitofs=%d baseofs=%d\n",
          (int) pic_code_gen_pass_get(pf),
          (int) value_bit_offset_get(src),
          (int) ((dbo && value_is_const(dbo))
            ? (long) (value_const_get(dbo) & 0xffff) : -1L));
      }
      if (dbo && value_is_const(dbo)) {
        bit_ix += (unsigned long) (value_const_get(dbo) & 0xffff);
        src_use = value_clone(src);
        value_baseofs_set(src_use, VALUE_NONE);
      } else {
        src_use = src;
      }
      /* the result is either all 0 or all 0xff */
      pic_indirect_setup3(pf, src_use, VALUE_NONE, dst,
        (variable_sz_t) (bit_ix / 8), &ipos);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src_use,
        (size_t) (bit_ix / 8), (variable_const_t) (bit_ix & 7));
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          value_is_signed(src) ? 0xff : 1);
      if (src_use != src) {
        value_release(src_use);
      }
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
      }
      return;
    }
    /* non-indirect: original emission (unchanged) */
    /* the result is either all 0 or all 0xff */
    pic_indirect_setup3(pf, src, VALUE_NONE, dst, 0, &ipos);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f(pf, PIC_OPCODE_BTFSC, src, 0);
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 
        value_is_signed(src) ? 0xff : 1);
    if (!pic_value_is_w(dst)) {
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
    }
  } else if (!value_is_volatile(src)
      && (value_sz_get(src) <= 8)
      && ((value_bit_offset_get(src) & 7) != 0)) {
    variable_sz_t ipos;
    variable_sz_t ii;
    variable_sz_t src_sz;
    variable_sz_t src_bit_ofs;

    src_sz      = value_sz_get(src);
    src_bit_ofs = value_bit_offset_get(src);
    src = pic_bit_overlay_create(src);
    pic_indirect_setup3(pf, dst, VALUE_NONE, VALUE_NONE, 0, &ipos);
    if (value_is_volatile(dst)
        || pic_value_is_w(dst)
        || !pic_val_bank_same(pf, dst, src)) {
      /* pic_instr_append(pf, PIC_OPCODE_CLRW); */
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
      for (ii = 0; ii < src_sz; ii++) {
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src, 
            (src_bit_ofs + ii) / 8, 
            (src_bit_ofs + ii) & 7);
        pic_instr_append_w_kn(pf, PIC_OPCODE_IORLW, 1 << ii);
      }
      if (!pic_value_is_w(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      }
    } else {
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, 0);
      for (ii = 0; ii < src_sz; ii++) {
        pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, src, 
            (src_bit_ofs + ii) / 8, 
            (src_bit_ofs + ii) & 7);
        pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, dst, 0, ii);
      }
    }
    value_release(src);
    if (!pic_value_is_w(dst)) {
      pic_value_sign_extend(pf, dst, 0, value_is_signed(src));
    }
  } else {
    /* dst = (src >> src_bit_ofs) & mask */
    value_t       tdst;
    variable_sz_t src_bit_sz;
    variable_sz_t src_bit_ofs;
    variable_sz_t src_byte_sz;
    variable_sz_t src_byte_ofs;
    unsigned      mask;
    value_t       tmp;

    src_bit_ofs  = value_bit_offset_get(src);
    src_bit_sz   = value_sz_get(src);
    src_byte_ofs = src_bit_ofs / 8;
    src_bit_ofs  = src_bit_ofs & 7;
    src_byte_sz  = (src_bit_sz + 7) / 8;

    src = pic_bit_overlay_create(src);

    if (src_bit_ofs) {
      tdst = (pic_value_is_w(dst) || value_is_volatile(dst)) 
        ? pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE,
            pic_value_is_w(dst) ? 1 : pic_result_sz_get(src, VALUE_NONE, dst))
        : dst;
    } else {
      tdst = src;
    }
    /* tdst = src >> src_bit_ofs */
    if (src_bit_ofs) {
      tmp = pfile_constant_get(pf, src_bit_ofs, VARIABLE_DEF_NONE);
      pic_shift_right(pf, OPERATOR_SHIFT_RIGHT, tdst, src, tmp,
        PIC_SHIFT_FLAG_MASK_BITS);
      value_release(tmp);
    }

    mask = ((1 << src_bit_sz) - 1);

    if (pic_value_is_w(dst)) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tdst,
        (tdst == src) ? src_byte_ofs : 0, PIC_OPDST_W);
      if ((mask & 0xff) != 0xff) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, mask & 0xff);
      }
    } else {
      variable_sz_t ii;
      variable_sz_t sz;

      if (src_byte_sz < value_sz_get(dst)) {
        sz = src_byte_sz;
      } else {
        sz = value_sz_get(dst);
      }

      for (ii = 0; ii < sz; ii++) {
        if ((mask & 0xff) == 0xff) {
          if (dst != tdst) {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tdst, 
              ii + ((tdst == src) ? src_byte_ofs : 0), PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
          }
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, mask & 0xff);
          if (dst == tdst) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, dst, ii, PIC_OPDST_F);
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_ANDWF, tdst, 
              ii + ((tdst == src) ? src_byte_ofs : 0), PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
          } 
        }
      }
      while (ii < value_sz_get(dst)) {
        pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii);
        ii++;
      }
    }
    if ((src != tdst) && (dst != tdst)) {
      pic_var_temp_release(pf, tdst);
    }
    value_release(src);
  }
}

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

static void pic_assign_array_to_pointer(pfile_t *pf, value_t dst, value_t src);
static boolean_t pic_src_is_data_array(const value_t src);
static boolean_t pic_variable_is_rom_int_const_table(variable_t sv);

static void pic_assign_lookup_label_to_pointer(pfile_t *pf, value_t dst,
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

typedef enum {
  PIC_VALUE_TO_W_NONE, /* unused */
  PIC_VALUE_TO_W_MOV,
  PIC_VALUE_TO_W_SUB,
  PIC_VALUE_TO_W_ADD
} pic_value_to_w_t;

static void pic_value_to_w(pfile_t *pf, pic_value_to_w_t op,
    value_t val, value_t val_sign, variable_sz_t pos);

static void pic_assign_from_value(pfile_t *pf, value_t dst, value_t src);

/* In-place add of an offset expression into a pointer SLOT. dst here is a
 * pointer VALUE-VIEW (`*-V-`): routing the add through pic_op's generic
 * emitters computes into a temp and stores the result THROUGH the view --
 * store-through semantics (bound-trap + FSR + INDF), correct for `p = v`
 * but wrong for a slot update: it writes the pointee instead of the slot
 * and left `p is arr[k]` pointing at the array base with the element
 * address smashed over arr[0] (both 14-bit cores; the 16-bit core adds
 * in place and was fine). A plain slot view over the same variable stores
 * to the slot bytes on every core. */
static void pic_pointer_slot_add_emit(pfile_t *pf, value_t dst, value_t ofs)
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

static void pic_assign_array_to_pointer(pfile_t *pf, value_t dst, value_t src)
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

static void pic_assign_ptr_to_ptr(pfile_t *pf, value_t dst, value_t src)
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
static void pic_assign_to_ptr(pfile_t *pf, value_t dst, value_t src)
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
static void pic_assign_from_ptr(pfile_t *pf, value_t dst, value_t src)
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

static void pic_assign_to_float(pfile_t *pf, value_t dst, value_t src)
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

static void pic_assign_from_float(pfile_t *pf, value_t dst, value_t src)
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
static boolean_t pic_variable_is_array_table(variable_t sv)
{
  variable_def_t sdef;

  if (!sv || VARIABLE_NONE == sv) {
    return BOOLEAN_FALSE;
  }
  sdef = variable_def_get(sv);
  return sdef && VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(sdef);
}

/* `const byte buf[N]` may use VARIABLE_DEF_TYPE_INTEGER + CONST on the variable. */
static boolean_t pic_variable_is_rom_int_const_table(variable_t sv)
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
static boolean_t pic_dst_backs_pointer_storage(value_t dst)
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
static boolean_t pic_src_is_data_array(const value_t src)
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

static boolean_t pic_try_src_arraylike_for_pointer_bind(const value_t src)
{
  if (pic_src_is_data_array(src)) {
    return BOOLEAN_TRUE;
  }
  return pic_variable_is_rom_int_const_table(value_variable_get(src));
}

/* `p is buf` may reach here as OPERATOR_ASSIGN with dst already dereferenced
 * (*p in the cmd stream) while the backing variable is still a POINTER def.
 * pic_assign_array_to_pointer needs a non-indirect value typed as pointer. */
static boolean_t pic_try_bind_array_to_pointer_slot(pfile_t *pf, value_t dst,
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

/* note: dst or src can be W */
static void pic_op_assign(pfile_t *pf, operator_t op, value_t dst, 
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

/* operator on val1 & dst (assign, neg, etc.). if val1 is signed,
 * and its size is smaller than dst, and the result *is not*
 * signed, shrink dst to val1 to eliminate sign extension,
 * perform the operation, then zero out the upper bytes in dst
 */
static void pic_unary_fixup(pfile_t *pf, operator_t op,
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

static void pic_add_sub_float(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_add_sub(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_mul(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_div_mod(pfile_t *pf, operator_t op, value_t dst, 
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
#define PIC_ASSIGN_LOGICAL_TO_DST_NONE 0x0000
#define PIC_ASSIGN_LOGICAL_TO_DST_OP   0x0001 /* finish with pic_op(..) */
static void pic_assign_logical_to_dst(pfile_t *pf, value_t dst, uchar n,
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

static void pic_value_to_w(pfile_t *pf, pic_value_to_w_t op,
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
static void pic_assign_from_w(pfile_t *pf, value_t dst, boolean_t sign_extend)
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

/* much simpler than relational -- need only compare bytes;
 * nb : this is called for both OPERATOR_EQ & OPERATOR_NE
 * if dst is VALUE_NONE, the caller must check _Z and act
 * appropriately.
 */
static operator_t pic_relational_float(pfile_t *pf, operator_t op, 
  value_t dst, value_t val1, value_t val2);
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

static void pic_op_relational(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  pic_relational(pf, op, dst, val1, val2);
}

/* if dst is VALUE_NONE, the caller is responsible for checking _Z
 * _Z is set if val1 && val2 == 0, clear if not
 */
static void pic_op_logical_and(pfile_t *pf, operator_t op, value_t dst,
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
static void pic_op_logical_or(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_binary_and(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_binary_or(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_binary_xor(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_cmp(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_neg(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(src));

  if (value_is_float(src)) {
    pic_var_float_t flt;

    pic_var_float_get(pf, &flt);
    if (!value_is_same(dst, src)) {
      if (value_is_float(dst)) {
        pic_op(pf, OPERATOR_ASSIGN, dst, src, VALUE_NONE);
        src = dst;
      } else {
        pic_op(pf, OPERATOR_ASSIGN, flt.fval1, src, VALUE_NONE);
        src = flt.fval1;
      }
    }
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, 3, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, 0x80);
    pic_instr_append_f_bn(pf, PIC_OPCODE_BCF, src, 3, 7);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, src, 3, 7);
    if (!value_is_same(dst, src)) {
      pic_op(pf, OPERATOR_ASSIGN, dst, src, VALUE_NONE);
    }
    pic_var_float_release(pf, &flt);
  } else if (!pic_work_in_temp(pf, op, dst, src, val2,
        PIC_WORK_IN_TEMP_FLAG_VAL1
        | PIC_WORK_IN_TEMP_FLAG_DST)) {
    variable_sz_t sz;

    sz = pic_result_sz_get(src, val2, dst);
    if ((1 == sz) 
      && !pic_is_12bit(pf)  
      && (value_is_volatile(dst) || !value_is_same(dst, src))) {
      variable_sz_t ipos;

      pic_indirect_setup3(pf, dst, src, val2, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_COMF, src, 0, PIC_OPDST_W);
      pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      
      pic_value_sign_extend(pf, dst, 0, 
          pic_result_is_signed(pf, src, val2));
    } else if (value_is_volatile(dst)) {
        value_t tmp;

        tmp = pic_var_temp_get(pf, pic_result_flag_get(pf, src, val2),
            pic_result_sz_get(src, val2, dst));
        pic_op(pf, OPERATOR_CMPB, tmp, src, val2);
        pic_op(pf, OPERATOR_INCR, dst, tmp, VALUE_NONE);
        pic_var_temp_release(pf, tmp);
    } else if (value_sz_get(dst) > sz) {
      value_t        tmp;
      variable_def_t def;

      tmp = value_clone(dst);
      def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
          value_dflag_test(src, VARIABLE_DEF_FLAG_SIGNED)
            ? VARIABLE_DEF_FLAG_SIGNED
            : VARIABLE_DEF_FLAG_NONE,
          sz);
      value_def_set(tmp, def);
      pic_op(pf, OPERATOR_CMPB, tmp, src, val2);
      pic_op(pf, OPERATOR_INCR, tmp, tmp, VALUE_NONE);
      pic_value_sign_extend(pf, dst, sz - 1, 
          pic_result_is_signed(pf, src, val2));
      value_release(tmp);
    } else {
      pic_op(pf, OPERATOR_CMPB, dst, src, val2);
      pic_op(pf, OPERATOR_INCR, dst, dst, VALUE_NONE);
    }
  }
}

static void pic_op_incr(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(src));

  if (!pic_work_in_temp(pf, op, dst, src, val2,
    PIC_WORK_IN_TEMP_FLAG_VAL1
    | PIC_WORK_IN_TEMP_FLAG_DST)) {
    variable_sz_t sz;
    variable_sz_t ii;

    ii = 0;
    sz = pic_result_sz_get(src, val2, dst);
    if (1 == sz) {
      /* trivial case */
      pic_opdst_t   opdst;
      variable_sz_t ipos;

      opdst = (value_is_same(src, dst)) ? PIC_OPDST_F : PIC_OPDST_W;

      ii = 1; /* for sign extend */
      pic_indirect_setup3(pf, src, val2, dst, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, src, 0, opdst);
      if (PIC_OPDST_W == opdst) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      }
    } else if (value_is_same(src, dst) && !value_is_indirect(src)) {
      for (ii = 0; ii < sz; ii++) {
        if (ii) {
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        }
        pic_instr_append_f_d(pf, PIC_OPCODE_INCF, dst, ii, PIC_OPDST_F);
      }
    } else {
      variable_sz_t ipos;
      pic_opdst_t   opdst;
      value_t       src_sign;

      opdst = (value_is_same(dst, src)) ? PIC_OPDST_F : PIC_OPDST_W;
      pic_indirect_setup3(pf, dst, src, VALUE_NONE, 0, &ipos);
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
      src_sign = VALUE_NONE;
      for (ii = 0; ii < sz; ii++) {
        pic_indirect_bump3(pf, dst, src, val2, ii, &ipos);
        if (ii) {
          if (ii == value_sz_get(src)) {
            src_sign = pic_value_sign_get(pf, src, VALUE_NONE);
            src      = src_sign;
            if (VALUE_NONE == src) {
              opdst = PIC_OPDST_W;
            }
          }
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
        }
        if (VALUE_NONE == src) {
          if (ii + 1 < sz) {
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
          }
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, src, 
            (src == src_sign) ? 0 : ii, opdst);
        }
        if (PIC_OPDST_W == opdst) {
          pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii);
        }
      }
      if (VALUE_NONE != src_sign) {
        pic_var_accum_release(pf, src_sign);
      }
    }
    pic_value_sign_extend(pf, dst, ii - 1,
      pic_result_is_signed(pf, src, val2));
  }
}

static void pic_op_decr(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2)
{
  assert(!pic_value_is_w(dst));
  assert(!pic_value_is_w(src));

  if (!pic_work_in_temp(pf, op, dst, src, val2,
    PIC_WORK_IN_TEMP_FLAG_VAL1
    | PIC_WORK_IN_TEMP_FLAG_DST)) {
    variable_sz_t sz;
    variable_sz_t ii;

    sz = pic_result_sz_get(src, val2, dst);
    if (1 == sz) {
      /* trivial case */
      pic_opdst_t   opdst;
      variable_sz_t ipos;

      opdst = (value_is_same(src, dst)) ? PIC_OPDST_F : PIC_OPDST_W;

      ii = 1;
      pic_indirect_setup3(pf, src, VALUE_NONE, dst, 0, &ipos);
      pic_instr_append_f_d(pf, PIC_OPCODE_DECF, src, 0, opdst);
      if (PIC_OPDST_W == opdst) {
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
      }
    } else {
      /* the work must be done in dst, 
       * so first copy src(n..1) --> dst(n..1) */
      variable_sz_t ipos;

      if (!value_is_same(dst, src)) {
        pic_indirect_setup3(pf, dst, src, VALUE_NONE, sz - 1, &ipos);
        for (ii = sz; ii > 1; ii--) {
          if (ii > value_sz_get(src)) {
            if (value_is_signed(src)) {
              if (ii == sz) {
                pic_value_sign_get_in_w(pf, src);
              }
              pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii - 1);
            } else {
              pic_instr_append_f(pf, PIC_OPCODE_CLRF, dst, ii - 1);
            }
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, src, ii - 1, 
              PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, ii - 1);
          }
          pic_indirect_bump3(pf, dst, src, VALUE_NONE, ii - 2, &ipos);
        }
        if (value_is_indirect(dst) || (sz > 2)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
          pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, src, 0, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_DECF, src, 0, PIC_OPDST_W);
        }
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, dst, 0);
        if (value_is_indirect(dst) || (sz > 2)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
        }
      } else {
        pic_indirect_setup3(pf, dst, src, VALUE_NONE, 0, &ipos);
        if (value_is_indirect(dst) || (sz > 2)) {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 1);
          pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, dst, 0, PIC_OPDST_F);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_DECF, dst, 0, PIC_OPDST_F);
        }
      }
      if ((2 == sz) && !value_is_indirect(dst)) {
        pic_instr_append_f_d(pf, PIC_OPCODE_INCF, dst, 0, PIC_OPDST_W);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        pic_instr_append_f_d(pf, PIC_OPCODE_DECF, dst, 1, PIC_OPDST_F);
        ii = sz;
      } else {
        for (ii = 1; ii < sz; ii++) {
          pic_indirect_bump3(pf, dst, VALUE_NONE, VALUE_NONE, ii, &ipos);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
          pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, dst, ii, PIC_OPDST_F);
        }
      }
    }
    pic_value_sign_extend(pf, dst, ii - 1,
      pic_result_is_signed(pf, src, val2));
  }
}

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
typedef enum {
  PIC_ASSIGN_SHIFT_N_BYTES_ACTION_COPY,
  PIC_ASSIGN_SHIFT_N_BYTES_ACTION_SWAP
} pic_assign_shift_n_bytes_action_t;

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

static void pic_shift_left(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_shift_left(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_shift_right(pfile_t *pf, operator_t op, value_t dst,
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

static void pic_op_shift_right(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2)
{
  pic_shift_right(pf, op, dst, val1, val2, PIC_SHIFT_FLAG_MASK_BITS);
}

/* set dst to 0 if val is 0, 1 if val != 0 */
static void pic_op_logical_or_not(pfile_t *pf, operator_t op, value_t dst,
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


/************************************************************
 **
 ** pic_op_priv.h : PIC operator generation -- module-private interface
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **           (c) 2020-2025, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: created by the 2026-10-31 pic_op.c decomposition.
 **
 ** pic_op.c (8751 lines) is now one translation unit per operator
 ** family. Everything here is INTERNAL to that module -- no other
 ** module includes this header. A function stays `static` (with a
 ** TU-local prototype) when every caller lives in the same TU; the
 ** rest are declared here and defined exactly once. Public entry
 ** points stay in pic_op.h.
 **
 ************************************************************/
#ifndef pic_op_priv_h__
#define pic_op_priv_h__

#include "../libcore/pf_proc.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pf_op.h"
#include "pic_op.h"


#define PIC_SHIFT_FLAG_NONE      0x0000
#define PIC_SHIFT_FLAG_MASK_BITS 0x0001

typedef struct pic_dynamic_bit_ref_ {
  value_t byte;
  value_t mask;
  value_t byte_index;
  value_t bit_index;
} pic_dynamic_bit_ref_t;

typedef enum pic_last_value_ {
  PIC_LAST_VALUE_FIRST,
  PIC_LAST_VALUE_DIVISOR = PIC_LAST_VALUE_FIRST,
  PIC_LAST_VALUE_DIVIDEND,
  PIC_LAST_VALUE_MULTIPLIER,
  PIC_LAST_VALUE_MULTIPLICAND,
  PIC_LAST_VALUE_CT
} pic_last_value_t;

/* shared flag vocabulary: bit-field work routines take these */
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

typedef enum {
  PIC_VALUE_TO_W_NONE, /* unused */
  PIC_VALUE_TO_W_MOV,
  PIC_VALUE_TO_W_SUB,
  PIC_VALUE_TO_W_ADD
} pic_value_to_w_t;

#define PIC_ASSIGN_LOGICAL_TO_DST_NONE 0x0000
#define PIC_ASSIGN_LOGICAL_TO_DST_OP   0x0001 /* finish with pic_op(..) */

typedef enum {
  PIC_ASSIGN_SHIFT_N_BYTES_ACTION_COPY,
  PIC_ASSIGN_SHIFT_N_BYTES_ACTION_SWAP
} pic_assign_shift_n_bytes_action_t;

/* _multiply/_divide de-duplication cache: one array, owner = pic_op.c */
extern value_t pic_last_values[PIC_LAST_VALUE_CT];

/* defined once, called from another operator TU */
void pic_last_value_set(pic_last_value_t which, value_t n);
boolean_t pic_value_is_w(value_t val);
variable_sz_t pic_result_sz_get(value_t val1, value_t val2, 
    value_t dst);
flag_t pic_result_flag_get(pfile_t *pf, value_t val1, value_t val2);
boolean_t pic_result_is_signed(pfile_t *pf, value_t val1, 
    value_t val2);
value_t pic_var_temp_get_assign(pfile_t *pf, value_t val, value_t val2,
    value_t dst);
uchar pic_value_const_byte_get(const value_t val, unsigned ofs);
variable_sz_t pic_dynamic_index_byte_sz_get(value_t idx);
void pic_fsr0_add_dynamic_index(pfile_t *pf, value_t idx,
    value_t array_val);
void pic_classic_irp_set_from_w(pfile_t *pf);
boolean_t pic_variable_chunks_holed(variable_t var);
void pic_fsr_chunk_walk_emit_ofs(pfile_t *pf, variable_t var,
    value_t idx, size_t offset, boolean_t hybrid);
void pic_lookup_index_load_w(pfile_t *pf, value_t baseofs);
boolean_t pic_value_variable_is_array_of_pointers(const value_t val);
void pic_indirect_setup3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ofs, variable_sz_t *ipos);
void pic_indirect_bump3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ix, variable_sz_t *ipos);
void pic_value_sign_get_in_w(pfile_t *pf, value_t val);
value_t pic_value_sign_get(pfile_t *pf, value_t val, value_t accum);
boolean_t pic_val_bank_same(pfile_t *pf, value_t val1, value_t val2);
boolean_t pic_val_movl_bank_equal(pfile_t *pf,
    value_t v1, size_t ofs1, value_t v2, size_t ofs2);
void pic_value_sign_extend(pfile_t *pf, value_t val, 
    variable_sz_t pos, boolean_t is_signed);
void pic_assign_const(pfile_t *pf, value_t dst,
  value_t val1, value_t val2, variable_const_t n);
boolean_t pic_work_in_temp(pfile_t *pf, operator_t op,
  value_t dst, value_t val1, value_t val2, unsigned flags);
void pic_value_is_zero(pfile_t *pf, value_t src);
void pic_integer_to_float(pfile_t *pf, value_t src);
void pic_float_to_integer(pfile_t *pf, value_t src);
boolean_t pic_value_is_dynamic_bit_array_element(value_t val);
void pic_dynamic_bit_ref_addr_setup(pfile_t *pf,
    pic_dynamic_bit_ref_t *ref);
boolean_t pic_dynamic_bit_load_to_w(pfile_t *pf, value_t src);
void pic_assign_to_bit(pfile_t *pf, value_t dst, value_t src);
void pic_assign_from_bit(pfile_t *pf, value_t dst, value_t src);
void pic_assign_lookup_label_to_pointer(pfile_t *pf, value_t dst,
    label_t lbl, variable_const_t offset);
void pic_pointer_slot_add_emit(pfile_t *pf, value_t dst, value_t ofs);
void pic_assign_array_to_pointer(pfile_t *pf, value_t dst, value_t src);
void pic_assign_ptr_to_ptr(pfile_t *pf, value_t dst, value_t src);
void pic_assign_to_ptr(pfile_t *pf, value_t dst, value_t src);
void pic_assign_from_ptr(pfile_t *pf, value_t dst, value_t src);
void pic_assign_to_float(pfile_t *pf, value_t dst, value_t src);
void pic_assign_from_float(pfile_t *pf, value_t dst, value_t src);
boolean_t pic_variable_is_array_table(variable_t sv);
boolean_t pic_variable_is_rom_int_const_table(variable_t sv);
boolean_t pic_dst_backs_pointer_storage(value_t dst);
boolean_t pic_src_is_data_array(const value_t src);
boolean_t pic_try_src_arraylike_for_pointer_bind(const value_t src);
boolean_t pic_try_bind_array_to_pointer_slot(pfile_t *pf, value_t dst,
    value_t src);
void pic_op_assign(pfile_t *pf, operator_t op, value_t dst, 
    value_t src, value_t val2);
void pic_unary_fixup(pfile_t *pf, operator_t op,
    value_t dst, value_t val1, value_t val2);
void pic_add_sub_float(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_add_sub(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_mul(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_div_mod(pfile_t *pf, operator_t op, value_t dst, 
    value_t val1, value_t val2);
void pic_assign_logical_to_dst(pfile_t *pf, value_t dst, uchar n,
    unsigned flags);
void pic_value_to_w(pfile_t *pf, pic_value_to_w_t op,
    value_t val, value_t val_sign, variable_sz_t pos);
void pic_assign_from_w(pfile_t *pf, value_t dst, boolean_t sign_extend);
void pic_op_relational(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_logical_and(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_logical_or(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_binary_and(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_binary_or(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_binary_xor(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_cmp(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2);
void pic_op_neg(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2);
void pic_op_incr(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2);
void pic_op_decr(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2);
void pic_shift_left(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2, unsigned flags);
void pic_op_shift_left(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_shift_right(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2, unsigned flags);
void pic_op_shift_right(pfile_t *pf, operator_t op, value_t dst,
  value_t val1, value_t val2);
void pic_op_logical_or_not(pfile_t *pf, operator_t op, value_t dst,
  value_t src, value_t val2);

#endif /* pic_op_priv_h__ */

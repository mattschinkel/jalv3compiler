/************************************************************
 **
 ** value.h : value declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **           (c) 2024-2024, Rob Jansen
** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef value_h__
#define value_h__

#include "variable.h" 

/*typedef struct value_ *value_t;*/

#define VALUE_NONE ((value_t) 0)
#define VALUE_FLAG_NONE         0x0000U
#define VALUE_FLAG_TEMPSET      0x0001U
#define VALUE_FLAG_INDIRECT     0x0002U
#define VALUE_FLAG_CONST_TESTED 0x0004U
#define VALUE_FLAG_ARRAY        0x0008U /* before dereferencing, this
                                          was an array */ 
#define VALUE_FLAG_POINTER      0x0010U /* before dereferencing, this
                                          was a poitner */
#define VALUE_FLAG_PTR_SLOT_RAW 0x0020U /* read pointer slot storage bytes */
#define VALUE_FLAG_DEAD         0x0040U /* refcount reached 0: any use is a bug */

value_t   value_alloc(variable_t var);
void      value_lock(value_t val);
boolean_t value_is_same(const value_t val1, const value_t val2);

void     value_release(value_t val);
unsigned value_id_get(const value_t val);
value_t  value_clone(value_t src);

unsigned value_bit_offset_get(const value_t var);
void     value_bit_offset_delta_set(value_t var, unsigned delta);

/* test the variable definition flags */
boolean_t     value_dflag_test(const value_t val, flag_t flags);
/* test the variable flags */
boolean_t     value_vflag_test(const value_t val, flag_t flags);
/* test the value flags */
boolean_t     value_flag_test(const value_t val, flag_t flags);
void          value_flag_set(value_t val, flag_t flags);
void          value_flag_clr(value_t val, flag_t flags);
flag_t        value_flag_get_all(value_t val);
void          value_flag_set_all(value_t val, flag_t flags);

void          value_dereference(value_t val);

variable_t    value_variable_get(const value_t val);
void          value_variable_set(value_t val, variable_t var);
variable_sz_t value_sz_get(const value_t val);
variable_sz_t value_byte_sz_get(const value_t val);

unsigned      value_bit_pos_get(const value_t val);
unsigned      value_bit_size_get(const value_t val);

const char   *value_name_get(const value_t val);

value_t       value_baseofs_get(const value_t val);
void          value_baseofs_set(value_t val, value_t ofs);
boolean_t     value_baseofs_depends(const value_t val, const value_t dep);

variable_const_t value_const_get(const value_t val);
void             value_const_set(value_t val, variable_const_t c);

boolean_t        value_baseofs_const_get(const value_t val,
                    variable_const_t *dst);

float            value_const_float_get(const value_t val);
void             value_const_float_set(value_t val, float c);

void           value_assign_ct_bump(value_t var, ctr_bump_t dir);

ctr_t          value_use_ct_get(const value_t var);
void           value_use_ct_bump(value_t var, ctr_bump_t dir);

void value_dump(const value_t val, FILE *dst);
variable_base_t value_base_get(const value_t var);

boolean_t value_is_const(const value_t val);
boolean_t value_is_pseudo_const(const value_t val);
boolean_t value_is_indirect(const value_t val);
boolean_t value_is_signed(const value_t val);
boolean_t value_is_volatile(const value_t val);
boolean_t value_is_auto(const value_t val);
boolean_t value_is_array(const value_t val);
boolean_t value_is_pointer(const value_t val);
boolean_t value_is_boolean(const value_t val);
boolean_t value_is_function(const value_t val);
boolean_t value_is_label(const value_t val);
boolean_t value_is_value(const value_t val);
boolean_t value_is_shared(const value_t val);
boolean_t value_is_structure(const value_t val);
boolean_t value_is_float(const value_t val);
boolean_t value_def_is_same(const value_t val1, const value_t val2);

variable_def_t value_def_get(const value_t val);
void           value_def_set(value_t val, variable_def_t var);

void value_indirect_set(value_t val);
void value_indirect_clear(value_t val);


pfile_proc_t *value_proc_get(const value_t var);
label_t       value_label_get(const value_t val);

variable_def_type_t value_type_get(const value_t val);
boolean_t value_is_number(const value_t val);
boolean_t value_is_bit(const value_t val);
boolean_t value_is_single_bit(const value_t val);
boolean_t value_is_multi_bit(const value_t val);
boolean_t value_is_lookup(const value_t val);
boolean_t value_is_temp(const value_t val);
boolean_t value_is_used(const value_t val);
boolean_t value_is_universal(const value_t val);
boolean_t value_is_string(const value_t val);

variable_ct_t value_ct_get(const value_t val);
/* like value_baseofs_set, but subscript deals with value size */
value_t value_subscript_set(value_t val, variable_ct_t subscript);
value_t value_constant_get(variable_const_t n, variable_def_t def);
value_t value_constant_float_get(float n, variable_def_t def);

typedef struct {
  value_t old;
  value_t new;
} value_map_pair_t;

typedef struct {
  size_t            used;
  size_t            alloc;
  value_map_pair_t *map;
} value_map_t;

value_t value_map_find(const value_map_t *map, value_t val);

value_t value_variable_remap(value_t val, const variable_map_t *map);


/*
 * OWNERSHIP CONTRACT
 * ------------------
 * value_t is a handle with a release contract, and the convention is the
 * only thing that enforces it.
 *
 *  - A function that HANDS OUT a value transfers a reference: value_alloc,
 *    value_clone, value_constant_get, pfile_constant_get, pfile_value_find,
 *    pfile_proc_value_temp_get*, jal_parse_expr, jal_token_to_constant, ...
 *    The receiver releases it.
 *
 *  - A function that STORES or CONSUMES a value takes its OWN reference and
 *    the caller still releases: pfile_cmd_op_add, value_baseofs_set,
 *    value_variable_set, pic_code_value_set, expr_val_set, ...
 *    (This asymmetry -- stores lock, callers must release -- produced three
 *    leaks of the MOVLB family, all fixed 2026-10-31.)
 *
 *  - A function that takes a value_t BY VALUE and releases that parameter
 *    has CONSUMED it, and nothing in the signature says so. That ambiguity
 *    produced a double release in pic_eeprom_or_id_dump() vs pic_cmd_dump()
 *    (21 corpus samples, hidden for years because a freed cache element
 *    failed to resolve).
 *
 * RULE for new code: do NOT consume a value_t parameter. Release at the
 * caller, or take `value_t *` and clear the caller's handle so the transfer
 * is visible. Existing consuming functions are listed by
 *     unit-tests/value_ownership_scan.py --cond
 * which is a review aid (brace/depth heuristic), not a gate.
 *
 * Lifetime note: values are region-scoped (see value_arena_*), so a missing
 * release no longer leaks MEMORY -- but the reference still owns the
 * value's `variable`, so getting it wrong still corrupts refcounts.
 */
/* Values allocated since the region was last destroyed. The region is
 * compile-scoped, so this is 0 once pfile_close() has run -- an invariant,
 * not a leak counter. */
unsigned long value_live_ct(void);
void          value_arena_begin(void);
unsigned long value_arena_destroy(void);

#endif /* value_h__ */


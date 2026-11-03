/************************************************************
 **
 ** pic_op.h : PIC operator generation declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ************************************************************/
#ifndef pic_op_h__
#define pic_op_h__

#include "pic.h"

#define SWAP(type, a, b) \
   { type tmp__; tmp__ = a; a = b; b = tmp__; }


/* operator --> pic instructions */
void pic_op(pfile_t *pf, operator_t op, value_t dst, value_t val1,
    value_t val2);
void pic_indirect_setup(pfile_t *pf, value_t val, size_t offset);
/* FSR = array/variable base + runtime byte index (classic cores) */
void pic_classic_fsr_load_base_dynamic_index(pfile_t *pf,
    variable_t var, variable_base_t base, value_t idx,
    variable_sz_t idx_range);

void pic_last_values_reset(void);

/* A pointer VALUE held in a scratch slot is zero -> defined trap (never a
 * bogus FSR / call through address 0).  Used by runtime-index pointer-slot
 * loads and indirect calls. */
void pic_pointer_value_emit_null_guard(pfile_t *pf, value_t ptr);

operator_t pic_relational(pfile_t *pf, operator_t op, value_t dst, 
    value_t val1, value_t val2);

value_t pic_cast(value_t src, variable_sz_t sz);

#endif /* pic_op_h__ */


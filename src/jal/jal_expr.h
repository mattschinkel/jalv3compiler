/************************************************************
 **
 ** jal_expr.h : JAL expression parsing declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef jal_expr_h__
#define jal_expr_h__

#include "../libcore/pfile.h"
#include "jal_vdef.h"

value_t jal_parse_expr(pfile_t *pf);

typedef enum {
  JAL_PARSE_VALUE_TYPE_ANY,
  JAL_PARSE_VALUE_TYPE_LVALUE,
  /* Like LVALUE but keep pseudo `'get` in jvals for `pointer is pseudo` RHS. */
  JAL_PARSE_VALUE_TYPE_LVALUE_FOR_POINTER_IS,
  JAL_PARSE_VALUE_TYPE_RVALUE
} jal_parse_value_type_t;

value_t   jal_parse_value(pfile_t *pf, jal_parse_value_type_t type);
boolean_t jal_parse_structure(pfile_t *pf, value_t *pval);
/* opt_locked_jvals: pass jvals[] from jal_parse_value after value_lock (two releases on
 * fatal parse); NULL elsewhere (single release). */
boolean_t jal_parse_subscript(pfile_t *pf, value_t *pval,
    value_t opt_locked_jvals[JAL_VAL_TYPE_CT]);

#define JAL_PARSE_POSTFIX_FLAG_STRUCTURE 0x01
#define JAL_PARSE_POSTFIX_FLAG_SUBSCRIPT  0x02

/* add an element offset to a value's baseofs (const fold or runtime ADD) */
void   jal_val_baseofs_add(pfile_t *pf, value_t val, value_t ofs);

flag_t jal_parse_postfix(pfile_t *pf, value_t *pval,
    value_t opt_locked_jvals[JAL_VAL_TYPE_CT]);

#endif /* jal_expr_h__ */


